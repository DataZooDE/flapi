# Demo GIFs

All demos are **real recordings** against live servers — no staged output.
Each `.tape` is a [VHS](https://github.com/charmbracelet/vhs) script; the
comment header in each tape says where to run it from and what it needs.

| GIF | Story | Length | Recorded against |
|---|---|---|---|
| `flapi-demo.gif` | One SQL file → REST endpoint + MCP tool (curl both) | 36 s | released v26.07.13 (`uvx`) |
| `flapi-demo-agent.gif` | Claude Code as an agent on flAPI MCP tools (Basic auth, per-tool RBAC) | 38 s | trunk build with fix [#100](https://github.com/DataZooDE/flapi/issues/100) |
| `flapi-demo-bigquery.gif` | An AI agent on top of BigQuery: one YAML tool, typed params, "the model never writes SQL" | 35 s | trunk build with fix #100; needs Google ADC |
| `flapi-demo-pack.gif` | `flapi pack`: fold config + SQL + data into one binary, deploy = scp | 29 s | released v26.07.13 (`uvx`) |
| `flapi-demo-sharepoint.gif` | A SharePoint list as REST + MCP tool via the [erpl-web](https://github.com/DataZooDE/erpl-web) DuckDB extension (`ATTACH ... TYPE sharepoint_lists`), agent analyzes the live list | 34 s | trunk build with fix #100; needs Azure app credentials (Graph) |

Demo project trees used by the recordings are in `demo-projects/`
(`pack-myapi/` for the pack demo — CSV data, see #101 for why not parquet;
`bigquery/` for the BigQuery demo — set your own `project_id`, the GIF never
shows it).

**Recording notes**

- The two agent demos require a build that includes the `#100` fix
  (`"logging": null` in the MCP initialize response makes Claude Code reject
  the handshake on ≤ v26.07.13). Re-record them only on a fixed build, and
  re-record `flapi-demo.gif` / `flapi-demo-pack.gif` with plain `uvx` once a
  release containing the fixes ships.
- Port 8080 was occupied on the recording machine, hence 8084/8085/8086 in
  the tapes.
- The SharePoint demo runs on the **erpl_web** community extension
  (`INSTALL erpl_web FROM community` — works on flAPI's embedded DuckDB
  1.5.3 despite the erpl-web README saying 1.5.4+). Credentials come from
  erpl-web's `.env.microsoft` (`ERPL_MS_*` vars, Azure app with Graph
  client-credentials); the committed `demo-projects/sharepoint/flapi.yaml`
  only references `{{env.*}}` — no secrets. The "project tracker" rows
  (items 54–59 in TestList on clouddatazoo.sharepoint.com) were seeded for
  the demo via `graph_sharepoint_create_item`; the query filters
  `Number >= 10` so the pre-existing Foo/Bar/Baz CI fixture rows stay
  untouched and invisible.
- Scenario considered and NOT recorded, honestly: **SAP/ERPL** — the
  example targets a localhost ABAP trial system that wasn't running; the
  ERPL demo needs a live SAP box. Record `examples/sqls/sap/` flows when
  one is up.
