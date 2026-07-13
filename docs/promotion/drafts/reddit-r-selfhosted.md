<!--
Target: r/selfhosted
Timing: Week 3+ (playbook §8), after the Show HN. Do NOT post before the
account has 2-3 weeks of genuine participation in the sub (§3 — "the
account matters more than the post").
Prerequisites: read the sub's self-promo rules on the day of posting;
r/selfhosted cares a lot about licensing — BSL is disclosed in the first
lines below on purpose, do not bury it. Stay in the comments.
-->

# Title

flAPI — a single static binary that turns SQL files into a REST API (config can be baked into the binary, scp is the deploy). Heads-up: BSL 1.1, source-available, not open source

# Body

Disclosure: I'm one of the authors. And before anything else, because this
sub rightly cares: **flAPI is licensed under the Business Source License
1.1.** That is source-available, *not* open source. You can read, modify,
and self-host it, and production use is permitted under the Additional Use
Grant — what's restricted is offering flAPI itself to third parties as a
hosted service or embedding it in a product you sell. Each version
converts to MPL-2.0 five years after publication. If BSL is a dealbreaker
for you, that's a completely fair position and I'd point you at ROAPI
(Apache-2.0) or PostgREST (MIT) instead.

Still here? The self-hosting story is the part I think you'll like.

**What it is:** you write a SQL file (Mustache-templated) and a YAML
config; flAPI serves it as a REST endpoint (and an MCP tool for AI
clients, from the same config). DuckDB is embedded, so the SQL can read
Parquet/CSV on disk, Postgres, S3/GCS/Azure, BigQuery, Iceberg, Delta —
50+ sources. Auth (Basic/JWT with roles), validation, rate limiting, and
caching are config, not code.

**Why it fits this sub:**

- **One static C++ binary.** No runtime, no interpreter, no node_modules,
  no Python venv. Linux x86_64/ARM64, macOS ARM64, Windows. Docker image
  exists if you prefer, but it's genuinely optional.
- **`flapi pack` bakes the config in.** `flapi pack --in ./myconfig --out
  flapi-prod` appends your entire config tree (YAML + SQL + small data
  files) as a ZIP after the executable (on macOS it goes into a reserved
  Mach-O segment and gets re-codesigned, so it stays notarisable). The
  result is one file. `scp flapi-prod user@host` *is* the deployment.
- **Secrets can't end up in the artifact.** Pack refuses `*.env`,
  `*.pem`, `*.key`, `secrets/*` by default; credentials come from
  environment variables at runtime.
- **Reproducible.** Set `SOURCE_DATE_EPOCH` and packing is byte-identical
  across runs — you can diff your deploy artifacts.
- **Telemetry disclosure:** it sends anonymous start/stop events by
  default. Opt out with `--no-telemetry`, `FLAPI_NO_TELEMETRY=1`, or in
  the YAML. I know how this sub feels about telemetry, so: it's
  documented, it's one flag, and no query data or credentials are ever
  sent.

Quick try without downloading anything:

```
uvx --from flapi-io flapi -c flapi.yaml
```

(PyPI package is `flapi-io` because `flapi` was taken.)

**Limitations, honestly:**

- Read-oriented data APIs. It will not be your app's CRUD backend.
- DuckDB-centric — great if your data is files/warehouses, wrong tool if
  you need MySQL-triggers-and-transactions territory.
- MCP is HTTP-only (no stdio transport).
- BSL 1.1, as covered up top.
- Young project; expect rough edges and file issues when you hit them.

Repo: https://github.com/DataZooDE/flapi

Happy to answer questions about the pack/bundle mechanics or resource
footprint on small boxes.
