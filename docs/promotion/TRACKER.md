# Promotion execution tracker

Execution tracker for [PROMOTION_STRATEGY.md](../PROMOTION_STRATEGY.md). All
copy-paste material lives in [SUBMISSION_KIT.md](SUBMISSION_KIT.md) and
[drafts/](drafts/). License bug: [#97](https://github.com/DataZooDE/flapi/issues/97).

## ✅ Prepared (in repo)

- [x] Fact-check of all playbook claims against source (2 corrections: injection claim scoped to typed params; BSL ≠ open source)
- [x] `server.json` for the official MCP Registry (repo root)
- [x] `docs/promotion/SUBMISSION_KIT.md` — canonical metadata + per-channel steps
- [x] Launch copy drafts: Show HN (+first comment +pushback Q&A), r/mcp, r/DuckDB, r/selfhosted, r/dataengineering, r/opensource (recommends skip), DuckDB Discord, dbt forum, newsletter pitch
- [x] Blog drafts: flagship "REST + free MCP tool", security deep-dive on typed-param prepared statements

## 🔴 Blockers before launch (Week 0)

- [x] Fix wheel license metadata (Apache-2.0 → BUSL-1.1) — [PR #99](https://github.com/DataZooDE/flapi/pull/99), closes #97. Live: PyPI shows `BUSL-1.1` (verified 2026-09-26)
- [x] Add `mcp-name: io.github.datazoode/flapi` marker to PyPI README — included in PR #99. Live on PyPI (verified 2026-09-26)
- [x] Record demo GIF — `docs/promotion/assets/flapi-demo.gif` (36 s, 825 KB, VHS; embedded at the top of Readme.md). Re-record anytime: `vhs docs/promotion/assets/demo.tape`
- [x] Four more scenario GIFs (all real recordings; see `assets/README.md`): **agent-over-MCP** (`flapi-demo-agent.gif` — Claude Code answering via `customer_lookup`), **agent on BigQuery** (`flapi-demo-bigquery.gif` — public dataset, typed params, "the model never writes SQL"), **self-packaging deploy** (`flapi-demo-pack.gif`), **SharePoint lists** (`flapi-demo-sharepoint.gif` — live Microsoft 365 list via the erpl-web extension's `ATTACH ... TYPE sharepoint_lists`, agent analyzes the tracker). SAP remains blocked (no live ABAP system) rather than faked.
- [x] Filed + fixed two launch-blocking bugs found while recording: [#100](https://github.com/DataZooDE/flapi/issues/100) MCP `logging: null` breaks the Claude Code handshake ([PR #102](https://github.com/DataZooDE/flapi/pull/102)); [#101](https://github.com/DataZooDE/flapi/issues/101) `read_parquet` fails on `embed://` bundles ([PR #103](https://github.com/DataZooDE/flapi/pull/103)). Both merged and have shipped in every release since v26.08.07.
- [ ] README polish: lead with the MCP angle — PR #152 (REST + MCP as equals)

## 📤 Submissions (Week 1 — "seed quietly")

- [ ] Official MCP Registry: publish the release-stamped `server.json` (see SUBMISSION_KIT §1 — not the repo copy, which is a `0.0.0-dev` template). Prerequisites met; for v26.09.23, stamp locally as §1 describes
- [ ] ⛔ awesome-mcp-servers — **skipped (decision 2026-09-26)**. [punkpeye#10023](https://github.com/punkpeye/awesome-mcp-servers/pull/10023) was closed unmerged 2026-09-07; relisting needs a Glama listing + score badge, not pursued for now (SUBMISSION_KIT §2)
- [x] wong2 list (mcpservers.org): submitted via web form 2026-07-13 (free tier, category Database, contact jr@data-zoo.de) — "reviewed within 12 hours", approval lands by email. **Listing not confirmed** as of 2026-09-26 (not in the wong2 README; the site blocks automated checks)
- [ ] ⛔ Glama — **skipped (decision 2026-09-26)**, along with awesome-mcp-servers (SUBMISSION_KIT §6)
- [ ] mcp.so submit form · Smithery · PulseMCP submit
- [ ] AlternativeTo / LibHunt / SaaSHub (alternative to PostgREST, Hasura, Datasette, ROAPI, MXCP)
- [ ] Publish flagship blog post; post DuckDB Discord #show-and-tell (draft ready)
- [ ] ⛔ awesome-selfhosted: license-blocked (BSL not accepted; non-free ⊘ path possible, low ROI)

## 🧑 Human-only groundwork (start now, 2–3 week lead time)

- [ ] Warm Reddit account: genuine answers in r/dataengineering, r/mcp, r/DuckDB, r/selfhosted
- [ ] Join DuckDB Discord, dbt Slack/forum, LLM-tooling Discords; participate before posting

## 📅 Week 2–3

- [ ] dbt Community Forum Show-and-Tell (draft ready) · SAP/ERPL communities · LLM Discords
- [ ] Reddit thread-jacking with the saved search queries (SUBMISSION_KIT.md §12)
- [ ] Show HN (draft ready; weekday 8–10am ET; stay in thread 4–6h) + same-day r/mcp + r/DuckDB posts
- [ ] Pitch newsletters (draft ready): Latent Space, Ben's Bites, TLDR AI, Data Engineering Weekly

## 🔁 Ongoing

- [ ] One comparison/recipe post per week (vs PostgREST / Hasura / Datasette+ROAPI / MXCP; BigQuery→MCP recipe; SAP BW→REST; self-packaging engineering story)
- [ ] SO/dev.to answers with disclosure
- [ ] MotherDuck guest-post outreach; investigate `embed://` as a DuckDB community extension
