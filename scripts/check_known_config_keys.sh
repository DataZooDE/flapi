#!/usr/bin/env bash
# Fails when a config key is read but missing from its "known keys" table, so
# the "unknown key" warnings cannot drift and cry wolf:
#
# - the main file: src/config_manager.cpp reads config["x"] but x is not in
#   ConfigManager::KnownTopLevelKeys() (#160);
# - endpoint files: src/config_manager.cpp reads endpoint_config["x"], or
#   src/endpoint_config_parser.cpp reads yaml_node["x"] / safeGet(yaml_node,
#   "x", ...), but x is not in EndpointConfigParser::KnownEndpointKeys() (#282).
set -euo pipefail
cd "$(dirname "$0")/.."
src=src/config_manager.cpp
parser=src/endpoint_config_parser.cpp
table=$(sed -n '/KnownTopLevelKeys() {/,/return keys;/p' "$src")
endpoint_table=$(sed -n '/KnownEndpointKeys() {/,/return keys;/p' "$parser")
[[ -n "$table" ]] || { echo "KnownTopLevelKeys() not found in $src" >&2; exit 1; }
[[ -n "$endpoint_table" ]] || { echo "KnownEndpointKeys() not found in $parser" >&2; exit 1; }
missing=0

# Main-file keys: config["x"], not endpoint_config["x"].
# Endpoint-file keys also read from a node named `config` in config_manager.cpp;
# they are checked against the endpoint table below instead.
endpoint_keys_named_config=" cache connection rate-limit request "
for key in $(grep -o '\(^\|[^_A-Za-z]\)config\["[A-Za-z_-]*"\]' "$src" | sed 's/.*config\["//; s/"\]//' | sort -u); do
    [[ "$endpoint_keys_named_config" == *" $key "* ]] && continue
    if ! grep -q "\"$key\"" <<<"$table"; then
        echo "config key '$key' is read in $src but not in KnownTopLevelKeys()" >&2
        missing=1
    fi
done

# Endpoint-file keys.
endpoint_readers=$(
    echo $endpoint_keys_named_config | tr ' ' '\n'
    grep -o 'endpoint_config\["[A-Za-z_-]*"\]' "$src" | sed 's/.*\["//; s/"\]//'
    grep -o 'yaml_node\["[A-Za-z_-]*"\]' "$parser" | sed 's/.*\["//; s/"\]//'
    grep -o 'safeGet<[^>]*>(yaml_node, "[A-Za-z_-]*"' "$parser" | sed 's/.*(yaml_node, "//; s/"$//'
)
for key in $(sort -u <<<"$endpoint_readers"); do
    if ! grep -q "\"$key\"" <<<"$endpoint_table"; then
        echo "endpoint key '$key' is read but not in EndpointConfigParser::KnownEndpointKeys()" >&2
        missing=1
    fi
done
exit $missing
