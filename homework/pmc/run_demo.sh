#!/bin/sh
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
bin="$repo/.pmc-install/bin"
results="$repo/homework/pmc/results"
mkdir -p "$repo/.pmc-build" "$results"
run=$(mktemp -d "$repo/.pmc-build/pmc-run.XXXXXX")
socket=$(mktemp -d /tmp/halo-pmc.XXXXXX)
data="$run/data"
mkdir -p "$socket"
port=65439
started=0
cleanup() {
    if [ "$started" = 1 ]; then "$bin/pg_ctl" -D "$data" -m fast -w stop >/dev/null; fi
    rmdir "$socket" 2>/dev/null || true
}
trap cleanup EXIT HUP INT TERM
"$bin/initdb" -D "$data" -A trust --no-locale > "$results/initdb.log" 2>&1
"$bin/pg_ctl" -D "$data" -l "$run/server.log" -o "-k $socket -p $port -c listen_addresses=''" -w start > "$results/start.log" 2>&1
started=1
"$bin/psql" -X -v ON_ERROR_STOP=1 -h "$socket" -p "$port" -d postgres \
    -f "$repo/homework/pmc/sql/demo.sql" > "$results/demo.txt" 2>&1
python3 "$repo/homework/pmc/tests/integration.py" --psql "$bin/psql" \
    --socket "$socket" --port "$port" > "$results/integration.txt" 2>&1
cat "$results/integration.txt"
printf 'Demo output: %s\n' "$results/demo.txt"
