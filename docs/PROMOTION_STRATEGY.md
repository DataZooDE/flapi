# flAPI Promotion Playbook

A concrete, community-by-community plan for getting flAPI in front of the
people most likely to adopt it. Built from a scan of where SQL-to-API,
DuckDB, and MCP conversations actually happen in 2026.

The guiding principle: **flAPI wins on distribution channels that most of
its competitors ignore.** PostgREST, Hasura, and Directus fight over the
"database → API" search term. flAPI is the only tool that lands in *three*
fast-growing ecosystems at once — **DuckDB**, **MCP / AI tools**, and
**single-binary self-hosting** — and each has its own hungry, under-served
community. Promote where the overlap is thin, not where the competition is
thick.

---

## 1. Sharpen the hook first (positioning)

Nobody upvotes "SQL-to-API framework." They upvote a sentence that names a
pain they felt this week. Lead with one of these, matched to the audience:

| Audience | One-liner |
|----------|-----------|
| **AI / MCP crowd** | "Write one SQL file. Get a REST endpoint **and** an MCP tool your LLM can call — with per-tool RBAC and a tool-description injection scanner built in." |
| **Data engineers** | "Your parquet/Postgres/BigQuery/S3 data as a governed REST API in a YAML file. No Flask, no FastAPI, no glue code." |
| **Self-hosters** | "A single static binary that turns SQL into an API. `scp flapi-prod user@host` *is* the whole deploy — config folded into the binary." |
| **Security-minded** | "Typed params are bound as **DuckDB prepared statements** — SQL injection is structurally impossible at those sites, not just 'mitigated'." |
| **DuckDB users** | "You already query 50+ sources with DuckDB. flAPI puts a REST + MCP server in front of it in one binary." |

**The single most valuable angle in 2026 is the MCP one.** "Every REST
endpoint is also an AI tool, from the same config" is a genuinely
differentiated claim — MXCP is the only close competitor, and it's
Python/dbt-centric. Lead with MCP everywhere the audience touches AI.

---

## 2. The "free distribution" tier — do this first, this week

These are directories and lists that *exist to be submitted to*. Zero risk
of a "spam" backlash, permanent SEO/discovery value, and they seed the
credibility you'll cite in later posts. This is the highest ROI work here.

### MCP registries & directories (ride the wave)
flAPI ships an MCP server — get it listed everywhere people browse for MCP
tools:

- **Official MCP Registry** — the canonical source the directories read from.
- **[mcp.so](https://mcp.so)** — submit form.
- **[Smithery](https://smithery.ai)** — `smithery mcp publish` CLI or web dashboard; can even host/run it.
- **[Glama](https://glama.ai/mcp)** — auto-indexes open-source MCP servers from GitHub; claim the listing.
- **[PulseMCP](https://www.pulsemcp.com)** — "Submit" in top nav; 18k+ servers, high discovery traffic.
- **[punkpeye/awesome-mcp-servers](https://github.com/punkpeye/awesome-mcp-servers)** and **[wong2/awesome-mcp-servers](https://github.com/wong2/awesome-mcp-servers)** — open a PR / submission. Put flAPI in the "Databases" *and* "Data Platforms" categories.

  Metadata to include everywhere: install command (`uvx --from flapi-io flapi`), transport, auth model (per-tool RBAC), supported clients, docs link, license, and the tool-list.

### DuckDB ecosystem
- **DuckDB Discord `#show-and-tell` / `#extensions`** — the friendliest room on this list. DuckDB users *love* seeing what people build on top of DuckDB. Post the MCP angle here.
- **[DuckDB Community Extensions](https://duckdb.org/community_extensions/)** — flAPI uses an `embed://` VFS scheme and registers with DuckDB; if any reusable piece can ship as a community extension, that's a listing + a blog mention from the DuckDB team.
- **MotherDuck** actively publishes "MCP + DuckDB" content — pitch a guest post or get cited; their blog and videos are already covering exactly this space.

### Self-hosting & general
- **[awesome-selfhosted](https://github.com/awesome-selfhosted/awesome-selfhosted)** — PR to the "Automation" or "Miscellaneous / API" section. The single-binary story is tailor-made. *Caveat (verified 2026-07-13): flAPI's BSL 1.1 license is not in their accepted-licenses list; a listing would have to go through their non-free (⊘) section. See SUBMISSION_KIT.md §8 — low priority.*
- **[Awesome list for APIs / data-engineering]** — find the relevant `awesome-*` repos (awesome-datascience, awesome-data-engineering) and PR flAPI in.
- **AlternativeTo / LibHunt / SaaSHub** — list flAPI as an alternative to PostgREST, Hasura, Datasette, ROAPI. Captures comparison-shopping traffic forever.

---

## 3. Reddit — value-first, per-community angle

Reddit rules are strict and enforced by humans. **The account matters more
than the post.** Spend 2–3 weeks genuinely answering questions in these subs
before any launch post, or the launch gets nuked as spam. Then post a "Show
/ I built" with an honest limitations section and *stay in the comments*.

| Subreddit | Angle that fits the culture | Notes |
|-----------|----------------------------|-------|
| **r/dataengineering** | "Governed data API without standing up a backend" — engineering teardown, not an ad | Skeptical of hype; lead with architecture, caching, prepared-statement safety |
| **r/DuckDB** | "REST + MCP server on top of DuckDB, single binary" | Small but perfectly targeted; near-zero competition here |
| **r/mcp** and **r/modelcontextprotocol** | "Turn any SQL query into an MCP tool with RBAC + injection scanning" | Fastest-growing relevant audience in 2026 |
| **r/selfhosted** | "Single static binary, config baked in, `scp` to deploy" | Docker image + no-runtime-deps play very well |
| **r/opensource** | Launch post with clear install + honest limitations | Exists to discover projects; low spam-risk |
| **r/webdev** | Only as an engineering teardown/benchmark, never a direct promo | Harsh on ads; be careful |
| **r/dataanalysis / r/BusinessIntelligence** | "Analysts ship APIs with just SQL — no backend dev needed" | The core value prop, in their language |
| **r/rust / r/cpp** | Build-story: single-binary C++ service, self-packaging via appended ZIP / Mach-O segment | r/cpp will appreciate the packaging hack specifically |

**Thread-jacking (the highest-signal Reddit move):** don't wait for a launch
window. Search these subs weekly for live questions like *"how do I turn my
SQL query into an API,"* *"PostgREST alternative,"* *"expose parquet as
REST,"* *"MCP server for my database."* Answer the question *thoroughly and
honestly first*, then mention flAPI as one option with a link. A genuinely
helpful comment on an existing thread converts far better than a launch post
and never reads as spam.

---

## 4. Hacker News — one good shot

HN is where a "Show HN" for a DuckDB/MCP tool can outperform a Product Hunt
launch. But it's one shot per project realistically, so make it count.

- **Title:** `Show HN: flAPI – Turn one SQL file into a REST endpoint and an MCP tool`. Concrete, no adjectives.
- **Timing:** weekday, ~8–10am ET. Have the repo README polished, a 60-second GIF, and the `uvx` one-liner front and center.
- **First comment (yours):** the build story — *why* C++/single-binary, the prepared-statement injection story, the appended-ZIP self-packaging trick (HN loves a clever low-level detail). Precedent: ROAPI's "Show HN" did well on exactly this territory.
- **Be present for 4–6 hours** answering every comment. HN ranking rewards engaged authors.
- **Anticipated pushback + honest answers:** "why not PostgREST?" (not Postgres-only, DuckDB = 50+ sources, MCP built in); "why not just FastAPI?" (no code, governance/caching/RBAC included); "why not Datasette/ROAPI?" (MCP-native, prepared-statement safety, single-binary self-packaging).
- **Also viable:** an "Ask HN" or a standalone blog post ("How we made SQL injection structurally impossible in a SQL-templating tool") that earns its own front-page slot without being a launch.

---

## 5. Stack Overflow & Q&A — the evergreen long game

People search SO for years. You can't post ads, but you *can* answer real
questions well and reference flAPI where it's genuinely the best fit.

- Hunt for questions tagged `duckdb`, `parquet`, `rest`, `fastapi`, `postgrest`, `model-context-protocol` that boil down to "how do I serve this data over HTTP quickly."
- Write a complete, correct answer (even the FastAPI/Quarkus way), then add: "If you'd rather not maintain that glue code, flAPI does exactly this from a YAML file — [link]." Disclose that you're affiliated.
- Do the same on **dev.to** and **Medium**: there are already posts like *"Turning a Parquet file into a queryable REST API with DuckDB + Quarkus + Kotlin."* Write the *shorter* flAPI version as a response/alternative and cross-link.

---

## 6. Niche forums & communities where competitors aren't

This is the creative edge. These audiences have the pain and almost no one
is pitching them a SQL-to-API+MCP tool:

- **dbt Community Forum ("Show and Tell")** — MXCP announced here and got traction. flAPI's "REST + MCP, no Python/dbt required, single binary" is a clean counter-position. dbt users running DuckDB locally are a perfect fit.
- **Locally Optimistic / dbt Slack / Data Twitter(X)** — the analytics-engineering crowd. Frame: "let analysts ship read-only APIs without begging a backend team."
- **MotherDuck / DuckDB office hours & meetups** — lightning-talk or demo slot. This community *is* your ICP.
- **ERPL / SAP-on-DuckDB niche** — flAPI can expose **SAP ERP & BW** via ERPL. Almost nobody offers "SAP data as a governed REST/MCP API from a YAML file." SAP developer communities (SAP Community, r/SAP, SAP-focused Discords/LinkedIn groups) are an untapped, high-value, low-competition audience. This is arguably flAPI's most differentiated single wedge.
- **Microsoft 365 / SharePoint niche** — via the [erpl-web](https://github.com/DataZooDE/erpl-web) DuckDB extension, flAPI serves **SharePoint lists** (plus Entra ID, Teams, Business Central, Dataverse) as REST + MCP tools — working demo recorded (`docs/promotion/assets/flapi-demo-sharepoint.gif`). "Your team's SharePoint list as an AI tool, no Power Platform needed" is untapped: r/Office365, r/sharepoint, Microsoft Tech Community, M365 admin Discords. Same low-competition logic as the SAP wedge.
- **LLM tooling Discords** (Claude/Anthropic dev community, Cursor, LangChain, LlamaIndex) — wherever people wire up MCP servers, "expose your warehouse as governed MCP tools" is on-topic.
- **AI engineering newsletters** (Latent Space, tldr.ai, Ben's Bites, Data Engineering Weekly) — pitch the MCP-governance angle; these curate tools weekly and are hungry for concrete demos.

---

## 7. Content that does the promoting for you (SEO + evergreen)

Ship a few of these on the project blog / docs; they rank and get cited long
after a Reddit post scrolls away:

1. **Comparison pages** (highest-intent search traffic):
   - "flAPI vs PostgREST" · "vs Hasura" · "vs Datasette / ROAPI" · "vs MXCP"
   - Be scrupulously fair — list where the *other* tool wins. Fair comparisons get linked by neutral parties and don't get flamed.
2. **"Every REST endpoint, free MCP tool"** — the flagship narrative post. This is the one to syndicate everywhere.
3. **"SQL injection, structurally impossible"** — a security deep-dive on typed-param → prepared-statement binding. Security posts punch above their weight on HN/lobste.rs.
4. **"A single binary that unpacks its own config"** — the self-packaging (appended ZIP / reserved Mach-O segment) engineering story. Catnip for r/cpp, lobste.rs, HN.
5. **Recipe posts** — "Serve your BigQuery table as an MCP tool in 5 minutes," "SAP BW → REST API without a backend," "Cache a slow Snowflake query with DuckLake." Each targets a long-tail search and doubles as a Reddit/HN demo.
6. **A 60-second demo GIF/video** — reused in every single post above. The `uvx --from flapi-io flapi` → live endpoint → Claude calling it as a tool loop is the money shot.

Also consider **lobste.rs** (higher signal than HN for systems/security
content — the C++ packaging and injection-safety posts fit its taste) and
**Lemmy's technology/selfhosted communities** (Reddit refugees, receptive to
open-source self-hosted tools).

---

## 8. Suggested launch sequence

1. **Week 0 (prep):** polish README + 60s GIF; write the flagship "REST + free MCP tool" post; start genuinely participating in r/dataengineering, r/mcp, DuckDB Discord.
2. **Week 1 (seed quietly):** submit to *all* MCP directories + awesome lists + AlternativeTo. Post the demo in DuckDB Discord `#show-and-tell`. Publish the flagship blog post.
3. **Week 2 (niche):** dbt Community "Show and Tell"; SAP/ERPL communities; LLM-tooling Discords. Begin thread-jacking on Reddit/SO with genuinely helpful answers.
4. **Week 3 (big shot):** Show HN, timed weekday morning ET, with the build-story first comment. Same day: cross-post the DuckDB/MCP angle to r/mcp and r/DuckDB (which are launch-friendly). Pitch the AI newsletters.
5. **Ongoing:** ship one comparison/recipe post per week for SEO; keep answering questions; feed the MCP-governance angle to newsletters as features land.

---

## 9. Guardrails (don't torch the goodwill)

- **Always disclose affiliation.** "I built this" or "disclosure: I'm on the flAPI team." Undisclosed shilling gets you banned and screenshotted.
- **Value before link, every time.** Answer the actual question first; the link is a P.S., not the point.
- **Never astroturf** — no fake accounts, no sockpuppet upvotes. HN and Reddit detect it and the blowback is worse than no launch.
- **Read each community's self-promo rules** before posting; many require a ratio of contributions to promo, or a specific flair/megathread.
- **Lead with the honest limitation** ("read-only APIs; not a write layer; DuckDB-centric"). It builds more trust than any feature list and preempts the top comment.
- **Show up for the comments.** An unanswered launch thread dies; an engaged author thread climbs.

---

*Last updated: 2026-07-13*
