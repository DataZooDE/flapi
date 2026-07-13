<!--
Target: dbt Community Forum, "Show and Tell" category
Timing: Week 2 of the launch sequence (playbook §8 — niche communities;
§6 notes MXCP announced here and got traction, so the audience is primed).
Prerequisites: forum account in good standing; be respectful of dbt and
of MXCP specifically — many readers use both. Reply to every comment.
-->

# Title

Show and Tell: flAPI — REST + MCP endpoints for your warehouse from plain SQL files (no Python, no dbt project required, single binary)

# Body

Hi all — disclosure up front: I'm one of the authors of flAPI
(https://github.com/DataZooDE/flapi).

Some of you will have seen MXCP announced here, which serves MCP tools
from dbt-adjacent SQL — it's good work, and if your team lives in dbt and
Python, it's a natural fit. flAPI sits in the same problem space with a
different set of trade-offs, and I think enough of you straddle both
worlds that it's worth showing.

**The idea:** you write a SQL file (Mustache-templated) and a YAML config;
flAPI serves it as a REST endpoint *and* an MCP tool from the same
definition. It's a single static C++ binary with DuckDB embedded — no
Python environment, no orchestration, no project scaffolding. Anything
DuckDB reaches is fair game: Parquet/CSV, Postgres, BigQuery, S3/GCS/Azure,
Iceberg, Delta, and 50+ sources. For the many of you running dbt + DuckDB
locally, flAPI can sit directly on the artifacts dbt produces — dbt builds
the models, flAPI serves them.

```yaml
url-path: /revenue-by-region/
mcp-tool:
  name: revenue_by_region
  description: Monthly revenue aggregated by region
request:
  - field-name: region
    field-in: query
    validators:
      - type: enum
        values: [emea, amer, apac]
template-source: revenue_by_region.sql
connection: [warehouse]
```

What you get in the box, as config rather than code: typed parameter
validation (typed params are bound as DuckDB prepared statements, not
string-interpolated — injection is structurally impossible for those
sites), Basic/JWT auth with per-tool role checks (fail-closed), rate
limiting, and DuckLake-based caching with scheduled full or incremental
refresh and snapshot time-travel — handy for keeping a hot endpoint from
re-scanning the warehouse per request.

Deployment is deliberately boring: one binary, or `flapi pack` to fold
your whole config tree into the binary so `scp` is the deploy. Quickest
try: `uvx --from flapi-io flapi -c flapi.yaml` (PyPI package is
`flapi-io`).

**Where flAPI is *not* the right choice, honestly:**

- If your semantic layer and tests live in dbt and you want tools defined
  *inside* that workflow, MXCP's dbt-native approach may serve you better.
- Read-oriented APIs only — no writes, no business logic.
- DuckDB-centric by design.
- MCP is over Streamable HTTP only (no stdio transport; stdio clients
  need a proxy like mcp-remote).
- License is BSL 1.1 — source-available, not open source; production use
  is permitted, reselling it as a hosted service is not; converts to
  MPL-2.0 per version.
- It's a young project.

Would love to hear how people here are exposing dbt-built models to
downstream apps and LLMs today — that feedback directly shapes what we
build next.
