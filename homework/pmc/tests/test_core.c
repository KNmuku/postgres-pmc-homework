#include "utils/halo_pmc.h"
#include <assert.h>
#include <errno.h>
#include <linux/perf_event.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

long __real_syscall(long number, ...);
ssize_t __real_read(int fd, void *buffer, size_t count);
void *__real_mmap(void *address, size_t length, int prot, int flags, int fd, off_t offset);
long __wrap_syscall(long number, ...);
ssize_t __wrap_read(int fd, void *buffer, size_t count);
void *__wrap_mmap(void *address, size_t length, int prot, int flags, int fd, off_t offset);
#ifdef __GLIBC__
ssize_t __real___read_chk(int fd, void *buffer, size_t count, size_t length);
ssize_t __wrap___read_chk(int fd, void *buffer, size_t count, size_t length);
#endif

static int fail_open, fail_cache, short_read, fail_mapping;
static volatile uint64_t sink;

long
__wrap_syscall(long number, ...)
{
    va_list ap;
    struct perf_event_attr *attr;
    int pid, cpu, group;
    unsigned long flags;
    assert(number == SYS_perf_event_open);
    va_start(ap, number);
    attr = va_arg(ap, struct perf_event_attr *);
    pid = va_arg(ap, int);
    cpu = va_arg(ap, int);
    group = va_arg(ap, int);
    flags = va_arg(ap, unsigned long);
    va_end(ap);
    if (fail_open || (fail_cache && attr->type == PERF_TYPE_HW_CACHE)) {
        errno = fail_open ? EACCES : EOPNOTSUPP;
        return -1;
    }
    return __real_syscall(number, attr, pid, cpu, group, flags);
}

ssize_t
__wrap_read(int fd, void *buffer, size_t count)
{
    ssize_t n = __real_read(fd, buffer, count);
    return short_read && n > 8 ? n - 8 : n;
}

#ifdef __GLIBC__
/* Ubuntu fortification may route read() through __read_chk at -O1. */
ssize_t
__wrap___read_chk(int fd, void *buffer, size_t count, size_t length)
{
    ssize_t n = __real___read_chk(fd, buffer, count, length);
    return short_read && n > 8 ? n - 8 : n;
}
#endif

void *
__wrap_mmap(void *address, size_t length, int prot, int flags, int fd, off_t offset)
{
    if (fail_mapping) {
        errno = ENOMEM;
        return MAP_FAILED;
    }
    return __real_mmap(address, length, prot, flags, fd, offset);
}

static void
work(unsigned count)
{
    uint64_t x = 123;
    for (unsigned i = 0; i < count; i++)
        x = x * 6364136223846793005ULL + i + 1;
    sink = x;
}

int
main(void)
{
    HaloPmcContext ctx;
    HaloPmcSnapshot a, b, first, second, sys;
    double v;
    unsigned family, model;
    const char *vendor = halo_pmc_cpu_vendor(&family, &model);

    assert(halo_pmc_scaled(100, 200, 100, &v) && v == 200);
    assert(halo_pmc_scaled(UINT64_MAX, UINT64_MAX, UINT64_MAX, &v) && isfinite(v));
    assert(!halo_pmc_scaled(10, 10, 0, &v));
    assert(!halo_pmc_scaled(10, 0, 1, &v));
    assert(!halo_pmc_scaled(10, 1, 2, &v));
    assert(!halo_pmc_scaled(10, 1, 1, NULL));
    assert(halo_pmc_sign_extend(0x7f, 8) == 127);
    assert(halo_pmc_sign_extend(0x80, 8) == UINT64_MAX - 127);
    assert(halo_pmc_sign_extend(0xffffffff, 32) == UINT64_MAX);
    assert(halo_pmc_sign_extend(0xffff, 64) == 65535);
    assert(halo_pmc_sign_extend(0x100000001, 32) == 1);
    puts("PASS scaling, zero-running guard and PMC-width sign extension");

    fail_open = 1;
    assert(!halo_pmc_open(&ctx) && ctx.last_errno == EACCES && ctx.valid == 0);
    halo_pmc_close(&ctx);
    fail_open = 0;
    puts("PASS permission failure and idempotent cleanup");

    if (!halo_pmc_open(&ctx)) {
        printf("SKIP real PMU tests: errno=%d %s\n", ctx.last_errno, strerror(ctx.last_errno));
        return 77;
    }
    printf("CPU %s family=%u model=%u; valid mask=0x%x\n", vendor, family, model, ctx.valid);
    assert(halo_pmc_start(&ctx));
    work(500000);
    assert(halo_pmc_stop(&ctx, &first));
    assert(first.value[0] > 0 && first.value[1] > 0);
    assert(first.time_running > 0 && first.time_running <= first.time_enabled);
    assert(halo_pmc_start(&ctx));
    work(100);
    assert(halo_pmc_stop(&ctx, &second));
    assert(second.value[0] < first.value[0] / 10);
    puts("PASS repeated measurements are intervals rather than cumulative totals");

    assert(halo_pmc_start(&ctx));
    work(10000);
    if (halo_pmc_read_fast(&ctx, &a)) {
        assert(halo_pmc_read(&ctx, HALO_PMC_READ_SYSCALL, &sys));
        assert(halo_pmc_read_fast(&ctx, &b));
        for (int i = 0; i < HALO_PMC_EVENTS; i++)
            if (ctx.valid & (1U << i)) {
                assert(a.value[i] <= sys.value[i]);
                assert(sys.value[i] <= b.value[i]);
            }
        {
            uint64_t old_reads = ctx.read_calls;
            for (int i = 0; i < 10000; i++)
                assert(halo_pmc_read(&ctx, HALO_PMC_READ_RDPMC, &a));
            assert(ctx.read_calls == old_reads);
        }
        puts("PASS RDPMC <= read() <= RDPMC and 10000 fast snapshots without read() calls");
    } else {
        puts("SKIP real RDPMC: kernel did not expose a usable mmap counter index");
    }
    assert(halo_pmc_stop(&ctx, &a));

    short_read = 1;
    assert(!halo_pmc_start(&ctx) && ctx.last_errno == EIO);
    short_read = 0;
    assert(halo_pmc_start(&ctx));
    work(10000);
    short_read = 1;
    assert(!halo_pmc_stop(&ctx, &a) && ctx.last_errno == EIO);
    short_read = 0;
    assert(!ctx.active);
    assert(halo_pmc_start(&ctx));
    halo_pmc_cancel(&ctx);
    assert(!ctx.active);
    halo_pmc_close(&ctx);
    halo_pmc_close(&ctx);
    puts("PASS partial-read errors, cancel and cleanup");

    fail_cache = 1;
    fail_mapping = 1;
    assert(halo_pmc_open(&ctx) && ctx.valid == 3);
    fail_cache = fail_mapping = 0;
    assert(halo_pmc_start(&ctx));
    assert(!halo_pmc_read_fast(&ctx, &a));
    assert(!halo_pmc_read(&ctx, HALO_PMC_READ_RDPMC, &a));
    assert(halo_pmc_read(&ctx, HALO_PMC_READ_AUTO, &a));
    work(10000);
    assert(halo_pmc_stop(&ctx, &a));
    assert(a.valid == 3);
    {
        pid_t child = fork();
        assert(child >= 0);
        if (child == 0)
            _exit(!halo_pmc_start(&ctx) && ctx.last_errno == EBADF ? 0 : 1);
        int status;
        assert(waitpid(child, &status, 0) == child);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    halo_pmc_close(&ctx);
    puts("PASS unsupported optional events, mmap failure, syscall fallback and fork-owner guard");
    puts("ALL CORE TESTS PASSED");
    return 0;
}
