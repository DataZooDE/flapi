# Shipping one self-contained binary

**Goal:** deploy flAPI and its whole configuration tree as a single executable you
can `scp` to a host — no config directory, no volume mount.

---

The same `flapi` binary that serves the API can fold an entire
config tree into itself, producing one self-contained executable
deployable via `scp`.

```bash
# Pack a config tree into a new bundled binary
flapi pack --in ./examples --out flapi-prod

# Inspect what's bundled
./flapi-prod info

# Extract the bundle for debugging
./flapi-prod unpack --to /tmp/extracted

# Run it — serves the bundled config from any cwd
cd /tmp && ./flapi-prod
```

**How it works:** a ZIP archive is appended after the executable on
Linux/Windows (or written into a pre-allocated `__FLAPI/__bundle`
Mach-O segment on macOS, then re-`codesign`-ed so the result is
notarisable). At startup, flAPI reverse-scans for the bundle (or
probes the segment on macOS) and registers an
`EmbeddedArchiveFileProvider` plus an `embed://` DuckDB
filesystem so config / SQL templates / `read_csv()` calls all
resolve to the in-memory bundle. If no bundle is present
(unbundled binary, truncated tail), all paths fall back to the
local filesystem unchanged — existing operators see no behaviour
change.

**Secrets stay out of the bundle.** `flapi pack` refuses files
matching `*.env`, `secrets/*`, `*.pem`, `*.key` by default. The
override (`--allow-secrets`) is for testing only. Credentials
come from the environment at runtime (`AWS_*`, `GOOGLE_*`,
`AZURE_*`, `FLAPI_CONFIG_SERVICE_TOKEN`, `{{env.VAR}}` YAML
interpolation).

**Reproducible builds.** Set `SOURCE_DATE_EPOCH` before
`flapi pack` and the output is byte-identical across runs:

```bash
SOURCE_DATE_EPOCH=1700000000 flapi pack --in examples --out a
SOURCE_DATE_EPOCH=1700000000 flapi pack --in examples --out b
sha256sum a b   # identical
```

**12-factor env vars.** `FLAPI_CONFIG` falls back for `-c` /
`--config`; `FLAPI_LOG_LEVEL` falls back for `--log-level`. CLI
flag wins over env var wins over built-in default. Invalid log
levels exit non-zero with a single-line error.

**macOS notes.** The reserved-segment size is 16 MiB by default
(knob `FLAPI_RESERVED_BUNDLE_MIB` at CMake configure time);
oversized bundles exit non-zero with a corrective error. A
`--macos-append` flag is available for local debugging — it uses
the Linux/Windows append-after-EOF layout but the result is
intentionally not notarisable.

See [docs/CLI_REFERENCE.md §3](../CLI_REFERENCE.md#3-self-packaging-subcommands)
and [docs/spec/DESIGN_DECISIONS.md §9](../spec/DESIGN_DECISIONS.md#9-self-packaging-via-appended-zip)
for full reference + rationale.

## How it works

- [spec/DESIGN_DECISIONS.md § 9](../spec/DESIGN_DECISIONS.md#9-self-packaging-via-appended-zip) — the appended-ZIP design and why it was chosen
- [CLI_REFERENCE § 3](../CLI_REFERENCE.md#3-self-packaging-subcommands) — every subcommand and flag
