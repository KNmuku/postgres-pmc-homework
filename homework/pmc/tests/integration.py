#!/usr/bin/env python3
"""Real-backend tests. The caller supplies an isolated running test cluster."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
import re
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--psql', required=True)
parser.add_argument('--socket', required=True)
parser.add_argument('--port', required=True)
args = parser.parse_args()
command = [args.psql, '-X', '-q', '-A', '-t', '-v', 'ON_ERROR_STOP=1',
           '-h', args.socket, '-p', args.port, '-d', 'postgres']

def query(sql, *, check=True, options=None):
    env = dict(os.environ)
    env.pop('PGOPTIONS', None)
    if options:
        env['PGOPTIONS'] = options
    result = subprocess.run(command, input=sql, text=True, capture_output=True, env=env)
    if check and result.returncode:
        raise AssertionError(result.stderr)
    records = [json.loads(line.split('halo_pmc ', 1)[1]) for line in result.stderr.splitlines()
               if re.match(r'^NOTICE:\s+halo_pmc \{', line)]
    return result, records

setup = '''
SET jit = off;
SET max_parallel_workers_per_gather = 0;
SET enable_mergejoin = off;
SET enable_nestloop = off;
CREATE TEMP TABLE r AS SELECT i AS id FROM generate_series(1,2000) i;
CREATE TEMP TABLE s AS SELECT i AS id FROM generate_series(1,1000) i;
ANALYZE r; ANALYZE s;
'''
join = 'SELECT count(*) FROM r JOIN s USING(id);\n'
passed = []

def done(name):
    passed.append(name)
    print('PASS', name, flush=True)

r, reports = query(setup + join)
assert r.stdout.strip() == '1000' and reports == []
done('disabled mode preserves query output and produces no PMC report')

r, reports = query(setup + 'SET halo_pmc.enabled=on;\n' + join)
assert r.stdout.strip() == '1000' and len(reports) == 1
record = reports[0]
assert record['scope'] == 'HashBuild' and record['rows'] == 1000
assert record['instructions'] > 0 and record['cycles'] > 0
assert 0 < record['time_running_ns'] <= record['time_enabled_ns']
assert record['loop_samples'] == record['rdpmc_calls'] == 0
done('HashBuild interval has real nonzero counters with no loop sampling')

for method in ['read', 'rdpmc', 'auto']:
    sql = setup + f'SET halo_pmc.enabled=on; SET halo_pmc.sample_every=10; SET halo_pmc.read_method={method};\n' + join
    r, reports = query(sql)
    assert len(reports) == 1 and r.stdout.strip() == '1000'
    record = reports[0]
    assert record['loop_samples'] == 100 and record['skipped_samples'] == 0, record
    assert record['segment_raw_counts']['instructions'] > 0
    assert record['segment_raw_counts']['cycles'] > 0
    if method == 'read':
        assert record['read_calls'] == 202 and record['rdpmc_calls'] == 0
    else:
        assert record['read_calls'] == 2 and record['rdpmc_calls'] == 200
    done(f'paired target-code snapshots with method={method}; no per-iteration enable/disable')

for setting in ['halo_pmc.events', 'halo_pmc_list', 'halo_watch_pmc_list']:
    sql = setup + f'''SET halo_pmc.enabled=on;
SET {setting} = Instructions, PERF_COUNT_HW_CACHE_L1D_RD, PERF_COUNT_HW_CACHE_L1D_MISS_RD;
''' + join
    r, reports = query(sql)
    assert len(reports) == 1
    assert reports[0]['perf_count_hw_cache_l1d_rd'] > 0
    assert reports[0]['perf_count_hw_cache_l1d_miss_rd'] is not None
    assert 0 <= reports[0]['l1d_hit_rate_estimate_percent'] <= 100
    done(f'pmc_name lookup and course event-list spelling {setting}')

cpuinfo = open('/proc/cpuinfo', encoding='utf-8').read()
if 'AuthenticAMD' in cpuinfo and re.search(r'cpu family\s*:\s*26\b', cpuinfo) and re.search(r'model\s*:\s*68\b', cpuinfo):
    r, records = query(setup + 'SET halo_pmc.enabled=on; SET halo_pmc.events=amd_ls_dispatch_stores;\n' + join)
    assert len(records) == 1 and records[0]['amd_ls_dispatch_stores'] > 0
    done('CPU-matched AMD raw store-dispatch event is loaded from pmc_name')

bad_profile = '''INSERT INTO public.pmc_name(name,pmctype,pe_config,cpu_vendor,description)
VALUES('test_wrong_cpu',4,553,'not_this_cpu','deliberate CPU mismatch');
SET halo_pmc.enabled=on; SET halo_pmc.target=executor;
SET halo_pmc.events=test_wrong_cpu; SELECT 1;
'''
r, records = query(bad_profile, check=False)
assert r.returncode != 0 and 'is not defined for CPU' in r.stderr and not records
done('CPU-incompatible raw event is rejected before producing misleading counts')

quoted_name = '''INSERT INTO public.pmc_name(name,pmctype,pe_config,description)
VALUES('quote"event',0,4,'quoted catalog-name test');
SET halo_pmc.enabled=on; SET halo_pmc.target=executor;
SET halo_pmc.sample_every=1; SET halo_pmc.events='"quote""event"';
SELECT 42424;
'''
r, records = query(quoted_name)
assert r.stdout.strip() == '42424' and len(records) == 1
assert 'quote"event' in records[0] and 'quote"event' in records[0]['segment_raw_counts']
done('quoted catalog event names still produce valid report JSON')

r, reports = query('SELECT 12345;', options='-c halo_pmc.enabled=on -c halo_pmc.target=executor -c halo_pmc.events=instructions,cycles')
assert r.stdout.strip() == '12345' and len(reports) == 1
assert reports[0]['scope'] == 'ExecutorRun' and 'perf_count_hw_cache_l1d_rd' not in reports[0]
done('connection startup GUC placeholders retain the configured event list')

sql = '''
CREATE FUNCTION pg_temp.nested_pmc() RETURNS integer LANGUAGE plpgsql AS $$
BEGIN PERFORM sum(i) FROM generate_series(1,1000) i; RETURN 42; END $$;
SET halo_pmc.target=executor; SET halo_pmc.enabled=on;
SELECT pg_temp.nested_pmc();
'''
r, reports = query(sql)
assert r.stdout.strip() == '42' and len(reports) == 1
done('nested SPI executors are included once in the outer interval')

sql = '''
CREATE FUNCTION pg_temp.caught_pmc() RETURNS integer LANGUAGE plpgsql AS $$
BEGIN
  BEGIN PERFORM 10/(i-1) FROM generate_series(1,2) i;
  EXCEPTION WHEN division_by_zero THEN NULL; END;
  RETURN 42;
END $$;
SET halo_pmc.target=executor; SET halo_pmc.enabled=on;
SELECT pg_temp.caught_pmc(); SELECT 31415;
'''
r, reports = query(sql)
assert r.stdout.split() == ['42', '31415'] and len(reports) == 1
assert reports[0]['rows'] == 1 and reports[0]['instructions'] > 0
done('caught nested error discards partial measurement and next query recovers')

sql = setup + r'''
SET halo_pmc.enabled=on;
\set ON_ERROR_STOP off
SET halo_pmc.events = missing_event;
''' + join + r'''
SET halo_pmc.events = instructions,cycles;
\set ON_ERROR_STOP on
''' + join
r, reports = query(sql)
assert 'not uniquely defined' in r.stderr and r.stdout.strip() == '1000'
assert len(reports) == 1
done('bad catalog name does not leave the next measurement nested or enabled')

sql = '''
SET halo_pmc.target=executor; SET halo_pmc.enabled=on;
BEGIN; DECLARE c CURSOR FOR SELECT i FROM generate_series(1,10) i;
FETCH 3 FROM c; FETCH 2 FROM c; CLOSE c; COMMIT;
'''
r, reports = query(sql)
assert [x['rows'] for x in reports] == [3, 2]
done('cursor FETCH invocations use independent count/time deltas')

def independent(n):
    r, records = query(f'SET halo_pmc.target=executor; SET halo_pmc.enabled=on; SELECT {n};')
    assert r.stdout.strip() == str(n) and len(records) == 1
    assert records[0]['instructions'] > 0
with ThreadPoolExecutor(max_workers=4) as pool:
    list(pool.map(independent, range(12)))
done('12 connections maintain independent backend counter state')

r, _ = query('CREATE ROLE pmc_restricted; SET ROLE pmc_restricted; SET halo_pmc.enabled=on;', check=False)
assert r.returncode != 0 and 'permission denied' in r.stderr
done('PMC settings require superuser privileges')

r, records = query('SET halo_pmc.target=executor; SET halo_pmc.enabled=on; SELECT 27182; SET halo_pmc.enabled=off; SELECT 27182;')
assert r.stdout.split() == ['27182', '27182'] and len(records) == 1
done('turning observation off stops subsequent measurements')

print(f'ALL {len(passed)} POSTGRESQL INTEGRATION TESTS PASSED')
