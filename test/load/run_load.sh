#!/usr/bin/env bash
# flAPI load harness driver (issue 0b).
#
# Starts a flapi server on a free port against examples/flapi.yaml, runs the k6
# scenario mix, writes the JSON summary, and shuts the server down. Records RSS
# alongside latency because the observability epic adds retained state (export
# queue, payload buffers, audit buffer), so a memory regression matters as much
# as a latency one.
#
# Usage:
#   test/load/run_load.sh [--profile smoke|full] [--out FILE] [--runs N] [--build-type release]
set -euo pipefail
cd "$(dirname "$0")/../.."
REPO="$PWD"

PROFILE=full
OUT=""
RUNS=1
BUILD_TYPE="${FLAPI_BUILD_TYPE:-release}"
TRACING=off

while [ $# -gt 0 ]; do
    case "$1" in
        --profile) PROFILE="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        --runs) RUNS="$2"; shift 2 ;;
        --build-type) BUILD_TYPE="$2"; shift 2 ;;
        # NFR-2: the cost of tracing when it is actually ON. Uses the file
        # exporter so the measurement is of flAPI, not of a collector's latency.
        --tracing) TRACING="$2"; shift 2 ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done

K6="${K6_BIN:-$(command -v k6 || true)}"
if [ -z "$K6" ]; then
    echo "ERROR: k6 not found. Install it, or set K6_BIN." >&2
    echo "  curl -sSL https://github.com/grafana/k6/releases/download/v0.55.0/k6-v0.55.0-linux-amd64.tar.gz | tar xz" >&2
    exit 1
fi

FLAPI="$REPO/build/$BUILD_TYPE/flapi"
# Be explicit rather than falling back: several integration tests silently prefer
# build/debug, and a stale debug binary makes the whole measurement meaningless.
[ -x "$FLAPI" ] || { echo "ERROR: $FLAPI not found. Run: make $BUILD_TYPE" >&2; exit 1; }

port_free() { python3 -c "import socket;s=socket.socket();s.bind(('127.0.0.1',0));print(s.getsockname()[1]);s.close()"; }

run_once() {
    local idx="$1" out_file="$2"
    local port; port=$(port_free)
    local tmp; tmp=$(mktemp -d)
    local log="$tmp/server.log"

    # Each run gets its own copy of the examples tree. The DuckLake paths in
    # examples/flapi.yaml are relative to the config file (./data/cache.ducklake),
    # so sharing the tree makes runs fight over a DuckDB file lock - the second
    # run dies with "Conflicting lock is held". Copying also keeps a load run from
    # mutating the repo's checked-in example data.
    cp -r "$REPO/examples" "$tmp/examples"
    rm -rf "$tmp/examples/data/cache" "$tmp/examples/data/cache.ducklake"* 2>/dev/null || true

    if [ "$TRACING" != "off" ]; then
        cat >> "$tmp/examples/flapi.yaml" <<TRACEEOF

tracing:
  enabled: true
  exporter: otlp_file
  capture: $TRACING
  file:
    path: $tmp/traces.jsonl
  sample:
    type: always_on
TRACEEOF
    fi

    DATAZOO_DISABLE_TELEMETRY=1 \
        "$FLAPI" -c "$tmp/examples/flapi.yaml" -p "$port" --log-level warning \
        > "$log" 2>&1 &
    local pid=$!

    local deadline=$((SECONDS + 60))
    until curl -fsS "http://127.0.0.1:$port/health/live" >/dev/null 2>&1; do
        if ! kill -0 "$pid" 2>/dev/null; then
            echo "ERROR: server exited during startup:" >&2; tail -20 "$log" >&2; exit 1
        fi
        [ $SECONDS -lt $deadline ] || { echo "ERROR: server not live within 60s" >&2; tail -20 "$log" >&2; kill "$pid"; exit 1; }
        sleep 0.5
    done

    echo "--- run $idx/$RUNS on port $port (profile=$PROFILE) ---" >&2
    BASE_URL="http://127.0.0.1:$port" PROFILE="$PROFILE" \
        "$K6" run --quiet --summary-export "$out_file" "$REPO/test/load/scenarios.js" >&2

    # Peak RSS of the server process, in KiB, folded into the summary.
    local rss; rss=$(awk '/VmHWM/{print $2}' "/proc/$pid/status" 2>/dev/null || echo 0)
    python3 - "$out_file" "$rss" <<'PY'
import json, sys
p, rss = sys.argv[1], int(sys.argv[2])
d = json.load(open(p))
d.setdefault("flapi", {})["server_peak_rss_kib"] = rss
json.dump(d, open(p, "w"), indent=2)
PY

    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    rm -rf "$tmp"
}

if [ "$RUNS" -eq 1 ]; then
    OUT="${OUT:-$REPO/test/load/last-run.json}"
    run_once 1 "$OUT"
    echo "summary: $OUT"
else
    # Multiple runs feed the variance measurement (issue 0a) and median-of-N gating.
    DIR=$(mktemp -d); files=()
    for i in $(seq 1 "$RUNS"); do
        f="$DIR/run$i.json"; run_once "$i" "$f"; files+=("$f")
    done
    OUT="${OUT:-$REPO/test/load/last-run.json}"
    python3 "$REPO/test/load/summarize.py" --out "$OUT" "${files[@]}"
    echo "aggregate: $OUT"
fi
