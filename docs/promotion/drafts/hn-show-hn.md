<!--
Target: Hacker News (Show HN)
Timing: Week 3 of the launch sequence (playbook §8). Weekday, ~8-10am ET.
Prerequisites: README polished, 60-second GIF live, uvx one-liner front and
center in the README. Author must be available for 4-6 hours after posting
to answer every comment. Same day: cross-post to r/mcp and r/DuckDB, pitch
newsletters. This is realistically one shot — do not post until the repo
is ready.
-->

# Show HN submission

## Title

```
Show HN: flAPI – Turn one SQL file into a REST endpoint and an MCP tool
```

## URL

```
https://github.com/DataZooDE/flapi
```

---

## First comment (post immediately after submitting, from the author account)

Author here. flAPI takes a SQL file (Mustache-templated) plus a small YAML
config and serves it as a REST endpoint and an MCP tool from the same
definition — one binary, no backend code. DuckDB 1.5.3 is embedded, so the
SQL can hit Parquet/CSV, Postgres, BigQuery, S3/GCS/Azure, Iceberg, Delta,
and the rest of DuckDB's 50+ source ecosystem. There's even an SAP ERP/BW
path via the ERPL extension, though that one still has documented stability
caveats.

Quickest way to try it:

    uvx --from flapi-io flapi -c flapi.yaml

(The package is `flapi-io` because `flapi` was taken on PyPI.)

Some build decisions people here might find interesting:

**Why C++ and a single static binary.** The target user is a data analyst
or a small data team, and the deploy target is often "a VM somewhere" with
no container registry and no Python environment anyone trusts. A statically
linked C++17 binary with DuckDB embedded means the entire runtime is one
file. No interpreter, no virtualenv, no shared-library roulette. It also
keeps the request path short: parameter validation, template rendering, and
query execution all happen in-process.

**SQL injection: what we actually do.** Request parameters that carry a
typed validator (int, double, boolean, date, time, uuid, enum, email,
string) are not string-interpolated into the SQL at all — the template
reference is rewritten to a `?` placeholder and the value is bound through
a DuckDB prepared statement. For those sites, injection is structurally
impossible: the value never touches the SQL text. I want to be precise
about the boundary, because this is HN: untyped parameters, triple-brace
`{{{ }}}` raw interpolation, and values used inside Mustache sections still
rely on validators and template discipline. The honest claim is "typed
params are bind-parameters," not "the whole templating system is immune."

**The self-packaging trick.** `flapi pack --in ./examples --out flapi-prod`
folds your whole config tree (YAML + SQL templates + small data files) into
the binary itself. On Linux/Windows it's the classic appended-ZIP: archive
after the executable image, located at startup by reverse-scanning for the
EOCD signature. macOS was the fun part — appended data breaks code signing,
so we reserve a 16 MiB `__FLAPI/__bundle` Mach-O segment at link time,
write the ZIP into it at pack time, and re-codesign, so the output stays
notarisable. Bundled data files are served to DuckDB through an `embed://`
filesystem, so `read_csv('embed://data/cities.csv')` reads from memory.
`SOURCE_DATE_EPOCH` makes the pack reproducible (byte-identical output),
and pack refuses `*.env`, `*.pem`, `*.key`, `secrets/*` so credentials
stay in the environment where they belong.

**Honest limitations, so you don't have to dig for them:**

- It's for read-oriented data APIs. It is not a CRUD backend and doesn't
  want to be one.
- It's DuckDB-centric. If DuckDB can't reach your source, neither can we.
- MCP is Streamable HTTP only (`/mcp/jsonrpc`, SSE for streaming). No
  stdio transport — stdio-only clients need a proxy like mcp-remote.
- License is Business Source License 1.1, not open source. Source-
  available, production use permitted under the Additional Use Grant,
  converts to MPL-2.0 after the Change Date. More below, since I know
  this will come up.
- It's a young project. The core paths are tested, but you will find
  rough edges.

Happy to go deep on any of this — the prepared-statement rewriting and the
Mach-O segment work were the two most interesting rabbit holes.

---

## Anticipated pushback — prepared Q&A

Keep these ready; answer in your own words in the thread, don't paste
verbatim walls.

### "Why not PostgREST?"

PostgREST is excellent if your data lives in Postgres and you want your
API to mirror your schema. flAPI differs in three ways: (1) the source is
anything DuckDB can read — Parquet on S3, BigQuery, Iceberg, a CSV, and
Postgres too — not one database; (2) the API surface is a curated set of
SQL templates you write, not a reflection of the schema, which matters when
you want a governed, deliberately narrow interface; (3) every endpoint is
also an MCP tool from the same config, with per-tool RBAC. If you're all-in
on Postgres and want schema-driven CRUD, PostgREST is the better tool.

### "Why not just FastAPI / Flask?"

You absolutely can, and for anything with real business logic you should.
flAPI's bet is that a large class of data APIs is "validated params → one
SQL query → JSON," and for that class the Python service is mostly
boilerplate you now own: validation, auth, rate limiting, caching, OpenAPI
docs, deployment. Here that's YAML config around a SQL file, plus things
you'd rarely build yourself (DuckLake snapshot caching with time-travel,
prepared-statement param binding, an MCP server). If your endpoint needs a
for-loop, use FastAPI.

### "Why not Datasette?"

Huge respect for Datasette — it pioneered a lot of this space. Different
goals though: Datasette is exploration-first (browse, facet, ad-hoc SQL
over SQLite), flAPI is contract-first (you define exactly which
parameterised queries are exposed, with typed validation, RBAC, and
caching in front). Also DuckDB vs SQLite as the engine, which changes
which sources you can federate. If you want to *explore* data, Datasette;
if you want to *publish a stable API* over it, that's our lane.

### "Why not ROAPI?"

Closest philosophical neighbour — also "config in, API out," also
single-binary, and it's Rust + Arrow/DataFusion, which is a great stack.
Differences as I see them: flAPI endpoints are arbitrary templated SQL you
author (not auto-generated query interfaces over registered tables), MCP
tools with per-tool RBAC come from the same config, DuckLake gives
scheduled full/incremental cache refresh with snapshot time-travel, and
`flapi pack` embeds the config into the binary. ROAPI is Apache-2.0, which
is a genuine point in its favour if license is your deciding factor.

### "Why not MXCP?"

MXCP is the closest competitor on the MCP side and it's good work. It's
Python and dbt-centric — if your team lives in dbt, that's a feature. flAPI
is one static binary with no Python runtime, serves REST and MCP from the
same file, and doesn't require dbt or any project scaffolding beyond YAML +
SQL. Different teams will reasonably pick differently.

### "BSL is not open source. Why should I care about a source-available tool?"

You're right, and we don't call it open source. The specifics: BSL 1.1,
each version converts to MPL-2.0 five years after publication, and the
Additional Use Grant permits production use — you can run flAPI in prod,
internally, for your APIs, today, free. What it restricts is offering
flAPI itself to third parties as a hosted service or embedding it in a
product you sell; that requires a commercial license.

Why we chose it, non-defensively: we're a small company (DataZoo GmbH) and
we want the code readable, patchable, and self-hostable while keeping
"cloud vendor rehosts it" off the table long enough to build a business
that funds development. That's a trade-off, not a virtue, and it's
reasonable to reject it — if BSL is a hard no for you, ROAPI (Apache-2.0)
or PostgREST (MIT) are genuinely good alternatives and I'll say so in the
thread. The delayed-open guarantee is the part we can offer honestly:
every version has a date at which it becomes MPL-2.0, no matter what
happens to us.

### "Isn't templating SQL with Mustache just asking for injection?"

For typed parameters, no template interpolation happens at all — the
reference becomes a `?` and the value is bound via a DuckDB prepared
statement, so it can't change the query structure. For everything else
(untyped params, raw `{{{ }}}` sites, section-controlled fragments), yes,
that's string templating and it relies on the validator layer (regex,
range, enum whitelists) plus template discipline. We document that boundary
rather than hand-wave it. The recommended pattern is: type every user-
facing parameter, and reserve raw interpolation for trusted values like
connection properties.

### "MCP over HTTP only? My client is stdio."

Correct — Streamable HTTP at `/mcp/jsonrpc` with SSE for streaming, no
stdio transport today. flAPI is a long-running server, so HTTP is the
natural fit, but stdio-only clients work through a proxy like mcp-remote.

### "What does the prompt-injection scanner actually do?"

Less than the name might suggest, deliberately. It scans MCP tool
descriptions in your YAML config at load time and flags patterns that look
like embedded prompt-injection attempts — hygiene against a poisoned or
carelessly copy-pasted config. It does not scan live traffic or model
output. Think "linter for tool descriptions," not "runtime firewall."

### "1000 req/sec on single-threaded DuckDB? / performance claims"

Don't lead with numbers; if pressed: typical simple cached GETs are in the
tens of milliseconds end-to-end, DuckDB parallelism is configurable, and
the DuckLake cache exists precisely so hot endpoints don't re-hit slow
sources. Offer to share reproducible benchmarks rather than asserting
throughput figures in-thread.
