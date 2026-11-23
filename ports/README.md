# vcpkg overlay ports

Ports here override the registry version selected by `vcpkg.json`'s
`builtin-baseline`, without moving that baseline for every other dependency.
They are picked up declaratively via `vcpkg-configuration.json`, so every
platform and CI job gets them with no environment plumbing.

## `opentelemetry-cpp` (1.24.0)

**Why this exists.** The pinned baseline
(`b2cb0da531c2f1f740045bfe7c4dac59f0b2b69c`, matching the vcpkg tag
`2024.11.16` used by the CI Docker images) resolves `opentelemetry-cpp` to
**1.17.0#1**, which has two disqualifying problems:

1. **It does not compile on the project's own CI toolchain.** 1.17.0 uses
   `uint8_t` and friends without including `<cstdint>` in 136 headers. Modern
   libstdc++ no longer provides it transitively, so the build fails at
   `api/include/opentelemetry/logs/severity.h:20`. Verified failing on both
   GCC 16 (local) and **GCC 13 on ubuntu:24.04**, which is exactly what
   `.github/docker/linux_amd64/Dockerfile` builds with.
2. **It has no `otlp-file` feature.** The OTLP File exporter postdates it, and
   that exporter is what serves flAPI's air-gapped deployment topology.

1.24.0 fixes both: it compiles clean, and it ships `otlp-file` as a feature, so
flAPI does not have to hand-roll a file exporter.

**Scope of the override.** Only `opentelemetry-cpp` moves. Every other package
still comes from the pinned baseline, so this is not a vcpkg bump. The port is a
verbatim copy of the upstream vcpkg port at the version above.

**Upgrading.** Replace this directory with a newer upstream port and re-run the
link check in `spike/` (`otel_link_spike.cpp`), which exercises the exact
surface flAPI depends on: TracerProvider, BatchSpanProcessor, the OTLP HTTP and
file exporters, the W3C propagator, and samplers. Delete this overlay entirely
once the pinned baseline carries a version that satisfies both requirements
above.

## `crow` (1.3.4)

**Why this exists.** flAPI completes responses from its handler pool (#120), which needs a strong
reference to the connection across the offload: `async-completion-keepalive.patch` adds
`crow::response::connection_keepalive` (not upstream). The pinned baseline would also resolve an
older crow (1.2.0 had a JSON writer that cut exponents, `1e300` -> `1000.0`, and no TCP_NODELAY option).

**Upgrading.** Bump `version` and `SHA512` in `portfile.cmake`, re-apply the patch (two lines in
`http_connection.h`, one member in `http_response.h`), and run the full integration suite: the
startup-failure tests (Crow 1.3 logs a bind failure instead of throwing), the keep-alive tests and
test_handler_offload exercise exactly the Crow behaviour flAPI depends on.
