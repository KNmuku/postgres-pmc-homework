#!/bin/sh
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
exclude=$(git -C "$repo" rev-parse --git-path info/exclude)
case "$exclude" in /*) ;; *) exclude="$repo/$exclude" ;; esac
for item in '/.pmc-build/' '/.pmc-install/'; do
    if ! grep -qxF "$item" "$exclude"; then printf '\n%s\n' "$item" >> "$exclude"; fi
done
build="$repo/.pmc-build/autoconf"
mkdir -p "$build" "$repo/.pmc-build/logs"
cd "$build"
if [ ! -f config.status ]; then
    "$repo/configure" --prefix="$repo/.pmc-install" --enable-debug --enable-cassert \
        --enable-tap-tests CFLAGS='-O2 -g' > "$repo/.pmc-build/logs/configure.log" 2>&1
fi
make -j"${JOBS:-8}" > "$repo/.pmc-build/logs/build.log" 2>&1
make install > "$repo/.pmc-build/logs/install.log" 2>&1
printf 'Built PostgreSQL and installed into %s\n' "$repo/.pmc-install"
