---
description: Summarize a running flAPI server's runtime metrics - slow or failing endpoints and MCP tools, rejections, slow or stale caches - with next steps
argument-hint: "[endpoint path or MCP tool name]"
---

Report on the running flAPI server's health and performance. Focus: $ARGUMENTS

Use the `flapi-operate` skill; `flapi-cache-tuning` for slow caches.

1. Fetch the data (server needs `--config-service`; export
   `FLAPI_CONFIG_SERVICE_TOKEN`): `flapii metrics --output json` (add
   `--endpoint <path>` when a focus is given), `curl -s $FLAPI_URL/health`, and
   `flapii cache audit` for caches that look slow or failing. The flapi MCP server's
   `flapi_get_metrics` tool returns the same data.
2. Summarize in a short table:
   - endpoints / tools by p95 latency (state that percentiles are estimates
     interpolated inside a histogram bucket, capped at the max) and error rate (5xx
     for REST, failures for MCP). `rejected` (401/403 on a route, MCP role denials)
     is refused callers, not errors. Report cache 503s (`status.503_cache`, `requests.cache_unavailable`)
     separately: they are a cache not ready yet, not a server error, and are not
     in `5xx` / `server_errors`
   - 401 / 403 / 429 rejection counts and what they suggest (bad clients, attacks,
     limits too tight)
   - caches: state, last refresh duration vs schedule, rows per refresh, data age;
     flag any cache whose refresh takes a large share of its schedule or whose data
     is older than two schedules, and any `failed` cache (its endpoint answers 503
     `cache_build_failed`; the error is in `flapii cache audit <path>`). Metrics and
     audit output use snake_case keys (`duration_ms`, `rows_affected`)
3. Note the observation window (`since_unix_ms`; counters reset on restart).
4. Recommend at most three concrete actions, most valuable first, each with the
   command or config change.
