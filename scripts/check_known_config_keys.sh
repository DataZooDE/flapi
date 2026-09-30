#!/usr/bin/env bash
# Fails when src/config_manager.cpp reads a top-level key (config["x"]) that is
# missing from ConfigManager::KnownTopLevelKeys(), so the "unknown key" warning
# (#160) cannot drift and cry wolf.
set -euo pipefail
cd "$(dirname "$0")/.."
src=src/config_manager.cpp
table=$(sed -n '/KnownTopLevelKeys() {/,/return keys;/p' "$src")
# Endpoint-file keys also read from a node named `config` in this file.
endpoint_keys=" cache connection rate-limit request "
missing=0
for key in $(grep -o 'config\["[A-Za-z_-]*"\]' "$src" | sed 's/config\["//; s/"\]//' | sort -u); do
    [[ "$endpoint_keys" == *" $key "* ]] && continue
    if ! grep -q "\"$key\"" <<<"$table"; then
        echo "config key '$key' is read in $src but not in KnownTopLevelKeys()" >&2
        missing=1
    fi
done
exit $missing
