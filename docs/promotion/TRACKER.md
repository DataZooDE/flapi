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

- [ ] Fix wheel license metadata (Apache-2.0 → BUSL-1.1) — see companion issue
- [ ] Add `mcp-name: io.github.datazoode/flapi` marker to PyPI README (MCP Registry ownership validation)
- [ ] Record 60-second demo GIF (`uvx` → curl → Claude calls the MCP tool); embed in README
- [ ] README polish: lead with the MCP angle

## 📤 Submissions (Week 1 — "seed quietly")

- [ ] Official MCP Registry: `mcp-publisher login github && mcp-publisher publish` (after blockers 1–2)
- [x] PR to punkpeye/awesome-mcp-servers — [punkpeye#10023](https://github.com/punkpeye/awesome-mcp-servers/pull/10023), submitted 2026-07-13
- [x] wong2 list (mcpservers.org): submitted via web form 2026-07-13 (free tier, category Database, contact jr@data-zoo.de) — "reviewed within 12 hours", approval lands by email
- [ ] mcp.so submit form · Smithery · claim Glama listing · PulseMCP submit
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
