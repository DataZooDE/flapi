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
| C++ standard | ⚠️ **forced flAPI to C++20 project-wide** — see below |

## The one thing that bit us

opentelemetry-cpp pulls in **abseil**, and abseil's exported targets carry
`INTERFACE_COMPILE_FEATURES "cxx_std_20"` (`ABSL_PROPAGATE_CXX_STD`), because the
vcpkg abseil binaries are themselves built as C++20.

Linking otel **PRIVATE** into `flapi-lib` therefore raised *only* `flapi-lib` to
C++20. `flapi_tests` and even the `flapi` executable (`src/main.cpp`) kept
compiling at C++17. The build stayed green and `ctest` then **segfaulted** in
`auth_middleware_test` while copying `endpoint->auth.type`: the two halves
disagreed about struct layout.

Nothing warns about this. It is the same hazard class as the
`CROW_ENABLE_COMPRESSION` ODR bug fixed in `d2b9e74`, with the trigger buried two
dependency levels down.

The fix is one standard everywhere (`CMAKE_CXX_STANDARD 20`) rather than
suppressing the propagation, because consuming C++20-built abseil from C++17
translation units is the underlying problem. DuckDB is unaffected — flAPI uses
its C API. `scripts/check_cxx_standard_uniform.sh` now fails the build if the
standard ever splits again.

**Verdict: PROCEED, keeping the SDK.** Decision D5 in
`docs/archive/otel-observability.md` is settled: opentelemetry-cpp stays, and
because 1.24.0 provides `otlp-file`, flAPI does **not** need to hand-roll a file
exporter — issue 6 shrinks to configuration plus tests.
