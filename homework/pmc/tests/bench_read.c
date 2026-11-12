#include "utils/halo_pmc.h"
#include <stdio.h>
#include <stdlib.h>
#include <sched.h>
#include <time.h>

static volatile uint64_t sink;

static double now_ns(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &t) != 0) exit(1);
    return (double)t.tv_sec * 1e9 + t.tv_nsec;
}

static int cmp(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

int main(void)
{
    enum { N = 50000, REPEATS = 7 };
    HaloPmcContext ctx;
    HaloPmcSnapshot s;
    double times[2][REPEATS];
    uint64_t calls[2] = {0, 0};
    cpu_set_t allowed, selected;
    int cpu = -1;
    if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0) return 1;
    for (int i = 0; i < CPU_SETSIZE; i++)
        if (CPU_ISSET(i, &allowed)) { cpu = i; break; }
    if (cpu < 0) return 1;
    CPU_ZERO(&selected); CPU_SET(cpu, &selected);
    if (sched_setaffinity(0, sizeof(selected), &selected) != 0) return 1;
    if (!halo_pmc_open(&ctx) || !halo_pmc_start(&ctx)) {
        fprintf(stderr, "PMU unavailable errno=%d\n", ctx.last_errno);
        return 77;
    }
    if (!halo_pmc_read_fast(&ctx, &s)) {
        puts("{\"rdpmc_available\":false}");
        halo_pmc_cancel(&ctx); halo_pmc_close(&ctx);
        return 77;
    }
    for (int r = 0; r < REPEATS; r++) {
        for (int pos = 0; pos < 2; pos++) {
            int m = pos ^ (r & 1);
            HaloPmcReadMethod method = m ? HALO_PMC_READ_RDPMC : HALO_PMC_READ_SYSCALL;
            uint64_t before = ctx.read_calls;
            double t = now_ns();
            for (int i = 0; i < N; i++) {
                if (!halo_pmc_read(&ctx, method, &s)) return 1;
                sink = s.value[0];
            }
            times[m][r] = (now_ns() - t) / N;
            calls[m] += ctx.read_calls - before;
        }
    }
    halo_pmc_cancel(&ctx); halo_pmc_close(&ctx);
    qsort(times[0], REPEATS, sizeof(double), cmp);
    qsort(times[1], REPEATS, sizeof(double), cmp);
    printf("{\"rdpmc_available\":true,\"cpu\":%d,\"events\":4,\"snapshots_per_repeat\":%d,"
           "\"repeats\":%d,\"read_ns_median\":%.3f,\"rdpmc_ns_median\":%.3f,"
           "\"speedup\":%.3f,\"read_path_read_calls\":%llu,\"rdpmc_path_read_calls\":%llu}\n",
           cpu, N, REPEATS, times[0][3], times[1][3], times[0][3] / times[1][3],
           (unsigned long long)calls[0], (unsigned long long)calls[1]);
    return 0;
}
