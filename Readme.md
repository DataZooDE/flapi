<p align="center">
  <img src="docs/assets/logo.svg" alt="flAPI logo" width="80" height="80">
</p>

<h1 align="center">flAPI</h1>
<p align="center"><strong>SQL → REST APIs and MCP tools, from one config</strong></p>

<p align="center">
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-BSL%201.1-blue.svg" alt="License: BSL 1.1"></a>
  <a href="https://duckdb.org"><img src="https://img.shields.io/badge/DuckDB-2.0.1-green.svg" alt="DuckDB 2.0.1"></a>
  <a href="https://github.com/DataZooDE/flapi/actions/workflows/build.yaml"><img src="https://github.com/DataZooDE/flapi/actions/workflows/build.yaml/badge.svg?branch=main" alt="Build"></a>
  <a href="https://pypi.org/project/flapi-io/"><img src="https://img.shields.io/pypi/v/flapi-io.svg?label=PyPI%20flapi-io" alt="PyPI flapi-io"></a>
  <a href="docs/MCP_REFERENCE.md"><img src="https://img.shields.io/badge/MCP-2026--07--28-orange.svg" alt="MCP 2026-07-28"></a>
  <a href="#-claude-code-plugin"><img src="https://img.shields.io/badge/Claude_Code-plugin-8A2BE2.svg" alt="Claude Code plugin"></a>
</p>

Write a SQL template and a few lines of YAML. flAPI serves the result as a REST endpoint **and** as an MCP tool for AI agents. Both use the same parameter validators and the same cache, and each has its own access control. It ships as one binary with [DuckDB](https://duckdb.org/) inside, so it reads Parquet, CSV, Postgres, BigQuery, S3 and 50+ more sources.

> **🤖 Using Claude Code?** flAPI ships a [Claude Code plugin](#-claude-code-plugin) with skills for scaffolding projects, writing endpoints and MCP tools, designing caches, and deploying and debugging flAPI. CI checks every example in the skills against the real binary.
>
> Install with `/plugin marketplace add DataZooDE/flapi`, then `/plugin install flapi@flapi`.

![flAPI demo: one SQL template served as a REST endpoint and an MCP tool](docs/promotion/assets/flapi-demo.gif)

## ✨ Key Features

### 🔌 REST and MCP from one config
*Guides: [Building a REST endpoint](docs/guides/rest-endpoints.md) · [Creating MCP tools](docs/guides/mcp-tools.md)*

- **One file, two protocols**: `url-path` makes a REST endpoint, and `mcp-tool` makes the same SQL an MCP tool. `mcp-resource` and `mcp-prompt` complete the set
- **Current MCP (`2026-07-28`) and older clients on the same endpoint**, with long-running tools (Tasks), typed results and OAuth discovery
- **Agent safety**: roles per tool, dry runs, per-tool rate limits, and a scanner that flags prompt injection in tool descriptions
- **OpenAPI 3.0** document at `/doc.yaml` and a Swagger UI at `/doc`

### 🗄️ Data sources
*Guides: [Connecting data sources](docs/guides/data-sources.md) · [Reading from cloud storage](docs/guides/cloud-storage.md)*

- **Everything DuckDB reads**: Parquet, CSV, JSON, [Iceberg](https://github.com/duckdb/duckdb-iceberg), Delta, [Postgres](https://github.com/duckdb/duckdb-postgres), [MySQL](https://github.com/duckdb/duckdb-mysql), SQLite, [BigQuery](https://github.com/hafenkran/duckdb-bigquery), SAP ERP and BW (via [ERPL](https://github.com/datazoode/erpl)), and S3 / GCS / Azure / HTTP
- **Configs and templates can live in object storage**: `flapi -c s3://bucket/flapi.yaml` (also `gs://`, `az://`, `https://`)

### 🧩 SQL templates and validation
*Guide: [Writing SQL templates](docs/guides/sql-templates.md)*

- **Mustache templates** with `params.*`, `conn.*`, `auth.*` and whitelisted `env.*`
- **Typed parameters are bound as prepared statements**: a `{{ params.x }}` (or `{{{ }}}` at a value site) on a typed field, outside a `{{#…}}` section, becomes a bound `?`, so it cannot inject SQL. Inside a section, the validator is the defence
- **Validators** for int, number, boolean, string (length, regex), email, uuid, enum, date and time. A bad request gets a 400 before any SQL runs
- **Writes** (POST/PUT/PATCH/DELETE) with `RETURNING` rows and cache invalidation

### ⚡ Caching (DuckLake)
*Guide: [Caching an expensive query](docs/guides/caching.md)*

- **Full refresh, append or merge**, inferred from `cursor` / `primary-key`, on a schedule
- **Snapshots and time travel**, retention and garbage collection, refresh on demand (`flapii cache refresh`), and prebuilt caches for serverless

### 🔐 Security
*Guide: [Authentication and authorization](docs/guides/authentication.md)*

- **Auth**: Basic, Bearer/JWT and OIDC, with roles per endpoint, PBKDF2 password hashes, users from AWS Secrets Manager, and `{{env.X}}` for every secret
- **Hardened by default where it matters**: SQL templates of a remote config run with DuckDB external access restricted (`duckdb-hardening`), so they can't read other files, buckets or hosts
- CORS allowlist, per-user rate limits, a JSONL audit log, direct TLS, and a config auditor that runs at startup

### 🔭 Observability
*Guide: [Observability](docs/guides/observability.md)*

- **OpenTelemetry tracing**: one span per request with the query inside, joined to the calling agent's trace
- **`request_id`** on every response and log line, runtime metrics, `/health` and `/health/live`
- **`log-http-requests`** logs every HTTP request DuckDB makes (S3, GCS, HTTP), with userinfo and query strings redacted (a debugging switch)

### 📦 Deployment
*Guides: [Shipping one self-contained binary](docs/guides/self-packaging.md) · [Serverless](docs/guides/serverless.md) · [Managing flAPI from the command line](docs/guides/cli.md)*

- **One binary**, with the DuckDB extensions it needs shipped next to it in `extensions/`
- **[Self-packaging](docs/guides/self-packaging.md)**: `flapi pack` folds the config, templates and small data files, and optionally the extensions (`--with-extensions`), into the binary itself. Then `scp` is the whole deploy. Secrets stay out: `pack` refuses `*.env`, `.env.*`, `secrets/*`, `*.pem` and `*.key`. Builds are reproducible (`SOURCE_DATE_EPOCH`), and macOS builds can be notarised
- **Docker** (`ghcr.io/datazoode/flapi`), **[scale-to-zero platforms](docs/guides/serverless.md)** (Cloud Run, App Runner, Container Apps, Knative) and **PyPI** (`flapi-io`)

## 📋 Table of Contents

- [Key Features](#-key-features)
- [Claude Code plugin](#-claude-code-plugin)
- [Installation](#-installation)
- [Quick Start](#-quick-start)
- [One config, two protocols](#-one-config-two-protocols)
- [Documentation](#-documentation)
- [Building from source](#-building-from-source)
- [Telemetry](#-telemetry)
- [License](#-license)
- [Contributing](#-contributing) · [Support](#-support) · [Citation](#-citation) · [Feedback](#feedback)

## 🤖 Claude Code plugin

The repository is also a [Claude Code](https://claude.com/claude-code) plugin marketplace:

```
/plugin marketplace add DataZooDE/flapi
/plugin install flapi@flapi
```

| Phase | Skills |
|---|---|
| Setup | `flapi-setup`: install, project init, connections, env whitelist, validation |
| Develop | `flapi-rest`, `flapi-mcp`, `flapi-auth`, `flapi-testing`, `flapi-live-config` |
| Caching | `flapi-cache-design`: full / append / merge, deletes, write-through, replicas, prebuilt caches, time travel. `flapi-cache-tuning`: measuring and shortening refresh time |
| Deploy | `flapi-deploy`: `flapi pack`, Docker, Cloud Run, offline extensions |
| Operate | `flapi-operate`: health, runtime metrics, logs, tracing, troubleshooting |

Slash commands: `/flapi:init`, `/flapi:new-endpoint`, `/flapi:add-cache`, `/flapi:auth-setup`,
`/flapi:deploy-check`, `/flapi:diagnose`, `/flapi:metrics`.

The plugin also registers your running flAPI as an MCP server. It adds a hook that validates and hot-reloads the config after each edit. See [Using flAPI with Claude Code](docs/guides/claude-code.md). To try local changes, run `claude --plugin-dir ./plugins/flapi`.

## 📥 Installation

### PyPI (both commands, no Docker)

```bash
uvx --from flapi-io flapi --version     # run without installing
pip install flapi-io                     # or install: provides `flapi` (server) and `flapii` (CLI)
```

The package is `flapi-io` because "flapi" was taken on PyPI. It carries both the `flapi` server and the `flapii` CLI.

### Release binaries

[GitHub Releases](https://github.com/DataZooDE/flapi/releases) has archives for Linux (amd64, arm64), macOS (arm64) and Windows (amd64). Each archive holds the `flapi` binary and an `extensions/` directory with the DuckDB extensions it loads by default. Keep the two together.

### Docker

```bash
docker pull ghcr.io/datazoode/flapi:latest
docker run --rm -p 8080:8080 -v "$PWD/demo:/config" ghcr.io/datazoode/flapi -c /config/flapi.yaml
```

This serves the `demo/` project from the [Quick Start](#-quick-start) below. Mount any directory that holds a `flapi.yaml`.

`:latest` and `:v<version>` come from releases; `:main` and `:sha-<commit>` come from main. The image carries the default extensions. The `-runtime` variant (for example `:v26.11.25-runtime` or `:main-runtime`) also carries `bigquery`. One port (8080) serves both REST and MCP (`/mcp/jsonrpc`).

## 🚀 Quick Start

You need [uv](https://docs.astral.sh/uv/) and network access: the PyPI package downloads its DuckDB extensions on first start. CI runs these blocks against every build (with the freshly built binaries in place of `uvx`).

**1. Create a project.** This scaffolds `demo/`: a config, a sample CSV, and a `/sample` endpoint with an `id` filter.

<!-- flapi: quickstart run -->
```bash
uvx --from flapi-io flapii project init demo
```

**2. Make the endpoint an MCP tool too.** It's the same file and the same SQL; add three lines:

<!-- flapi: quickstart run -->
```bash
cat >> demo/sqls/sample.yaml <<'EOF'
mcp-tool:
  name: get_sample
  description: Look up sample products, optionally by id
EOF
```

**3. Start the server.** It runs in the foreground on port 8080; stop it with Ctrl+C. It prints a security warning that MCP has no auth yet, which is expected for a local demo; see [Authentication](docs/guides/authentication.md).

<!-- flapi: quickstart serve -->
```bash
uvx --from flapi-io flapi -c demo/flapi.yaml
```

**4. Call it, in a second terminal: once over REST, once as an agent would over MCP.**

<!-- flapi: quickstart try expect="Gadget" -->
```bash
curl -s 'http://localhost:8080/sample?id=2'
# {"data":[{"id":2,"name":"Gadget","category":"tools","price":19.5}],"next":"","total_count":1}
```

<!-- flapi: quickstart try expect="structuredContent" -->
```bash
curl -s http://localhost:8080/mcp/jsonrpc -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"get_sample","arguments":{"id":"2"}}}'
# {"jsonrpc":"2.0","id":1,"result":{"content":[...],"structuredContent":{"rows":[{"id":2,"name":"Gadget",...}],"row_count":1}}}
```

The Swagger UI is at [http://localhost:8080/doc](http://localhost:8080/doc). To connect Claude Code to the tool, run `claude mcp add --transport http flapi http://localhost:8080/mcp/jsonrpc`.

**Next:** [Getting started](docs/guides/getting-started.md) walks through your own data, parameters and validation.

## 🧭 One config, two protocols

One endpoint file serves `GET /customers` and the MCP tool `get_customers`. REST and MCP are protected separately: the endpoint's `auth:` guards REST, and the top-level `mcp.auth` with the tool's `allowed-roles` guards `tools/call`.

```yaml
# sqls/customers.yaml
url-path: /customers
mcp-tool:
  name: get_customers
  description: Retrieve customers, optionally by id
  allowed-roles: [reader]          # who may call the tool (with mcp.auth enabled)

request:
  - field-name: id
    field-in: query
    required: false
    validators:
      - type: int
        min: 1

template-source: customers.sql
connection: [customers-parquet]

auth:                              # REST only
  enabled: true
  type: basic
  users:
    - username: reader
      password: '{{env.READER_PASSWORD}}'   # never a literal password; the variable may hold a
                                            # $pbkdf2-sha256$ hash, and must match template.environment-whitelist
      roles: [reader]
```

```sql
-- sqls/customers.sql
SELECT * FROM read_parquet('{{{ conn.path }}}')
WHERE 1=1
{{#params.id}}
  AND customer_id = {{ params.id }}
{{/params.id}}
```

GET endpoints are paginated by default (`?limit=` / `?offset=`, 100 rows per page), so the template has no `LIMIT`. MCP is on by default on the same port (`mcp: {enabled: false}` turns it off; `GET /mcp/health` reports its state). Files can also define an `mcp-resource` or `mcp-prompt` alone. For the `mcp.auth` block, see [Creating MCP tools](docs/guides/mcp-tools.md#protect-it).

## 📚 Documentation

**Start at the [documentation index](docs/README.md).** It routes you from what you're trying to do to the right page.

| I want to… | Guide |
|---|---|
| Serve my first endpoint | [Getting started](docs/guides/getting-started.md) |
| Add validation, filters, pagination | [Building a REST endpoint](docs/guides/rest-endpoints.md) |
| Write templates that are safe and fast | [Writing SQL templates](docs/guides/sql-templates.md) |
| Connect Postgres, SQLite, BigQuery, Snowflake, files … | [Connecting data sources](docs/guides/data-sources.md) |
| Let an AI agent call it | [Creating MCP tools for AI agents](docs/guides/mcp-tools.md) |
| Make a slow query fast | [Caching an expensive query](docs/guides/caching.md) |
| Require a login, restrict by role, filter rows per caller | [Authentication and authorization](docs/guides/authentication.md) |
| Read from S3, GCS or Azure | [Reading from cloud storage](docs/guides/cloud-storage.md) |
| Stop copy-pasting config blocks | [Reusing config and reading the environment](docs/guides/yaml-includes.md) |
| Deploy one self-contained binary | [Shipping one self-contained binary](docs/guides/self-packaging.md) |
| Run on Cloud Run and other scale-to-zero platforms | [Serverless](docs/guides/serverless.md) |
| Debug a failed request, probe health, trace, audit | [Observability](docs/guides/observability.md) |
| Manage a running server from the shell or CI | [Managing flAPI from the command line](docs/guides/cli.md) |
| Let Claude Code build and run it with me | [Using flAPI with Claude Code](docs/guides/claude-code.md) |

| Reference | Covers |
|---|---|
| [Configuration](docs/CONFIG_REFERENCE.md) | every `flapi.yaml` and endpoint key |
| [CLI](docs/CLI_REFERENCE.md) | server flags and subcommands (`pack`, `cache`, `extensions`, …) |
| [MCP](docs/MCP_REFERENCE.md) | the MCP protocol as flAPI implements it |
| [Config Service API](docs/CONFIG_SERVICE_API_REFERENCE.md) | runtime configuration over REST, used by `flapii` |
| [Architecture & design](docs/spec/ARCHITECTURE.md) | how it works inside, and why |
| [Changelog](CHANGELOG.md) | what changed in each release, with upgrade notes |

### MCP Registry

flAPI is listed in the official [MCP Registry](https://registry.modelcontextprotocol.io) under the name below (this line also serves as the registry's PyPI ownership marker):

mcp-name: io.github.datazoode/flapi

## 🏭 Building from source

flAPI is C++20 and builds much like [DuckDB](https://duckdb.org/docs/stable/dev/building/overview). The CI workflow [`build.yaml`](.github/workflows/build.yaml) and its build images (`.github/docker/`) are the reference for every platform.

On Linux, you need:

- A C++20 compiler, CMake, Ninja, `git`, `curl`, `zip`/`unzip`, `pkg-config`, `autoconf` and the OpenSSL headers. On Ubuntu:
  `sudo apt-get install -y build-essential cmake ninja-build git curl zip unzip pkg-config autoconf libssl-dev`
- [vcpkg](https://vcpkg.io), bootstrapped, with `VCPKG_ROOT` pointing at it. The build does not install vcpkg for you.

```bash
git clone --recurse-submodules https://github.com/DataZooDE/flapi.git
cd flapi
make release            # build/release/flapi
make test               # C++ unit tests
```

DuckDB v2.0.1 is a git submodule. vcpkg provides `crow`, `argparse`, `jwt-cpp`, `fmt`, `libarchive`, `curl`, `zstd`, `lz4`, `catch2`, `aws-sdk-cpp` and `opentelemetry-cpp`; `crow` and `opentelemetry-cpp` use the overlay ports in [`ports/`](ports/). `yaml-cpp` comes in through CMake. The [`Makefile`](Makefile) lists the integration-test targets.

## 📊 Telemetry

flAPI sends anonymous, aggregate usage events: `server_started` at boot, `$exception` on a failed request, and at most once a minute per route and status a count with durations (REST, MCP tool and auth outcomes). It sends route templates, never filled request paths, query data, SQL, credentials or personal information. [TELEMETRY.md](TELEMETRY.md) lists every property.

**Opt out** (any one is enough):

```bash
flapi --no-telemetry -c flapi.yaml   # CLI flag
export DATAZOO_DISABLE_TELEMETRY=1   # or FLAPI_NO_TELEMETRY=1
```

```yaml
telemetry:          # flapi.yaml
  enabled: false
```

## 📄 License

**Business Source License 1.1 (BSL 1.1)**

✅ **Free for production use**: use it inside your business
✅ **Free to copy, modify and build on**
✅ **Free for development and research**

❌ **Not offered to third parties on a hosted or embedded basis** without a commercial license
❌ **No rights to the flAPI or DataZoo trademarks**

🔄 **Converts to MPL 2.0** on each version's Change Date (see [LICENSE](LICENSE))

For a commercial license (embedding flAPI in a product, or offering it as a hosted service), contact [contact@data-zoo.de](mailto:contact@data-zoo.de). The [LICENSE](LICENSE) file has the full terms. Licensor: DataZoo GmbH.

## 🤝 Contributing

We welcome contributions. [Open an issue](https://github.com/DataZooDE/flapi/issues) to discuss a change, or send a pull request. [CLAUDE.md](CLAUDE.md) describes the code layout, conventions and test targets.

## 📞 Support

- **Documentation**: [docs/](docs/README.md) and [flapi.io](https://flapi.io)
- **Issues**: [GitHub Issues](https://github.com/DataZooDE/flapi/issues)
- **Commercial support and licensing**: [contact@data-zoo.de](mailto:contact@data-zoo.de)

## 🎓 Citation

```bibtex
@software{flapi,
  title  = {flAPI: SQL to REST APIs and MCP tools},
  author = {Joachim Rosskopf and DataZoo GmbH},
  year   = {2026},
  url    = {https://github.com/DataZooDE/flapi}
}
```

## 🏆 Acknowledgments

Built on [DuckDB](https://duckdb.org), [DuckLake](https://ducklake.select), [Crow](https://github.com/CrowCpp/Crow), [yaml-cpp](https://github.com/jbeder/yaml-cpp), [jwt-cpp](https://github.com/Thalhammer/jwt-cpp), [OpenTelemetry C++](https://github.com/open-telemetry/opentelemetry-cpp) and the [Model Context Protocol](https://modelcontextprotocol.io).

---

## Feedback

If flAPI misbehaves (an endpoint that won't serve, a cache that won't invalidate, an auth flow that won't complete), please
[open an issue](https://github.com/DataZooDE/flapi/issues). Deployments differ in ways we
can't reproduce here, so a report with your config is the fastest way to a fix. A failed
request answers `500` with a `request_id`. The details are in the server log under that id,
so quote the matching log lines in the report.

If it saved you time, a star on the repo helps other people find it.

On an interactive start, a small banner says the same thing once a day. Under a container
or systemd there is no terminal, so it never prints; the startup log line carries the
pointer instead. Silence both with `DATAZOO_NO_BANNER=1`.
