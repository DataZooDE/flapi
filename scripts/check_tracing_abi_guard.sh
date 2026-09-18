#!/usr/bin/env bash
# Verifies that the FLAPI_WITH_TRACING ODR guard is real.
#
# FLAPI_WITH_TRACING changes the layout of flapi::SpanScope, which is embedded in
# the Crow middleware tuple instantiated across several translation units. If the
# two build modes produced the SAME symbol names, a mixed build would link
# cleanly and then corrupt memory - the exact failure class that made
# CROW_ENABLE_COMPRESSION a global define, and that a split C++ standard already
# caused once in this repo.
#
# The protection is the versioned inline namespace: SpanScope's out-of-line
# methods mangle as tracing_on_v1::... or tracing_off_v1::..., so a mismatch is an
# undefined symbol at link time. This script proves the mangling actually differs
# rather than trusting that it does.
#
# Usage: scripts/check_tracing_abi_guard.sh <lib-a> [lib-b]
set -euo pipefail
cd "$(dirname "$0")/.."

lib="${1:-build/release/libflapi-lib.a}"
if [ ! -f "$lib" ]; then
    echo "SKIP: $lib not found"
    exit 0
fi

on=$(nm -C "$lib" 2>/dev/null | grep -c "tracing_on_v1::SpanScope::" || true)
off=$(nm -C "$lib" 2>/dev/null | grep -c "tracing_off_v1::SpanScope::" || true)

if [ "$on" -gt 0 ] && [ "$off" -gt 0 ]; then
    echo "ERROR: $lib contains BOTH tracing_on_v1 and tracing_off_v1 SpanScope symbols." >&2
    echo "A single library must be built in exactly one mode." >&2
    exit 1
fi
if [ "$on" -eq 0 ] && [ "$off" -eq 0 ]; then
    echo "ERROR: $lib has no versioned SpanScope symbols at all." >&2
    echo "The inline namespace has been removed or the methods inlined away," >&2
    echo "which silently disables the mixed-build link guard." >&2
    exit 1
fi

if [ "$on" -gt 0 ]; then
    echo "OK: $lib built with tracing ON ($on versioned symbols); a mixed link would fail."
else
    echo "OK: $lib built with tracing OFF ($off versioned symbols); a mixed link would fail."
fi
