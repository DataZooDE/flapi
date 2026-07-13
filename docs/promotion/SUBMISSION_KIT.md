# flAPI Submission Kit

Canonical, **fact-checked** metadata and per-channel instructions for executing
[PROMOTION_STRATEGY.md](../PROMOTION_STRATEGY.md). Every claim below was
verified against the source tree on 2026-07-13. Use these blurbs verbatim —
they are worded to survive skeptical audiences (HN, r/dataengineering).

> **⚠️ Two claims from the playbook were corrected during verification:**
> 1. *"SQL injection structurally impossible"* is only true **for typed
>    params** (typed validators → `?` placeholders → DuckDB prepared
>    statements, `src/prepared_template_rewriter.cpp`,
>    `src/query_executor.cpp`). Triple-brace `{{{ }}}`, untyped params and
>    section-interpolated values still rely on validators. Always scope the
>    claim.
> 2. flAPI is **BSL 1.1** (converts to MPL-2.0; production use permitted).
>    It is *source-available*, **not open source**. Never say "open source"
>    in any submission — this is the #1 predictable flame on HN/r/opensource.

---

## Canonical metadata (copy-paste)

| Field | Value |
|---|---|
| Name | flAPI |
| Repo | https://github.com/DataZooDE/flapi |
| One-liner | Turn SQL templates + YAML into REST endpoints **and** MCP tools — one static binary, DuckDB inside. |
| Install | `uvx --from flapi-io flapi -c flapi.yaml` · `pip install flapi-io` · GitHub-release binaries (Linux x86_64/ARM64, macOS ARM64, Windows) · Docker |
| MCP transport | **Streamable HTTP** at `/mcp/jsonrpc` (SSE for streaming). **No stdio** — stdio-only clients need a proxy such as `mcp-remote`. |
| Ports | Unified REST + MCP server on `8080` (configurable; `mcp.port` default `8081`) |
| Auth | Basic + JWT/OIDC; **per-tool RBAC**, fail-closed (tool without `allowed-roles` under auth = denied) |
| Tools | Dynamic — every configured endpoint with an `mcp-tool` block becomes a tool; flAPI is a *server generator*, not a fixed tool list |
| Security | Typed params bound as DuckDB prepared statements (injection structurally impossible *at bound sites*); typed validators (int/string/email/uuid/enum/date…); tool-description hygiene scanner (flags injection phrases in YAML tool descriptions at config load) |
| Caching | DuckLake: full/incremental refresh, snapshot time-travel |
| Data sources | Parquet/CSV, Postgres, BigQuery, S3/GCS/Azure, Iceberg, Delta + 50+ via DuckDB extensions; SAP ERP/BW via ERPL (demo in `examples/sqls/sap/`, stability caveats documented) |
| Language | C++17, single static binary, embedded DuckDB 1.5.3 |
| License | **BSL 1.1** → MPL-2.0; Additional Use Grant permits production use ("source-available", not open source) |
| Latest release | v26.07.13 |
| Honest limitations | Read-oriented data APIs (not a general CRUD backend) · DuckDB-centric · MCP over HTTP only · BSL license · young project |

**Long blurb (directories):**

> flAPI turns SQL templates and YAML configuration into governed REST
> endpoints and MCP tools — from the same config, with the same validators,
> RBAC and caching. It ships as a single static C++ binary with DuckDB
> embedded, so one `scp` (or `uvx --from flapi-io flapi`) puts a REST + MCP
> server in front of Parquet files, Postgres, BigQuery, S3 and 50+ other
> sources. Typed request parameters are bound as DuckDB prepared statements;
> per-tool RBAC is enforced fail-closed; results can be cached with
> DuckLake snapshots. Source-available under BSL 1.1 (production use
> permitted).

---

## 🔴 Blocking gaps — fix before any launch

1. **Wheel license metadata is wrong.** `.github/workflows/build.yaml`
   passes `--license Apache-2.0` to bin-to-wheel, but the repo LICENSE is
   BSL 1.1. PyPI is publicly showing the wrong license. Fix before *any*
   publicity — this is exactly the kind of inconsistency HN finds in
   minutes. (Tracked in the promotion tracking issue.)
2. **No 60-second demo GIF** (playbook Week-0 item). The money shot:
   `uvx --from flapi-io flapi` → curl the endpoint → Claude calling the
   same endpoint as an MCP tool. Suggested tooling: `vhs` (the repo's
   visual-designer skill supports VHS terminal demos).
3. **README doesn't lead with MCP.** Title is "Instant SQL based APIs";
   the playbook says MCP is the most valuable 2026 angle. Move it up.
4. **MCP Registry PyPI validation.** The registry validates PyPI ownership
   by finding `mcp-name: io.github.datazoode/flapi` in the PyPI package
   README. Add that line to the bin-to-wheel description in the release
   workflow, then publish (see below).

---

## Channel-by-channel

### 1. Official MCP Registry — `server.json` ✅ prepared (repo root)
- Prereq: item 4 above (mcp-name marker on PyPI), then:
  ```bash
  brew install mcp-publisher   # or download from modelcontextprotocol/registry releases
  mcp-publisher login github   # authenticates the io.github.datazoode namespace
  mcp-publisher publish        # reads ./server.json
  ```
- Keep `version` in `server.json` in sync with releases (add to release checklist/workflow).

### 2. punkpeye/awesome-mcp-servers — ✅ SUBMITTED: [PR #10023](https://github.com/punkpeye/awesome-mcp-servers/pull/10023)
- File: `README.md`, **Databases** section, alphabetical (case-insensitive)
  — insert between `Dataring-engineering/mcp-server-trino` and
  `davewind/mysql-mcp-server`.
- Entry line:
  ```markdown
  - [DataZooDE/flapi](https://github.com/DataZooDE/flapi) 🎖️ 🌊 🏠 - Turns SQL templates + YAML into REST endpoints and MCP tools from one config — a single binary with embedded DuckDB (Parquet, Postgres, BigQuery, S3 and 50+ sources), per-tool RBAC, and typed parameters bound as prepared statements.
  ```
  (🎖️ official · 🌊 C/C++ · 🏠 local)
- PR title: `Add DataZooDE/flapi to Databases 🤖🤖🤖` — the trailing robots
  opt into their documented agent fast-track (CONTRIBUTING.md).
- PR body: entry line + "Disclosure: submitted on behalf of the flAPI
  maintainers (DataZooDE)."

### 3. wong2/awesome-mcp-servers (mcpservers.org) — ⚠️ no PRs accepted; use the web form
- Their README states: *"We do not accept PRs. Please submit your MCP on the
  website: https://mcpservers.org/submit"* (verified 2026-07-13 — a PR
  attempt is rejected by GitHub permissions).
- Form fields (no login, no captcha; free tier is fine — skip the $39
  "Premium Submit"):
  - Server Name: `flAPI`
  - Short Description: `Turn SQL templates + YAML into REST APIs and MCP tools — one static binary with embedded DuckDB (Parquet, Postgres, BigQuery, S3 and 50+ sources), per-tool RBAC, and DuckLake caching.`
  - Link: `https://github.com/DataZooDE/flapi`
  - Category: `Database`
  - Contact Email: maintainer address

### 4. mcp.so — needs human (web form)
- https://mcp.so → Submit. Paste the long blurb; category Database/Data
  Platform; note transport = streamable HTTP.

### 5. Smithery — needs human (account)
- https://smithery.ai → dashboard or `smithery` CLI. flAPI is a
  self-hosted HTTP server, so list it as a remote/self-hosted server, not
  a hosted stdio package.

### 6. Glama — needs human (claim listing)
- Glama auto-indexes from GitHub; check https://glama.ai/mcp/servers for
  an existing flAPI entry and claim it with the GitHub org account. Note:
  Glama badges emphasise "open-source" — if asked, say *source-available
  (BSL 1.1)*.

### 7. PulseMCP — needs human (web form)
- https://www.pulsemcp.com → "Submit" in top nav. Paste long blurb.

### 8. awesome-selfhosted — ⛔ mostly blocked by license
- Their `licenses.yml` (FOSS-only) has no BUSL entry. There *is* a
  `licenses-nonfree.yml` + a non-free (⊘) section; listing would require a
  PR adding `BUSL-1.1` there plus the entry, and non-free submissions get
  extra scrutiny. Possible but low ROI — deprioritise, or revisit if the
  license ever changes / a change-date passes to MPL-2.0.

### 9. AlternativeTo / LibHunt / SaaSHub — needs human (accounts, forms)
- Register flAPI as an alternative to: **PostgREST, Hasura, Datasette,
  ROAPI, MXCP, soul, prest**. Use the long blurb; license field:
  "BSL 1.1 (source-available)".

### 10. DuckDB Discord `#show-and-tell` — draft ready
- `drafts/discord-duckdb.md`. Friendliest first post; do this before HN.

### 11. dbt Community Forum "Show and Tell" — draft ready
- `drafts/dbt-forum.md`.

### 12. Reddit — drafts ready, **prereq: 2–3 weeks of genuine participation**
- `drafts/reddit-r-mcp.md`, `reddit-r-duckdb.md`, `reddit-r-selfhosted.md`,
  `reddit-r-dataengineering.md`, `reddit-r-opensource.md` (read its header —
  r/opensource is likely a skip due to BSL).
- Weekly thread-jack search queries: `sql query into an api`,
  `postgrest alternative`, `expose parquet rest`, `mcp server database`,
  `serve parquet http`.

### 13. Hacker News — draft ready, **prereq: GIF + README polish + license fix**
- `drafts/hn-show-hn.md` (title, first comment, pushback Q&A incl. BSL).
- Weekday 8–10am ET. Stay in the thread 4–6 hours.

### 14. AI newsletters — draft ready
- `drafts/newsletter-pitch.md` → Latent Space, Ben's Bites, TLDR AI,
  Data Engineering Weekly.

### 15. Stack Overflow / dev.to — ongoing, needs human
- Answer questions tagged `duckdb`/`parquet`/`rest`/`postgrest`/`mcp`
  fully first; flAPI as a P.S. with disclosure. Never copy-paste answers.

### 16. MotherDuck guest post / DuckDB community extension — needs human outreach
- Pitch the "MCP + DuckDB governance" angle to MotherDuck devrel.
- Community-extension idea: the `embed://` VFS is currently app-internal;
  extracting it is a real engineering project — treat as "investigate",
  not a quick listing.

### Blog posts (SEO/evergreen) — 2 of 6 drafted
- ✅ `drafts/blog-flagship-rest-plus-mcp.md` (the flagship narrative)
- ✅ `drafts/blog-security-typed-params.md` (HN/lobste.rs security piece)
- ☐ Comparison pages (vs PostgREST / Hasura / Datasette+ROAPI / MXCP)
- ☐ Recipe posts (BigQuery→MCP in 5 min; SAP BW→REST; DuckLake caching)
- ☐ Self-packaging engineering story (appended ZIP / Mach-O segment)

---

*Generated 2026-07-13. Claims verified against source; re-verify before
each submission if the code has moved.*
