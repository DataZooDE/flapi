---
description: Add DuckLake caching to an endpoint - pick full / append / merge, write the populate SQL, refresh, measure and tune
argument-hint: "[endpoint path] [freshness requirement]"
---

Add or improve caching for a flAPI endpoint. Request: $ARGUMENTS

Use the `flapi-cache-design` skill to choose and write the cache, then
`flapi-cache-tuning` to measure and speed up the refresh.

1. Read the endpoint YAML and its SQL template, and inspect the source: row count,
   whether rows are only appended or also updated/deleted, which column reliably
   increases on every change (the cursor candidate), and the primary key.
2. Decide the mode with the user, stating the trade-off in two lines:
   - full refresh: no `cursor` (small or fully-recomputed data)
   - append: `cursor` only (immutable events; handle late-arriving rows)
   - merge: `cursor` + `primary-key` (mutable rows; deletes handled in the SQL)
   and the freshness: `schedule`, warm-up mode, and whether writes should
   `invalidate-on-write` / `refresh-on-write`.
3. Make sure `ducklake:` is enabled in `flapi.yaml`. Write the cache block and the
   populate template (`template-file`), guarding incremental predicates with
   `{{#cache.previousSnapshotTimestamp}}` so the first run loads everything.
   Point the serving template at `{{cache.catalog}}.{{cache.schema}}.{{cache.table}}`.
4. Validate (`flapi --validate-config`), then refresh and measure:
   `flapii cache refresh <path>` and `flapii cache audit <path>` (duration_ms,
   rows_affected), or `flapi cache refresh -c flapi.yaml --endpoint <path>` with no
   server running. Refresh twice to prove the incremental path works.
5. If the refresh is slower than the schedule allows, apply `flapi-cache-tuning`
   and show before/after `duration_ms`.
6. Report: mode, schedule, the measured refresh time, rows per refresh, and how
   to watch it (`flapii metrics`).
