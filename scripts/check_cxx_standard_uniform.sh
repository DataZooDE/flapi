#!/usr/bin/env bash
# Guard against a silent C++-standard split across the flapi-lib / flapi_tests
# boundary.
#
# opentelemetry-cpp pulls in abseil, whose exported targets carry
# INTERFACE_COMPILE_FEATURES "cxx_std_20". Linking otel PRIVATE into flapi-lib
# raises flapi-lib alone; anything else still compiling at C++17 then disagrees
# with it about struct layout. That is an ODR/ABI violation which does not fail
# the build - it segfaults at runtime, far from the cause. (It did: a stack
# crow::response and an EndpointConfig read as garbage in auth_middleware_test.)
#
# Usage: scripts/check_cxx_standard_uniform.sh [build-dir]   (default build/release)
set -euo pipefail
cd "$(dirname "$0")/.."
BUILD_DIR="${1:-build/release}"
CDB="$BUILD_DIR/compile_commands.json"

if [ ! -f "$CDB" ]; then
    echo "SKIP: $CDB not found (configure with CMAKE_EXPORT_COMPILE_COMMANDS=ON)"
    exit 0
fi

python3 - "$CDB" <<'PY'
import json, re, sys, collections
entries = json.load(open(sys.argv[1]))
stds = collections.defaultdict(set)
for e in entries:
    f = e["file"]
    # Only our own sources; vendored deps legitimately use their own standard.
    # Vendored and fetched dependencies legitimately build at their own
    # standard; they interact with flAPI through stable boundaries.
    if any(s in f for s in ("/duckdb/", "vcpkg_installed", "third_party", "/_deps/")):
        continue
    # C sources have no C++ ABI to mismatch.
    if f.endswith((".c", ".h")):
        continue
    if not (f.endswith((".cpp", ".cc", ".cxx")) and (("/src/" in f) or ("/test/" in f))):
        continue
    m = re.search(r"-std=(\S+)", e.get("command", "") or " ".join(e.get("arguments", [])))
    if m:
        stds[m.group(1)].add(f)

if len(stds) > 1:
    print("ERROR: flAPI sources are compiled with more than one C++ standard:", file=sys.stderr)
    for std, files in sorted(stds.items()):
        print(f"  {std}: {len(files)} file(s), e.g. {sorted(files)[0]}", file=sys.stderr)
    print("\nThis is an ABI mismatch, not a style issue. See the comment on"
          "\nCMAKE_CXX_STANDARD in CMakeLists.txt.", file=sys.stderr)
    sys.exit(1)

std = next(iter(stds), None)
print(f"OK: all flAPI sources compile as {std}" if std else "OK: no flAPI sources found")
PY
