# flAPI Submission Kit

Canonical, **fact-checked** metadata and per-channel instructions for executing
[PROMOTION_STRATEGY.md](../PROMOTION_STRATEGY.md). Every claim below was
verified against the source tree on 2026-07-13 and **re-verified on 2026-09-26
against v26.09.23**. Use these blurbs verbatim — they are worded to survive
skeptical audiences (HN, r/dataengineering).

> **⚠️ Two claims from the playbook were corrected during verification:**
> 1. *"SQL injection structurally impossible"* is only true **for typed
>    params** (typed validators → `?` placeholders → DuckDB prepared
>    statements, `src/prepared_template_rewriter.cpp`,
>    `src/query_executor.cpp`). Triple-brace `{{{ }}}`, untyped params and
>    section-interpolated values still rely on validators. Always scope the
>    claim.
> 2. flAPI is **BSL 1.1**: production use is permitted, **except offering it
>    to third parties as a hosted or embedded service**; it converts to
>    MPL 2.0 five years after first publication. It is *source-available*,
>    **not open source**. Never say "open source" in any submission — this is
>    the #1 predictable flame on HN/r/opensource — and never say "production
>    use permitted" without the hosting carve-out.
>
> **Corrected in the 2026-09-26 re-verification:** the language/DuckDB versions
> (C++20, DuckDB 1.5.5 — not C++17, 1.5.3); the MCP row no longer claims SSE
> streaming, which flAPI does not serve; the ports row no longer cites
> `mcp.port`, a key that is parsed but never read (MCP shares the HTTP port);
> the licence rows now carry the hosting carve-out; and three of the four
> launch blockers were already resolved.

---

## Canonical metadata (copy-paste)

| Field | Value |
|---|---|
| Name | flAPI |
| Repo | https://github.com/DataZooDE/flapi |
| One-liner | Turn SQL templates + YAML into REST endpoints **and** MCP tools — one static binary, DuckDB inside. |
| Install | `uvx --from flapi-io flapi -c flapi.yaml` · `pip install flapi-io` · GitHub-release binaries (Linux x86_64/ARM64, macOS ARM64, Windows) · Docker |
| MCP transport | **Streamable HTTP** at `/mcp/jsonrpc`, JSON responses (no SSE streaming). **No stdio** — stdio-only clients need a proxy such as `mcp-remote`. |
| Ports | One port serves both REST and MCP: `8080` by default (`http-port`, `-p/--port`, or `FLAPI_PORT`) |
| Auth | Basic + JWT/OIDC; **per-tool RBAC**, fail-closed (tool without `allowed-roles` under auth = denied) |
| Tools | Dynamic — every configured endpoint with an `mcp-tool` block becomes a tool; flAPI is a *server generator*, not a fixed tool list |
| Security | Typed params bound as DuckDB prepared statements (injection structurally impossible *at bound sites*); typed validators (int/string/email/uuid/enum/date…); tool-description hygiene scanner (flags injection phrases in YAML tool descriptions at config load) |
| Caching | DuckLake: full/incremental refresh, snapshot time-travel |
| Data sources | Parquet/CSV, Postgres, BigQuery, S3/GCS/Azure, Iceberg, Delta + 50+ via DuckDB extensions; SAP ERP/BW via ERPL (demo in `examples/sqls/sap/`, stability caveats documented) |
| Language | C++20, single static binary, embedded DuckDB 1.5.5 |
| License | **BSL 1.1** → MPL 2.0 after five years; Additional Use Grant permits production use **except offering it to third parties as a hosted or embedded service** ("source-available", not open source) |
| Latest release | see [GitHub releases](https://github.com/DataZooDE/flapi/releases/latest) — not pinned here, so it cannot go stale |
| Honest limitations | Read-oriented data APIs (not a general CRUD backend) · DuckDB-centric · MCP over HTTP only · BSL license · young project |

**Long blurb (directories):**

> flAPI turns SQL templates and YAML configuration into governed REST
> endpoints and MCP tools — from the same config, with the same validators,
> RBAC and caching. It ships as a single static C++ binary with DuckDB
> embedded, so one `scp` (or `uvx --from flapi-io flapi`) puts a REST + MCP
> server in front of Parquet files, Postgres, BigQuery, S3 and 50+ other
> sources. Typed request parameters are bound as DuckDB prepared statements;
> per-tool RBAC is enforced fail-closed; results can be cached with
> DuckLake snapshots. Source-available under BSL 1.1: production use is
> permitted, except offering it to third parties as a hosted service.

---

## Launch blockers — status as of 2026-09-26

One of the four original blockers remains. The other three were fixed after
this kit was first written; each was re-checked against the live artefact,
not just the source.

1. ✅ **Wheel license metadata** — resolved. The release workflow passes
   `--license BUSL-1.1` to bin-to-wheel, and PyPI shows `BUSL-1.1` for
   `flapi-io`.
2. ✅ **Demo GIF** — resolved. Five demos in `assets/` (REST + MCP, agent,
   BigQuery, SharePoint, self-packaging), and `flapi-demo.gif` is embedded in
   the README.
3. 🔴 **README doesn't lead with MCP** — still open. The title is still
   "flAPI: Instant SQL based APIs" and the opening paragraph describes
   "read-only APIs" and REST only. The demo GIF's caption mentions MCP; the
   headline and first paragraph do not. (Note also that "read-only" undersells
   it: write endpoints exist.)
4. ✅ **MCP Registry PyPI validation** — resolved. `mcp-name:
   io.github.datazoode/flapi` is in `Readme.md`, which becomes the PyPI
   description, and it is live on PyPI now.

---

## Channel-by-channel

### 1. Official MCP Registry — `server.json` ✅ prepared, version automated
- **Publish the `server.json` attached to the GitHub release, not the repo copy.** The repo
  copy is a template with version `0.0.0-dev`. Each release job stamps the real version
  into a copy and attaches it to the release, reading the version from the wheel files it
  just built, so it always matches what PyPI serves (e.g. `26.9.23`, never `26.09.23`).
  CI fails if anyone hand-edits the repo copy back to a real version.
- Prereq met: the `mcp-name` marker is live on PyPI (blocker 4, resolved). Then:
  ```bash
  brew install mcp-publisher   # or download from modelcontextprotocol/registry releases
  mkdir -p /tmp/mcp-publish && cd /tmp/mcp-publish
  gh release download --repo DataZooDE/flapi --pattern server.json   # latest release
  mcp-publisher login github   # authenticates the io.github.datazoode namespace
  mcp-publisher publish        # reads ./server.json - the stamped one downloaded above
  ```
- **`v26.09.23` and earlier have no `server.json` asset** — stamping was added
  after that release, so the first release to carry it is the next one. Until
  then, stamp it locally with the same script CI uses, from that release's own
  wheel (every flapi-io wheel carries the same version; one is enough).
  Verified on 2026-09-26 to produce `26.9.23`:
  ```bash
  # from a checkout of the repo
  gh release download v26.09.23 --repo DataZooDE/flapi \
      --pattern 'flapi_io-*-macosx_11_0_arm64.whl' -D /tmp/flapi-wheel
  python3 scripts/stamp_server_json.py stamp --wheels-dir /tmp/flapi-wheel \
      --in server.json --out /tmp/mcp-publish/server.json
  cd /tmp/mcp-publish && mcp-publisher login github && mcp-publisher publish
  ```
- Running `mcp-publisher publish` from the repo root would submit the `0.0.0-dev`
  template. Always publish from the downloaded release asset.

### 2. punkpeye/awesome-mcp-servers — ⏸ CLOSED unmerged 2026-09-07, **blocked on Glama (#6)**
- [PR #10023](https://github.com/punkpeye/awesome-mcp-servers/pull/10023) was
  closed for 30 days' inactivity. The list now requires every entry to be
  **listed on Glama, claimed by the owner, with a quality score** (any grade),
  and the entry must carry the Glama score badge. That was never done.
- To resubmit: complete #6 below, then open a **new** PR — the entry below,
  plus the badge right after the GitHub URL. Use the badge format from the
  bot comment on #10023:
  `[![DataZooDE/flapi MCP server](https://glama.ai/mcp/servers/DataZooDE/flapi/badges/score.svg)](https://glama.ai/mcp/servers/DataZooDE/flapi)`
  (confirm the exact Glama path once the listing exists).
- The original submission, kept for reuse:
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

### 3. wong2/awesome-mcp-servers (mcpservers.org) — ✅ SUBMITTED 2026-07-13 via web form (review ≤12h, confirmation to jr@data-zoo.de) — **listing not confirmed**
- As of 2026-09-26 flAPI does not appear in the wong2 README, and
  mcpservers.org blocks automated checks (HTTP 403), so the listing could not
  be verified. Check the site by hand, and the confirmation email.
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

### 6. Glama — needs human (claim listing) — **now a prerequisite for #2**
- Glama auto-indexes from GitHub; check https://glama.ai/mcp/servers for
  an existing flAPI entry and claim it with the GitHub org account. Note:
  Glama badges emphasise "open-source" — if asked, say *source-available
  (BSL 1.1)*.
- For a score, Glama builds the server **from a Dockerfile you add on Glama**
  and requires it to start and answer MCP introspection. flAPI will not start
  without a `flapi.yaml`, so that Dockerfile must bake in a small demo config
  with `mcp.enabled: true` and at least one `mcp-tool`. No demo project here
  is ready as-is: `assets/demo-projects/pack-myapi` is the closest (its data
  is a bundled CSV), but it has no `mcp:` block or `mcp-tool`, and it reads
  `embed://data/customers.csv`, which only resolves inside a binary built with
  `flapi pack`. Either add the MCP config and point the connection at a plain
  file path, or `flapi pack` it and run the packed binary. The repo's own
  `Dockerfile` is not usable for this: it expects prebuilt binaries from the
  CI build context.

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

### 13. Hacker News — draft ready, **prereq: README leads with MCP** (GIF ✅, license ✅)
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

*Generated 2026-07-13; re-verified 2026-09-26 against v26.09.23. Claims
verified against source and, for PyPI and third-party listings, against the
live state. Re-verify before each submission if the code has moved.*
