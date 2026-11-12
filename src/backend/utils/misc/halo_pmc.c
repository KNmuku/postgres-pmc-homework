/*-------------------------------------------------------------------------
 * halo_pmc.c
 *   CPU PMC observation for an ExecutorRun interval in one backend thread.
 *
 * Kernel perf_event_open owns/schedules counters. RDPMC is only executed
 * with a kernel-provided mmap permission and a live hardware index.
 *-------------------------------------------------------------------------
 */
#ifndef HALO_PMC_STANDALONE
#include "postgres.h"
#endif

#include <errno.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>
#if defined(__x86_64__) && defined(__GNUC__)
#include <cpuid.h>
#endif

#include "utils/halo_pmc.h"

#ifdef __linux__
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#endif

#ifndef HALO_PMC_STANDALONE
#include "catalog/pg_type_d.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "executor/spi.h"
#include "storage/ipc.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/json.h"
#include "utils/lsyscache.h"
#include "utils/varlena.h"
#endif

/* Linux perf ABI encodings, also usable when building the portable stubs. */
static const HaloPmcEvent default_events[] = {
	{"instructions", 0, 1, 0, 0},
	{"cycles", 0, 0, 0, 0},
	{"perf_count_hw_cache_l1d_rd", 3, 0, 0, 0},
	{"perf_count_hw_cache_l1d_miss_rd", 3, UINT64_C(1) << 16, 0, 0}
};

const char *
halo_pmc_cpu_vendor(unsigned int *family, unsigned int *model)
{
	static char vendor[13] = "unknown";

	*family = *model = 0;
#if defined(__x86_64__) && defined(__GNUC__)
	{
		unsigned int a, b, c, d, base;

		if (!__get_cpuid(0, &a, &b, &c, &d))
			return vendor;
		memcpy(vendor, &b, 4);
		memcpy(vendor + 4, &d, 4);
		memcpy(vendor + 8, &c, 4);
		vendor[12] = '\0';
		if (!__get_cpuid(1, &a, &b, &c, &d))
			return vendor;
		base = (a >> 8) & 15;
		*family = base + (base == 15 ? ((a >> 20) & 255) : 0);
		*model = ((a >> 4) & 15) +
			((base == 6 || base == 15) ? (((a >> 16) & 15) << 4) : 0);
	}
#endif
	return vendor;
}

bool
halo_pmc_scaled(uint64_t raw, uint64_t enabled, uint64_t running, double *value)
{
	if (value == NULL || running == 0 || enabled == 0 || running > enabled)
		return false;
	*value = (double) ((long double) raw * enabled / running);
	return true;
}

/* Unsigned arithmetic avoids undefined signed left shifts. */
uint64_t
halo_pmc_sign_extend(uint64_t value, unsigned int width)
{
	uint64_t	sign;
	uint64_t	mask;

	if (width == 0 || width >= 64)
		return value;
	sign = UINT64_C(1) << (width - 1);
	mask = (UINT64_C(1) << width) - 1;
	return ((value & mask) ^ sign) - sign;
}

void
halo_pmc_close(HaloPmcContext *ctx)
{
	int			i;

	for (i = 0; i < HALO_PMC_EVENTS; i++)
	{
#ifdef __linux__
		if (ctx->page[i] != NULL)
			munmap(ctx->page[i], ctx->page_size);
#endif
		if (ctx->fd[i] >= 0 && (ctx->valid & (1U << i)))
			close(ctx->fd[i]);
		ctx->fd[i] = -1;
		ctx->page[i] = NULL;
	}
	ctx->active = false;
	ctx->valid = 0;
}

bool
halo_pmc_open(HaloPmcContext *ctx)
{
	return halo_pmc_open_config(ctx, default_events,
							   sizeof(default_events) / sizeof(default_events[0]));
}

bool
halo_pmc_open_config(HaloPmcContext *ctx, const HaloPmcEvent *events,
					 unsigned int count)
{
	int			i;

	memset(ctx, 0, sizeof(*ctx));
	for (i = 0; i < HALO_PMC_EVENTS; i++)
		ctx->fd[i] = -1;
	if (count < 2 || count > HALO_PMC_EVENTS)
	{
		ctx->last_errno = EINVAL;
		return false;
	}
	ctx->event_count = count;
	memcpy(ctx->events, events, count * sizeof(*events));
	ctx->owner_pid = (int) getpid();
#ifdef __linux__
	ctx->page_size = (size_t) sysconf(_SC_PAGESIZE);
	for (i = 0; i < (int) count; i++)
	{
		struct perf_event_attr attr;
		void	   *page;

		memset(&attr, 0, sizeof(attr));
		attr.size = sizeof(attr);
		attr.type = events[i].type;
		attr.config = events[i].config;
		attr.config1 = events[i].config1;
		attr.config2 = events[i].config2;
		attr.disabled = (i == 0);
		attr.exclude_kernel = 1;
		attr.exclude_hv = 1;
		attr.read_format = PERF_FORMAT_GROUP | PERF_FORMAT_ID |
			PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
		ctx->fd[i] = (int) syscall(SYS_perf_event_open, &attr, 0, -1,
									i == 0 ? -1 : ctx->fd[0], PERF_FLAG_FD_CLOEXEC);
		if (ctx->fd[i] < 0)
		{
			/* Instructions/cycles are required; cache events are optional. */
			if (i < 2)
				goto failed;
			continue;
		}
		ctx->valid |= 1U << i;
		if (ioctl(ctx->fd[i], PERF_EVENT_IOC_ID, &ctx->id[i]) < 0)
			goto failed;
		page = mmap(NULL, ctx->page_size, PROT_READ, MAP_SHARED, ctx->fd[i], 0);
		if (page != MAP_FAILED)
			ctx->page[i] = page;
	}
	return true;
failed:
	ctx->last_errno = errno;
	halo_pmc_close(ctx);
	return false;
#else
	ctx->last_errno = ENOSYS;
	return false;
#endif
}

static bool
halo_pmc_read_system(HaloPmcContext *ctx, HaloPmcSnapshot *snapshot)
{
#ifdef __linux__
	struct
	{
		uint64_t	nr;
		uint64_t	enabled;
		uint64_t	running;
		struct { uint64_t value; uint64_t id; } item[HALO_PMC_EVENTS];
	} buf;
	ssize_t		n;
	unsigned int seen = 0;
	uint64_t	i;
	int			j;

	memset(snapshot, 0, sizeof(*snapshot));
	if (ctx->owner_pid != (int) getpid() || ctx->fd[0] < 0)
	{
		ctx->last_errno = EBADF;
		return false;
	}
	do
	{
		n = read(ctx->fd[0], &buf, sizeof(buf));
	} while (n < 0 && errno == EINTR);
	ctx->read_calls++;
	if (n < (ssize_t) (3 * sizeof(uint64_t)) || buf.nr > HALO_PMC_EVENTS ||
		n != (ssize_t) ((3 + 2 * buf.nr) * sizeof(uint64_t)))
	{
		ctx->last_errno = n < 0 ? errno : EIO;
		return false;
	}
	for (i = 0; i < buf.nr; i++)
	{
		for (j = 0; j < HALO_PMC_EVENTS; j++)
			if ((ctx->valid & (1U << j)) && buf.item[i].id == ctx->id[j])
			{
				snapshot->value[j] = buf.item[i].value;
				seen |= 1U << j;
			}
	}
	if (seen != ctx->valid)
	{
		ctx->last_errno = EIO;
		return false;
	}
	snapshot->valid = seen;
	snapshot->time_enabled = buf.enabled;
	snapshot->time_running = buf.running;
	return true;
#else
	(void) snapshot;
	ctx->last_errno = ENOSYS;
	return false;
#endif
}

bool
halo_pmc_read_fast(HaloPmcContext *ctx, HaloPmcSnapshot *snapshot)
{
#if defined(__linux__) && defined(__x86_64__) && defined(__GNUC__)
	int			i;

	memset(snapshot, 0, sizeof(*snapshot));
	if (!ctx->active)
		return false;
	for (i = 0; i < HALO_PMC_EVENTS; i++)
	{
		struct perf_event_mmap_page *page = ctx->page[i];
		bool		ok = false;
		int			retry;

		if (!(ctx->valid & (1U << i)))
			continue;
		if (page == NULL)
			return false;
		for (retry = 0; retry < 64; retry++)
		{
			uint32_t	seq;
			uint32_t	index;
			unsigned int width;
			uint64_t	offset;
			uint64_t	value;
			uint32_t	lo;
			uint32_t	hi;

			seq = __atomic_load_n(&page->lock, __ATOMIC_ACQUIRE);
			if (seq & 1)
				continue;
			/* Every field must be read inside the mmap seqlock interval. */
			__asm__ volatile("" ::: "memory");
			if (!page->cap_bit0_is_deprecated || !page->cap_user_rdpmc)
				return false;
			index = page->index;
			width = page->pmc_width;
			offset = (uint64_t) page->offset;
			if (index == 0 || width == 0 || width > 64)
				return false;
			/* Serialize both measured code and the counter read itself. */
			__asm__ volatile("lfence\n\trdpmc\n\tlfence"
							 : "=a" (lo), "=d" (hi) : "c" (index - 1) : "memory");
			value = ((uint64_t) hi << 32) | lo;
			value = offset + halo_pmc_sign_extend(value, width);
			__asm__ volatile("" ::: "memory");
			if (__atomic_load_n(&page->lock, __ATOMIC_ACQUIRE) == seq)
			{
				snapshot->value[i] = value;
				ok = true;
				break;
			}
		}
		if (!ok)
			return false;
	}
	snapshot->valid = ctx->valid;
	ctx->rdpmc_calls++;
	return true;
#else
	(void) ctx;
	(void) snapshot;
	return false;
#endif
}

bool
halo_pmc_read(HaloPmcContext *ctx, HaloPmcReadMethod method,
			  HaloPmcSnapshot *snapshot)
{
	if (method != HALO_PMC_READ_SYSCALL && halo_pmc_read_fast(ctx, snapshot))
		return true;
	if (method == HALO_PMC_READ_RDPMC)
	{
		ctx->last_errno = ENOTSUP;
		return false;
	}
	return halo_pmc_read_system(ctx, snapshot);
}

bool
halo_pmc_start(HaloPmcContext *ctx)
{
#ifdef __linux__
	ctx->read_calls = ctx->rdpmc_calls = 0;
	/* Keep cumulative counters. RESET would not reset enabled/running time. */
	if (!halo_pmc_read_system(ctx, &ctx->baseline))
		return false;
	if (ioctl(ctx->fd[0], PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP) < 0)
	{
		ctx->last_errno = errno;
		return false;
	}
	ctx->active = true;
	return true;
#else
	ctx->last_errno = ENOSYS;
	return false;
#endif
}

void
halo_pmc_cancel(HaloPmcContext *ctx)
{
#ifdef __linux__
	if (ctx->active)
		(void) ioctl(ctx->fd[0], PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP);
#endif
	ctx->active = false;
}

bool
halo_pmc_stop(HaloPmcContext *ctx, HaloPmcSnapshot *delta)
{
#ifdef __linux__
	HaloPmcSnapshot end;
	int			i;

	if (!ctx->active)
	{
		ctx->last_errno = EINVAL;
		return false;
	}
	ctx->active = false;
	if (ioctl(ctx->fd[0], PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP) < 0)
	{
		ctx->last_errno = errno;
		return false;
	}
	if (!halo_pmc_read_system(ctx, &end))
		return false;
	memset(delta, 0, sizeof(*delta));
	delta->valid = end.valid;
	delta->time_enabled = end.time_enabled - ctx->baseline.time_enabled;
	delta->time_running = end.time_running - ctx->baseline.time_running;
	for (i = 0; i < HALO_PMC_EVENTS; i++)
		delta->value[i] = end.value[i] - ctx->baseline.value[i];
	if (delta->time_running == 0 || delta->time_running > delta->time_enabled)
	{
		ctx->last_errno = ERANGE;
		return false;
	}
	return true;
#else
	(void) delta;
	ctx->last_errno = ENOSYS;
	return false;
#endif
}

#ifndef HALO_PMC_STANDALONE
/* PostgreSQL integration: all state below belongs to a single child backend. */
bool		halo_pmc_enabled = false;
bool		halo_pmc_sampling = false;
bool		halo_pmc_loading = false;
int			halo_pmc_target = HALO_PMC_TARGET_HASH;
static int	sample_every = 0;
static int	read_method = HALO_PMC_READ_AUTO;
static int	query_depth = 0;
static int	query_sample_every;
static int	query_read_method;
static bool initialized;
static bool attempted;
static bool loop_previous_valid;
static bool segment_selected;
static bool events_changed = true;
static char *event_list;
static char *legacy_event_list;
static char *watch_event_list;
static char selected_list[1024];
static const char *query_scope;
static HaloPmcEvent selected_events[HALO_PMC_EVENTS];
static unsigned int selected_count;
static uint64_t segment_values[HALO_PMC_EVENTS];
static uint64_t sample_countdown;
static uint64_t samples;
static uint64_t skipped_samples;
static uint64_t largest_sample_cycles;
static HaloPmcContext backend_pmc;
static HaloPmcSnapshot loop_previous;

static bool
halo_pmc_check_list(char **newval, void **extra, GucSource source)
{
	(void) extra;
	(void) source;
	if (*newval != NULL && strlen(*newval) >= sizeof(selected_list))
	{
		GUC_check_errdetail("The PMC event list must contain fewer than 1024 bytes.");
		return false;
	}
	return true;
}

static void
halo_pmc_assign_list(const char *newval, void *extra)
{
	(void) extra;
	strlcpy(selected_list, newval != NULL ? newval : "", sizeof(selected_list));
	events_changed = true;
}

/* Table and event names are resolved outside the measured region. */
static void
halo_pmc_load_events(void)
{
	char	   *raw;
	List	   *names;
	ListCell   *cell;
	unsigned int family, model;
	const char *vendor = halo_pmc_cpu_vendor(&family, &model);

	selected_count = 2;
	memcpy(selected_events, default_events, 2 * sizeof(HaloPmcEvent));
	if (selected_list[0] == '\0')
	{
		selected_count = sizeof(default_events) / sizeof(default_events[0]);
		memcpy(selected_events, default_events, sizeof(default_events));
		return;
	}
	raw = pstrdup(selected_list);
	if (!SplitIdentifierString(raw, ',', &names) || names == NIL)
		ereport(ERROR, (errmsg("invalid halo_pmc event list")));
	halo_pmc_loading = true;
	if (SPI_connect() != SPI_OK_CONNECT)
	{
		halo_pmc_loading = false;
		elog(ERROR, "could not connect SPI for halo_pmc initialization");
	}
	PG_TRY();
	{
		foreach(cell, names)
		{
			char	   *name = lfirst(cell);
			Oid			types[1] = {TEXTOID};
			Datum		args[1];
			HeapTuple	tuple;
			TupleDesc	desc;
			bool		isnull;
			Datum		value;
			HaloPmcEvent event;
			char	   *row_vendor;
			int			row_family, row_model;
			unsigned int i;
			bool		duplicate = false;

			for (i = 0; i < selected_count; i++)
				if (pg_strcasecmp(name, selected_events[i].name) == 0)
					duplicate = true;
			if (duplicate)
				continue;
			if (selected_count == HALO_PMC_EVENTS)
				ereport(ERROR, (errmsg("at most %d PMC events including instructions and cycles are supported", HALO_PMC_EVENTS)));
			args[0] = CStringGetTextDatum(name);
			if (SPI_execute_with_args(
					"SELECT name, pmctype, pe_config, config1, config2, "
					"cpu_vendor, cpu_family, cpu_model FROM public.pmc_name "
					"WHERE lower(name) = lower($1)",
					1, types, args, NULL, true, 2) != SPI_OK_SELECT || SPI_processed != 1)
				ereport(ERROR,
						(errmsg("PMC event \"%s\" is not uniquely defined in public.pmc_name", name),
						 errhint("Run homework/pmc/sql/init.sql first, then choose a defined event.")));
			tuple = SPI_tuptable->vals[0];
			desc = SPI_tuptable->tupdesc;
			memset(&event, 0, sizeof(event));
			value = SPI_getbinval(tuple, desc, 1, &isnull);
			if (isnull)
				elog(ERROR, "NULL PMC name");
			strlcpy(event.name, TextDatumGetCString(value), sizeof(event.name));
			value = SPI_getbinval(tuple, desc, 2, &isnull);
			if (isnull || DatumGetInt32(value) < 0)
				elog(ERROR, "invalid PMC type");
			event.type = (uint32_t) DatumGetInt32(value);
			value = SPI_getbinval(tuple, desc, 3, &isnull);
			if (isnull)
				elog(ERROR, "NULL PMC config");
			event.config = (uint64_t) DatumGetInt64(value);
			value = SPI_getbinval(tuple, desc, 4, &isnull);
			event.config1 = isnull ? 0 : (uint64_t) DatumGetInt64(value);
			value = SPI_getbinval(tuple, desc, 5, &isnull);
			event.config2 = isnull ? 0 : (uint64_t) DatumGetInt64(value);
			value = SPI_getbinval(tuple, desc, 6, &isnull);
			row_vendor = isnull ? "any" : TextDatumGetCString(value);
			value = SPI_getbinval(tuple, desc, 7, &isnull);
			row_family = isnull ? 0 : DatumGetInt32(value);
			value = SPI_getbinval(tuple, desc, 8, &isnull);
			row_model = isnull ? 0 : DatumGetInt32(value);
			if ((strcmp(row_vendor, "any") != 0 && strcmp(row_vendor, vendor) != 0) ||
				(row_family != 0 && row_family != (int) family) ||
				(row_model != 0 && row_model != (int) model))
				ereport(ERROR,
						(errmsg("PMC event \"%s\" is not defined for CPU %s family %u model %u", name, vendor, family, model),
						 errhint("Choose a matching CPU event; Intel retired-store and AMD dispatched-store events have different meanings.")));
			selected_events[selected_count++] = event;
		}
	}
	PG_FINALLY();
	{
		SPI_finish();
		halo_pmc_loading = false;
	}
	PG_END_TRY();
	list_free(names);
	pfree(raw);
}

static void
halo_pmc_backend_exit(int code, Datum arg)
{
	(void) code;
	(void) arg;
	halo_pmc_close(&backend_pmc);
}

static void
halo_pmc_assign_enabled(bool newval, void *extra)
{
	(void) extra;
	/* An explicit re-enable permits retry after an earlier open failure. */
	if (newval && !backend_pmc.active && backend_pmc.valid == 0)
		attempted = false;
}

void
halo_pmc_init(void)
{
	static const struct config_enum_entry methods[] = {
		{"auto", HALO_PMC_READ_AUTO, false},
		{"read", HALO_PMC_READ_SYSCALL, false},
		{"rdpmc", HALO_PMC_READ_RDPMC, false},
		{NULL, 0, false}
	};
	static const struct config_enum_entry targets[] = {
		{"executor", HALO_PMC_TARGET_EXECUTOR, false},
		{"hash", HALO_PMC_TARGET_HASH, false},
		{NULL, 0, false}
	};
	int			i;

	if (initialized)
		return;
	for (i = 0; i < HALO_PMC_EVENTS; i++)
		backend_pmc.fd[i] = -1;
	DefineCustomBoolVariable("halo_pmc.enabled",
							 "Observe user-mode CPU performance counters.",
							 NULL, &halo_pmc_enabled, false, PGC_SUSET, 0,
							 NULL, halo_pmc_assign_enabled, NULL);
	DefineCustomIntVariable("halo_pmc.sample_every",
							"Observe every N target-code segments (0 disables loop snapshots).",
							NULL, &sample_every, 0, 0, INT_MAX, PGC_SUSET, 0,
							NULL, NULL, NULL);
	DefineCustomEnumVariable("halo_pmc.read_method",
							 "Loop snapshot method: auto, read, or rdpmc.",
							 NULL, &read_method, HALO_PMC_READ_AUTO, methods,
							 PGC_SUSET, 0, NULL, NULL, NULL);
	DefineCustomEnumVariable("halo_pmc.target", "Code region to observe.", NULL,
							 &halo_pmc_target, HALO_PMC_TARGET_HASH, targets,
							 PGC_SUSET, 0, NULL, NULL, NULL);
	DefineCustomStringVariable("halo_pmc.events", "Names from public.pmc_name.", NULL,
							   &event_list, "", PGC_SUSET, GUC_LIST_INPUT,
							   halo_pmc_check_list, halo_pmc_assign_list, NULL);
	/* Course PPT uses both spellings. They are session-only aliases. */
	DefineCustomStringVariable("halo_pmc_list", "Course alias for halo_pmc.events.", NULL,
							   &legacy_event_list, "", PGC_SUSET, GUC_LIST_INPUT,
							   halo_pmc_check_list, halo_pmc_assign_list, NULL);
	DefineCustomStringVariable("halo_watch_pmc_list", "Course alias for halo_pmc.events.", NULL,
							   &watch_event_list, "", PGC_SUSET, GUC_LIST_INPUT,
							   halo_pmc_check_list, halo_pmc_assign_list, NULL);
	/* Alias defaults must not override a real startup placeholder value. */
	if (event_list != NULL && event_list[0] != '\0')
		halo_pmc_assign_list(event_list, NULL);
	MarkGUCPrefixReserved("halo_pmc");
	on_proc_exit(halo_pmc_backend_exit, (Datum) 0);
	initialized = true;
}

int
halo_pmc_query_begin(const char *scope)
{
	int			depth = query_depth++;

	halo_pmc_sampling = false;
	if (depth > 0)
		return depth;
	query_scope = scope;
	if (events_changed)
	{
		halo_pmc_close(&backend_pmc);
		halo_pmc_load_events();
		attempted = false;
		events_changed = false;
	}
	if (!attempted)
	{
		attempted = true;
		if (!halo_pmc_open_config(&backend_pmc, selected_events, selected_count))
			ereport(NOTICE,
					(errmsg("halo_pmc unavailable: perf_event_open: %s (errno=%d)",
							strerror(backend_pmc.last_errno), backend_pmc.last_errno),
					 errhint("Check PMU support and perf permissions; queries continue normally.")));
	}
	if (backend_pmc.valid == 0)
		return depth;
	if (!halo_pmc_start(&backend_pmc))
	{
		ereport(NOTICE,
				(errmsg("halo_pmc unavailable: counter start: %s",
						strerror(backend_pmc.last_errno))));
		halo_pmc_close(&backend_pmc);
		return depth;
	}
	query_sample_every = sample_every;
	query_read_method = read_method;
	sample_countdown = (uint64_t) sample_every;
	samples = skipped_samples = largest_sample_cycles = 0;
	loop_previous_valid = false;
	segment_selected = false;
	memset(segment_values, 0, sizeof(segment_values));
	halo_pmc_sampling = sample_every > 0;
	return depth;
}

void
halo_pmc_segment_begin(void)
{
	segment_selected = false;
	if (--sample_countdown != 0)
		return;
	sample_countdown = (uint64_t) query_sample_every;
	segment_selected = true;
	loop_previous_valid = halo_pmc_read(&backend_pmc,
										 (HaloPmcReadMethod) query_read_method,
										 &loop_previous);
}

void
halo_pmc_segment_end(void)
{
	HaloPmcSnapshot current;
	int			i;

	if (!segment_selected)
		return;
	segment_selected = false;
	if (halo_pmc_read(&backend_pmc, (HaloPmcReadMethod) query_read_method, &current))
	{
		if (loop_previous_valid)
		{
			uint64_t	cycles = current.value[1] - loop_previous.value[1];

			if (cycles > largest_sample_cycles)
				largest_sample_cycles = cycles;
			for (i = 0; i < (int) backend_pmc.event_count; i++)
				segment_values[i] += current.value[i] - loop_previous.value[i];
			samples++;
		}
		else
			skipped_samples++;
	}
	else
	{
		loop_previous_valid = false;
		skipped_samples++;
	}
}

void
halo_pmc_query_abort(int depth)
{
	/* Do not allocate, log, or replace the original PostgreSQL error. */
	halo_pmc_cancel(&backend_pmc);
	query_depth = depth;
	halo_pmc_sampling = false;
	loop_previous_valid = false;
}

void
halo_pmc_query_end(int depth, uint64_t rows)
{
	HaloPmcSnapshot result;
	double		values[HALO_PMC_EVENTS];
	StringInfoData output;
	int			i;
	int			access_index = -1, miss_index = -1;

	query_depth = depth;
	if (depth > 0)
	{
		halo_pmc_sampling = depth == 1 && backend_pmc.active && query_sample_every > 0;
		return;
	}
	halo_pmc_sampling = false;
	if (!backend_pmc.active)
		return;
	if (!halo_pmc_stop(&backend_pmc, &result))
	{
		ereport(NOTICE,
				(errmsg("halo_pmc unavailable: counter stop/read: %s",
						strerror(backend_pmc.last_errno))));
		/* Avoid retaining an enabled group after an ioctl/read failure. */
		halo_pmc_close(&backend_pmc);
		return;
	}
	initStringInfo(&output);
	appendStringInfo(&output, "{\"scope\":\"%s\",\"rows\":%llu",
					 query_scope, (unsigned long long) rows);
	for (i = 0; i < (int) backend_pmc.event_count; i++)
	{
		appendStringInfoChar(&output, ',');
		escape_json(&output, backend_pmc.events[i].name);
		appendStringInfoChar(&output, ':');
		if ((result.valid & (1U << i)) &&
			halo_pmc_scaled(result.value[i], result.time_enabled,
							result.time_running, &values[i]))
			appendStringInfo(&output, "%.3f", values[i]);
		else
		{
			values[i] = -1;
			appendStringInfoString(&output, "null");
		}
		if (backend_pmc.events[i].type == 3 && backend_pmc.events[i].config == 0)
			access_index = i;
		if (backend_pmc.events[i].type == 3 && backend_pmc.events[i].config == (UINT64_C(1) << 16))
			miss_index = i;
	}
	appendStringInfoString(&output, ",\"l1d_hit_rate_estimate_percent\":");
	if (access_index >= 0 && miss_index >= 0 && values[access_index] > 0 &&
		values[miss_index] >= 0 && values[miss_index] <= values[access_index])
		appendStringInfo(&output, "%.3f", 100.0 * (1.0 - values[miss_index] / values[access_index]));
	else
		appendStringInfoString(&output, "null");
	appendStringInfoString(&output, ",\"segment_raw_counts\":{");
	for (i = 0; i < (int) backend_pmc.event_count; i++)
	{
		if (i > 0)
			appendStringInfoChar(&output, ',');
		escape_json(&output, backend_pmc.events[i].name);
		appendStringInfoChar(&output, ':');
		if (samples > 0 && (result.valid & (1U << i)))
			appendStringInfo(&output, "%llu", (unsigned long long) segment_values[i]);
		else
			appendStringInfoString(&output, "null");
	}
	appendStringInfoString(&output, "}");
	appendStringInfo(&output,
					 ",\"time_enabled_ns\":%llu,\"time_running_ns\":%llu"
					 ",\"loop_samples\":%llu,\"skipped_samples\":%llu"
					 ",\"max_sample_raw_cycles\":%llu"
					 ",\"read_calls\":%llu,\"rdpmc_calls\":%llu}",
					 (unsigned long long) result.time_enabled,
					 (unsigned long long) result.time_running,
					 (unsigned long long) samples,
					 (unsigned long long) skipped_samples,
					 (unsigned long long) largest_sample_cycles,
					 (unsigned long long) backend_pmc.read_calls,
					 (unsigned long long) backend_pmc.rdpmc_calls);
	/* Formatting and client output happen after the group is disabled. */
	ereport(NOTICE, (errmsg_internal("halo_pmc %s", output.data)));
	pfree(output.data);
}
#endif							/* !HALO_PMC_STANDALONE */
