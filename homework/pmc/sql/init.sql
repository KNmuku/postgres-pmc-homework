\set ON_ERROR_STOP on
-- Course event-name catalog. Run as the database owner/superuser.
CREATE TABLE IF NOT EXISTS public.pmc_name (
    name text PRIMARY KEY CHECK (length(name) BETWEEN 1 AND 95 AND name = lower(name)),
    pmctype integer NOT NULL CHECK (pmctype >= 0),
    pe_config bigint NOT NULL,
    config1 bigint NOT NULL DEFAULT 0,
    config2 bigint NOT NULL DEFAULT 0,
    cpu_vendor text NOT NULL DEFAULT 'any',
    cpu_family integer,
    cpu_model integer,
    description text NOT NULL
);

INSERT INTO public.pmc_name(name, pmctype, pe_config, cpu_vendor, cpu_family, cpu_model, description) VALUES
('instructions', 0, 1, 'any', NULL, NULL, 'Retired user-mode instructions'),
('cycles', 0, 0, 'any', NULL, NULL, 'User-mode CPU cycles'),
('perf_count_hw_cache_l1d_rd', 3, 0, 'any', NULL, NULL,
 'Generic L1D READ ACCESS event; exact CPU/kernel mapping is platform dependent'),
('perf_count_hw_cache_l1d_miss_rd', 3, 65536, 'any', NULL, NULL,
 'Generic L1D READ MISS event; use with matching ACCESS event for an estimate'),
('branch_instructions', 0, 4, 'any', NULL, NULL, 'Retired branches'),
('branch_misses', 0, 5, 'any', NULL, NULL, 'Mispredicted branches'),
('amd_ls_dispatch_stores', 4, 553, 'AuthenticAMD', 26, 68,
 'Zen5 model 44h: store operations dispatched to LS, speculative, not retired stores'),
('amd_ls_dispatch_all', 4, 1833, 'AuthenticAMD', 26, 68,
 'Zen5 model 44h: load/store/RMW operations dispatched to LS, speculative'),
('mem_inst_retired_all_stores', 4, 33488, 'GenuineIntel', 6, 94,
 'Intel Skylake model 5Eh example only: MEM_INST_RETIRED.ALL_STORES; verify other models')
ON CONFLICT (name) DO NOTHING;

COMMENT ON TABLE public.pmc_name IS 'HaloPMC event definitions; raw events must match CPU vendor/family/model';
-- Current backend has already registered the module during initialization.
SET halo_pmc.target = hash;
SET halo_pmc.enabled = on;
