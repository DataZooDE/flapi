#!/usr/bin/env bash
#
# Container smoke test for cgroup-aware memory sizing (#71).
#
# Background: issue #71 assumed DuckDB sizes max_memory from *host* RAM and
# ignores cgroup limits, so a memory-heavy query inside a small container would
# blow past the limit and get kernel-OOM-killed (exit 137). Empirically, the
# bundled DuckDB (1.5.2) is ALREADY cgroup-aware: it sizes max_memory to ~80%
# of the cgroup memory limit and threads to the CPU quota. So the kernel-OOM
# "red" cannot be reproduced -- DuckDB prevents it upstream.
#
# This test therefore asserts the *good* behavior as a regression guard: a
# ~3 GiB query in a 1 GiB container fails with a GRACEFUL DuckDB "Out of
# Memory" error (HTTP 500) and the server SURVIVES (would be exit 137 / gone if
# DuckDB ever stopped honoring the cgroup limit). It also asserts flAPI's
# startup `resource-limits:` log reports the cgroup-detected ceiling.
#
# Requirements: Linux + Docker. Skips (exit 0) if either is unavailable.
#
# Tunables (env):
#   FLAPI_SMOKE_BINARY  path to a linux flapi binary   (default: build/release/flapi)
#   FLAPI_SMOKE_IMAGE   base image to run it in        (default: ubuntu:24.04)
#   FLAPI_SMOKE_MEM     container memory limit         (default: 1g)
#   FLAPI_SMOKE_ROWS    memory-pressure row count      (default: 3000000)

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

BINARY="${FLAPI_SMOKE_BINARY:-${REPO_ROOT}/build/release/flapi}"
IMAGE="${FLAPI_SMOKE_IMAGE:-ubuntu:24.04}"
MEM="${FLAPI_SMOKE_MEM:-1g}"
ROWS="${FLAPI_SMOKE_ROWS:-3000000}"
CONFIG_DIR="${SCRIPT_DIR}/smoke"

RED='\033[0;31m'; GREEN='\033[0;32m'; YEL='\033[0;33m'; NC='\033[0m'
info()  { echo -e "${YEL}[smoke]${NC} $*"; }
pass()  { echo -e "${GREEN}[PASS]${NC} $*"; }
fail()  { echo -e "${RED}[FAIL]${NC} $*"; }
skip()  { echo -e "${YEL}[SKIP]${NC} $*"; exit 0; }

# --- preflight ------------------------------------------------------------
[ "$(uname -s)" = "Linux" ]      || skip "not Linux (uname=$(uname -s)); cgroup memory limits are Linux-only"
command -v docker >/dev/null 2>&1 || skip "docker not found on PATH"
docker info >/dev/null 2>&1       || skip "docker daemon not reachable"
[ -x "${BINARY}" ]                || { fail "flapi binary not found/executable: ${BINARY} (build with 'make release')"; exit 1; }

if ! docker run --rm -v "${BINARY}:/app/flapi:ro" "${IMAGE}" /app/flapi --version >/dev/null 2>&1; then
    skip "binary ${BINARY} cannot run in ${IMAGE} (likely glibc mismatch); set FLAPI_SMOKE_IMAGE to a compatible base or FLAPI_SMOKE_BINARY to a CI-built linux binary"
fi

CID=""
cleanup() { [ -n "${CID}" ] && docker rm -f "${CID}" >/dev/null 2>&1; }
trap cleanup EXIT

info "Starting flAPI in a --memory=${MEM} container (no explicit max_memory)..."
CID="$(docker run -d --rm \
    --memory="${MEM}" --memory-swap="${MEM}" \
    -v "${BINARY}:/app/flapi:ro" \
    -v "${CONFIG_DIR}:/cfg:ro" \
    -p 127.0.0.1:0:8080 \
    "${IMAGE}" \
    /app/flapi -c /cfg/flapi.yaml -p 8080 --host 0.0.0.0 --log-level info)"

PORT="$(docker port "${CID}" 8080/tcp 2>/dev/null | head -n1 | sed 's/.*://')"

healthy=0
for _ in $(seq 1 60); do
    if curl -sf --max-time 3 "http://127.0.0.1:${PORT}/health/" >/dev/null 2>&1; then healthy=1; break; fi
    sleep 0.5
done
if [ "${healthy}" -ne 1 ]; then
    fail "server never became healthy"; docker logs "${CID}" 2>&1 | tail -20; exit 1
fi

rc=0

# (1) startup log reports the cgroup-detected limit
LOGS="$(docker logs "${CID}" 2>&1)"
if echo "${LOGS}" | grep -qE 'resource-limits: memory source=cgroup'; then
    pass "startup log reports cgroup-detected memory limit"
    echo "${LOGS}" | grep -E 'resource-limits:' | sed 's/^/        /'
else
    fail "startup log missing 'resource-limits: memory source=cgroup...'"
    echo "${LOGS}" | tail -20; rc=1
fi

# (2) memory-heavy query degrades gracefully and the server survives
info "Firing ~3 GiB memory bomb (rows=${ROWS}) into a ${MEM} container..."
BODY="$(curl -s --max-time 90 "http://127.0.0.1:${PORT}/memtest/?rows=${ROWS}" 2>/dev/null || true)"
HEALTH="$(curl -s -o /dev/null --max-time 5 -w '%{http_code}' "http://127.0.0.1:${PORT}/health/" 2>/dev/null || true)"
STATE="$(docker inspect -f '{{.State.Status}} oom={{.State.OOMKilled}} exit={{.State.ExitCode}}' "${CID}" 2>/dev/null)"
info "post-bomb container state='${STATE}' /health=${HEALTH:-<none>}"

if echo "${STATE}" | grep -q 'oom=true' || echo "${STATE}" | grep -q 'exit=137'; then
    fail "process was KERNEL OOM-killed (${STATE}) -- DuckDB stopped honoring the cgroup limit; this is the #71 regression."
    rc=1
fi
if echo "${BODY}" | grep -qiE 'out of memory'; then
    pass "query failed with a graceful DuckDB 'Out of Memory' error (no kernel kill)"
else
    fail "expected a graceful 'Out of Memory' error, got: $(echo "${BODY}" | head -c 200)"; rc=1
fi
if [ "${HEALTH}" = "200" ]; then
    pass "server still serving requests after the bomb (health=200)"
else
    fail "server not healthy after the bomb (health=${HEALTH})"; rc=1
fi

echo
if [ "${rc}" -eq 0 ]; then
    pass "resource-limit smoke test PASSED: DuckDB honors the cgroup limit and degrades gracefully."
else
    fail "resource-limit smoke test FAILED (see above)."
fi
exit "${rc}"
