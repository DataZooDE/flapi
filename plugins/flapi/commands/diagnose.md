---
description: Diagnose a failing flAPI request, startup or cache refresh - from the symptom to the root cause and a verified fix
argument-hint: "[error message, status code, or what is wrong]"
---

Diagnose this flAPI problem: $ARGUMENTS

Use the `flapi-operate` skill (troubleshooting playbooks); pull in `flapi-rest`,
`flapi-cache-design`, `flapi-auth` or `flapi-setup` for the fix.

1. Reproduce first: the exact request (curl / MCP call) or startup command, and
   capture status code, body and `X-Request-Id`. A 500 body carries no SQL, only
   `request_id`: `grep <request_id>` the server log for the real error. Raise verbosity
   with `flapii config log-level set debug` (running server) or `--log-level debug`.
2. Collect state in one pass:
   - `curl -s localhost:8080/health` (readiness, failed/stale/unbuilt caches)
   - `flapi -c flapi.yaml --validate-config`
   - `flapii metrics --endpoint <path>` (error counts, latency, cache last refresh)
   - for cache problems `flapii cache audit <path>`
   - for template/SQL problems `flapii templates expand <path> --params '{...}'`
3. Classify by symptom and follow the matching playbook: 404 (route), 400
   (validator, header field sent as a query param), 401/403 (auth: inherited global
   `auth:`, `auth.roles`), 429 (rate limit), 500 (SQL/template/connection), 503
   (`cache_warming` / `cache_not_built` / `cache_build_failed`), startup exit
   (config, env whitelist, extension, DuckLake lock, OIDC placeholder), slow
   responses.
4. State the root cause with the evidence that proves it, apply the smallest fix,
   and re-run the reproduction to show it is gone.
5. If the cause is a flAPI bug rather than configuration, say so and draft an issue
   (symptom, minimal repro, expected vs actual, `flapi --version`).
