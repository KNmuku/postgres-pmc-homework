\set ON_ERROR_STOP on
\ir init.sql
SET max_parallel_workers_per_gather = 0;
SET enable_mergejoin = off;
SET enable_nestloop = off;
SET jit = off;
SET halo_watch_pmc_list = Instructions, PERF_COUNT_HW_CACHE_L1D_RD, PERF_COUNT_HW_CACHE_L1D_MISS_RD;

CREATE TEMP TABLE pmc_r AS SELECT i AS id, i % 1000 AS value FROM generate_series(1, 20000) AS i;
CREATE TEMP TABLE pmc_s AS SELECT i AS id, i % 1000 AS value FROM generate_series(1, 10000) AS i;
ANALYZE pmc_r;
ANALYZE pmc_s;

-- Base requirement: count the complete HashBuild function.
SET halo_pmc.sample_every = 0;
EXPLAIN (COSTS off) SELECT count(*) FROM pmc_r r JOIN pmc_s s USING (id) WHERE s.value > 100;
SELECT count(*) FROM pmc_r r JOIN pmc_s s USING (id) WHERE s.value > 100;

-- Extension: snapshots immediately before/after tuple hashing and insertion.
-- The event group is enabled once, never enabled/disabled inside this loop.
SET halo_pmc.sample_every = 1;
SET halo_pmc.read_method = auto;
SELECT count(*) FROM pmc_r r JOIN pmc_s s USING (id) WHERE s.value > 100;

-- Optional comparison, with explicit read() calls in the same observation sites.
SET halo_pmc.read_method = read;
SELECT count(*) FROM pmc_r r JOIN pmc_s s USING (id) WHERE s.value > 100;
SET halo_pmc.enabled = off;
