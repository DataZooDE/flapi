# `spike/` — opentelemetry-cpp link spike (issue -1)

The epic's abort gate, kept in the tree because it is the check to re-run
whenever the overlay port in `ports/opentelemetry-cpp` is upgraded.

`otel_link_spike.cpp` exercises exactly the opentelemetry-cpp surface flAPI
depends on — `TracerProvider`, `BatchSpanProcessor`, the OTLP HTTP exporter, a
custom `SpanExporter`, the `ParentBased`/`TraceIdRatioBased` samplers, resource
attributes, and W3C `traceparent` extract **and** inject — and links with
flAPI's own Linux flags (`-Wl,--as-needed -Wl,--no-undefined`) at C++17.

## Run it

```bash
cmake -S spike -B spike/build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_TARGET_TRIPLET=x64-linux \
  -DVCPKG_INSTALLED_DIR=$PWD/build/release/vcpkg_installed \
  -DVCPKG_MANIFEST_MODE=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build spike/build && ./spike/build/otel_link_spike     # prints "SPIKE OK"
```

## Result, 2026-09-15 (x64-linux, GCC 16.2.1)

| Gate | Result |
|---|---|
| `opentelemetry-cpp` **1.17.0** (pinned vcpkg baseline) | ❌ **does not compile** — `uint8_t` without `<cstdint>` in 136 headers; fails on GCC 16 *and* on GCC 13 in `ubuntu:24.04`, the CI image |
| `opentelemetry-cpp` **1.24.0** (overlay port) | ✅ builds clean, and ships the `otlp-file` feature 1.17.0 lacks |
| Links under `-Wl,--no-undefined` beside static DuckDB | ✅ |
| Duplicate strong symbols vs `libduckdb_static.a` | ✅ **0** (19,645 vs 6,216 symbols compared) |
| Binary size | 71,432,664 → 76,338,936 = **+4.71 MiB, +6.9 %** |
| `flapi pack` / `info` / `unpack` round-trip | ✅ 67 entries |
| `FLAPI_WITH_TRACING=OFF` | ✅ builds, runs, **0** otel symbols, within 36 KB of the pre-otel baseline |
| C++17 | ✅ (otel's config propagates gnu++20; flAPI pins 17 and it compiles) |

**Verdict: PROCEED, keeping the SDK.** Decision D5 in
`docs/plans/otel-observability.md` is settled: opentelemetry-cpp stays, and
because 1.24.0 provides `otlp-file`, flAPI does **not** need to hand-roll a file
exporter — issue 6 shrinks to configuration plus tests.
