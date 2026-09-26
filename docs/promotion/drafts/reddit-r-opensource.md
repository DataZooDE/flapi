<!--
Target: r/opensource — RECOMMENDATION: SKIP THIS SUBREDDIT. See rationale
below. If a "source-available" post is ever wanted, an adapted draft is
included, but the default plan should be to not post there at all.
Timing: n/a (skip). If overruled: only after the BSL question has been
handled well in public elsewhere (post-HN), never as part of the launch
wave.
-->

# Recommendation: skip r/opensource

The playbook (§3) lists r/opensource as low-risk, but that assessment
doesn't survive contact with the license. flAPI is **BSL 1.1 —
source-available, not open source** by the OSI definition, and
r/opensource exists specifically around that definition. A launch post
there would either (a) misrepresent the license and get flamed and
screenshotted, or (b) honestly disclose BSL and be off-topic by the sub's
own standards — the top comment writes itself: "this is not open source,
why is it here?" Either outcome burns goodwill that the other channels
depend on, and BSL projects (HashiCorp, et al.) are a recurring negative
flashpoint in that community. There is no version of this post that wins.

**What to do instead:**

- Let r/selfhosted carry the "self-hostable, source-available, honest
  about it" story — that draft (reddit-r-selfhosted.md) discloses BSL in
  the title and first paragraph and that community tolerates
  source-available when it's not disguised.
- Handle the license question proactively in the Show HN first comment
  and Q&A (already drafted).
- If flAPI's license situation ever changes (e.g., a core component goes
  MPL/Apache early, or a version crosses its MPL-2.0 change date and is
  maintained as such), *that* is a legitimate r/opensource post.

---

# Fallback draft (only if someone insists on posting anyway)

Post it as what it is — a source-available project — and expect a rough
ride. Better targets for this exact text: Lemmy selfhosted/technology
communities (playbook §7), or r/coolgithubprojects.

## Title

flAPI — source-available (BSL 1.1, converts to MPL-2.0) SQL-to-API server: one SQL file becomes a REST endpoint and an MCP tool

## Body

Disclosure: I'm one of the authors. And to be maximally clear up front:
**this is not open source.** flAPI is under the Business Source License
1.1. The source is public, you can modify and self-host it, and production
use is permitted under the Additional Use Grant; what's restricted is
offering flAPI itself to third parties as a hosted service or embedding it
in a commercial product. Each version automatically converts to MPL-2.0
five years after publication. If source-available doesn't meet your bar,
I understand — PostgREST (MIT) and ROAPI (Apache-2.0) are genuinely good
tools in the same space.

What it does: a Mustache-templated SQL file plus a YAML config becomes a
REST endpoint and an MCP tool from the same definition. Single static
C++17 binary with DuckDB embedded — so the SQL can query Parquet/CSV,
Postgres, BigQuery, S3/GCS/Azure, Iceberg, Delta, and 50+ other sources.
Typed request parameters are bound as DuckDB prepared statements (no
string interpolation for those sites); auth with per-tool RBAC, rate
limiting, and DuckLake-based caching are configuration, not code.
`flapi pack` can fold the whole config tree into the binary so a deploy
is one `scp`.

Try: `uvx --from flapi-io flapi -c flapi.yaml`
(PyPI package is `flapi-io`; `flapi` was taken.)

Limitations: read-oriented data APIs only (not a CRUD backend);
DuckDB-centric; MCP over HTTP only (no stdio — use a proxy like
mcp-remote for stdio clients); BSL 1.1 as above; young project.

Repo: https://github.com/DataZooDE/flapi
