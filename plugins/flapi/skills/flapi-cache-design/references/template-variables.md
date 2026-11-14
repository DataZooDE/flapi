# Cache template variables

Set by `CacheManager::refreshDuckLakeCache` (`src/cache_manager.cpp`) and exposed
by `SQLTemplateProcessor::createTemplateContext` (`src/sql_template_processor.cpp`).

## Populate template (`cache.template-file`)

| Variable | Value |
|---|---|
| `{{cache.catalog}}` | `ducklake.alias` (default `cache`) |
| `{{cache.schema}}` | `cache.schema` (default `cache`) |
| `{{cache.table}}` | `cache.table` |
| `{{cache.schedule}}` | `cache.schedule`, if set |
| `{{cache.snapshotId}}` / `{{cache.snapshotTimestamp}}` | newest snapshot that touched this table before this refresh (id, commit time). Not a watermark. |
| `{{cache.previousSnapshotId}}` | same snapshot id; kept (when a snapshot exists) even when the cursor watermark is dropped. |
| `{{cache.previousSnapshotTimestamp}}` | **the watermark.** With a `cursor:` it is `max(<cursor column>)` over the cached rows, as text in the cursor's type. Without a cursor it is the snapshot commit time. |
| `{{cache.cursorColumn}}` / `{{cache.cursorType}}` | from `cache.cursor` |
| `{{cache.primaryKeys}}` | `cache.primary-key`, comma-joined |
| `{{params.cacheMode}}` | `full`, `append` or `merge` |
| `{{conn.*}}`, `{{env.*}}` | first connection's properties, whitelisted environment variables (read at render time) |

`{{cache.previousSnapshotTimestamp}}` is **absent** (so `{{^...}}` renders) when a
cursor is configured and:

- the table does not exist yet, or is empty, or
- `max(cursor)` does not look like `cursor.type` (for `timestamp`/`date`/`time`
  only digits and `:-+. TZ`; for numeric types only digits and `+-.eE`; for
  anything else no quotes, `;`, `\`, newlines, `--` or `/*`). A warning is logged
  and that refresh takes the full branch.

`cursor.type` accepts `timestamp`, `timestamptz`, `datetime`, `date`, `time`,
`int`, `integer`, `bigint`, `smallint`, `hugeint`, `long`, `double`, `decimal`,
`numeric`, `float`, or any other name (treated as a string).

Names seen in older examples (`cache.mode`, a "current" snapshot timestamp) are
not set: an unknown variable renders as an empty string. Request parameters are not
available: a cache is built once, not per request.

## Rendering rules

- Mustache, not Handlebars: `{{#x}}...{{/x}}` (present), `{{^x}}...{{/x}}`
  (absent). `{{#if x}}` is not a conditional.
- `{{ x }}` HTML-escapes, `{{{ x }}}` does not. Use triple braces inside SQL
  string literals: `TIMESTAMP '{{{cache.previousSnapshotTimestamp}}}'`.
- The rendered text runs as one multi-statement script in one transaction
  (a failure changes nothing); separate statements with `;`. `rows_affected` is
  the sum of the DML statements' counts, NULL if there is none.
- `flapii templates expand <path> --cache` shows the rendered populate script and
  the variables without running it.

## Serving template (`template-source`)

Only `{{cache.catalog}}`, `{{cache.schema}}` and `{{cache.table}}` are set, plus
the usual `params.*`, `conn.*`, `env.*`, `auth.*`. Request values cannot override
them.
