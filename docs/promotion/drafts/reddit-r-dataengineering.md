<!--
Target: r/dataengineering
Timing: Week 3+ (playbook §8), after Show HN. This sub is skeptical of
hype (§3): engineering teardown framing, architecture first, link last.
Prerequisites: MANDATORY 2-3 weeks of genuine participation (answering
questions in the sub) before posting, or it gets nuked as spam. Read the
sub's self-promo rules on posting day. Stay in the comments for hours.
-->

# Title

How we built a governed read-only data API layer without a backend service — architecture teardown (SQL templates → prepared statements, DuckLake caching)

# Body

Disclosure: I'm one of the authors of the tool described (flAPI). This is
meant as an engineering write-up, not an ad — the design decisions apply
even if you build your own.

**The problem shape.** A large share of "we need an API for this data"
tickets are: validated parameters → one SQL query → JSON. The conventional
answer is a FastAPI/Flask service per team, and each one re-implements
validation, auth, rate limiting, caching, and OpenAPI docs around what is
ultimately a SELECT statement. We wanted that class of endpoint to be
config, not a service.

**Architecture.** A single C++17 binary with DuckDB 1.5.3 embedded. An
endpoint is two files: a YAML config (URL path, typed parameters with
validators, auth, rate limit, cache policy) and a Mustache-templated SQL
file. The request path is: parameter extraction → validator chain →
template render → DuckDB execution → JSON. Because DuckDB is the engine,
one endpoint can federate Parquet on S3, Postgres, BigQuery, Iceberg,
Delta, etc. The same YAML can also expose the query as an MCP tool for
LLM clients, with per-tool role checks (fail-closed: under auth, a tool
with no allowed-roles is denied).

**The injection question, scoped precisely.** Templated SQL rightly makes
people nervous, so here's exactly what we do. Parameters that carry a
typed validator (int, double, boolean, date, time, uuid, enum, email,
string) are *not* interpolated: the `{{ params.x }}` reference is
rewritten to a `?` placeholder and the value is bound through a DuckDB
prepared statement. For those sites, injection is structurally impossible
— the value never enters the SQL text. The honest boundary: untyped
parameters, raw triple-brace `{{{ }}}` sites, and Mustache
section-controlled fragments are still string templating and rely on the
validator layer (regex/range/enum whitelists) plus authoring discipline.
The mitigation is cultural + mechanical: type every user-facing param,
reserve raw interpolation for trusted values like connection properties.

**Caching via DuckLake.** Endpoint caches are DuckLake tables (snapshot-
based). YAML infers the sync mode: no keys → full refresh (CTAS); a
cursor column → incremental append; cursor + primary key → incremental
merge (upsert). Templates get `{{cache.previousSnapshotTimestamp}}` etc.,
so an incremental refresh is just `WHERE updated_at > TIMESTAMP
'{{cache.previousSnapshotTimestamp}}'`. Snapshots give you time-travel and
an audit trail; refreshes run on schedule or manual trigger, never on GET.
The practical win: a dashboard endpoint over a slow BigQuery query serves
from a local snapshot instead of costing you a warehouse scan per request.

**Deployment.** Single static binary; optionally `flapi pack` folds the
whole config tree into the binary itself (appended ZIP; reserved Mach-O
segment on macOS so signing survives), reproducible via
`SOURCE_DATE_EPOCH`, with a deny-list so secrets can't be packed. For a
lot of environments, "scp one file" beats a container pipeline.

**Where this approach loses.** Being upfront:

- Read-oriented only. No writes, no transactions, no business logic. If
  the endpoint needs a loop or a second system call, write a real service.
- DuckDB-centric. Sources DuckDB can't reach are out of scope. (There's
  an SAP ERP/BW path via the ERPL extension with a working example, but
  that extension has documented stability caveats.)
- The template layer's safety outside typed params is validator-dependent
  — it's defense-in-depth, not a proof.
- MCP transport is HTTP-only (no stdio).
- License is BSL 1.1 — source-available, not open source. Production use
  permitted; offering it as a hosted service is not. Converts to MPL-2.0
  per version after five years. If that's disqualifying for you, fair;
  ROAPI and PostgREST are good open-source neighbours.
- Young project.

Repo, if useful: https://github.com/DataZooDE/flapi — quickest trial is
`uvx --from flapi-io flapi -c flapi.yaml`.

Genuinely interested in this sub's take on two things: (1) where you draw
the line between "config-defined endpoint" and "just write the service,"
and (2) whether anyone has run DuckLake incremental merge at meaningful
scale — our experience is mostly mid-size tables.
