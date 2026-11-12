/*-------------------------------------------------------------------------
 * halo_pmc.h
 *   Per-backend Linux performance counters and optional x86 RDPMC snapshots.
 *-------------------------------------------------------------------------
 */
#ifndef HALO_PMC_H
#define HALO_PMC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HALO_PMC_EVENTS 8

typedef struct HaloPmcEvent
{
	char		name[96];
	uint32_t	type;
	uint64_t	config;
	uint64_t	config1;
	uint64_t	config2;
} HaloPmcEvent;

typedef enum HaloPmcReadMethod
{
	HALO_PMC_READ_AUTO,
	HALO_PMC_READ_SYSCALL,
	HALO_PMC_READ_RDPMC
} HaloPmcReadMethod;

typedef struct HaloPmcSnapshot
{
	uint64_t value[HALO_PMC_EVENTS];
	uint64_t time_enabled;
	uint64_t time_running;
	unsigned int valid;
} HaloPmcSnapshot;

typedef struct HaloPmcContext
{
	int			fd[HALO_PMC_EVENTS];
	uint64_t	id[HALO_PMC_EVENTS];
	void	   *page[HALO_PMC_EVENTS];
	size_t		page_size;
	unsigned int valid;
	int			owner_pid;
	int			last_errno;
	bool		active;
	uint64_t	read_calls;
	uint64_t	rdpmc_calls;
	HaloPmcSnapshot baseline;
	unsigned int event_count;
	HaloPmcEvent events[HALO_PMC_EVENTS];
} HaloPmcContext;

extern bool halo_pmc_open(HaloPmcContext *ctx);
extern bool halo_pmc_open_config(HaloPmcContext *ctx, const HaloPmcEvent *events,
								unsigned int count);
extern const char *halo_pmc_cpu_vendor(unsigned int *family, unsigned int *model);
extern void halo_pmc_close(HaloPmcContext *ctx);
extern bool halo_pmc_start(HaloPmcContext *ctx);
extern bool halo_pmc_stop(HaloPmcContext *ctx, HaloPmcSnapshot *delta);
extern void halo_pmc_cancel(HaloPmcContext *ctx);
extern bool halo_pmc_read(HaloPmcContext *ctx, HaloPmcReadMethod method,
						  HaloPmcSnapshot *snapshot);
/* RDPMC snapshots are raw counts. Only stop() supplies interval time deltas. */
extern bool halo_pmc_read_fast(HaloPmcContext *ctx, HaloPmcSnapshot *snapshot);
extern bool halo_pmc_scaled(uint64_t raw, uint64_t enabled, uint64_t running,
							double *value);
extern uint64_t halo_pmc_sign_extend(uint64_t value, unsigned int width);

#ifndef HALO_PMC_STANDALONE
extern PGDLLIMPORT bool halo_pmc_enabled;
extern PGDLLIMPORT bool halo_pmc_sampling;
extern PGDLLIMPORT bool halo_pmc_loading;
extern PGDLLIMPORT int halo_pmc_target;
#define HALO_PMC_TARGET_EXECUTOR 0
#define HALO_PMC_TARGET_HASH 1
extern void halo_pmc_init(void);
extern int halo_pmc_query_begin(const char *scope);
extern void halo_pmc_query_end(int depth, uint64_t rows);
extern void halo_pmc_query_abort(int depth);
extern void halo_pmc_segment_begin(void);
extern void halo_pmc_segment_end(void);
#endif

#endif							/* HALO_PMC_H */
