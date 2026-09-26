<!--
Target: AI-engineering newsletters — Latent Space, Ben's Bites, TLDR AI,
Data Engineering Weekly (adapt the first line per outlet).
Timing: Week 3, same day as the Show HN (playbook §8 — "pitch the AI
newsletters"). Include the HN thread link if it has traction.
Prerequisites: 60-second demo GIF/video ready to link (§7 — the uvx →
live endpoint → Claude-calling-the-tool loop); flagship blog post
published to link as the deeper read.
-->

# Subject line

One YAML file = a REST endpoint + a governed MCP tool (SQL in, RBAC included)

# Email body (~140 words)

Hi [name],

I'm one of the authors of flAPI — pitching it for [newsletter]'s tools
roundup because it hits the MCP-governance gap your readers keep asking
about: everyone can spin up an MCP server, almost nobody ships one with
access control.

flAPI turns a SQL file + YAML config into a REST endpoint **and** an MCP
tool from the same definition. Single static binary (C++, DuckDB
embedded), try it with `uvx --from flapi-io flapi`.

Three concrete facts:

- **Per-tool RBAC, fail-closed** — roles from JWT/OIDC or Basic auth; a
  tool with no allowed-roles under auth is denied by default.
- **Typed params are bound as DuckDB prepared statements** — injection is
  structurally impossible for those sites, not merely sanitized.
- **One engine, 50+ sources** — Parquet, Postgres, BigQuery, S3, Iceberg.

Honest caveats: read-only APIs, MCP over HTTP (no stdio), BSL 1.1
source-available license.

Repo: https://github.com/DataZooDE/flapi — 60-sec demo: [GIF link].
Happy to do a deeper write-up if useful.

[signature]
