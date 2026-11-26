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
| `{{cache.snapshotId}}` / `{{cache.snapshotTimestamp}}` | the newest snapshot that wrote this table **before** this refresh, i.e. the last refresh that changed it (id, commit time, UTC). Read before this refresh writes anything; it is not this refresh's snapshot (that id is in the audit row afterwards). Not a watermark. Absent for a table no snapshot has touched yet. Kept with `--full`. |
| `{{cache.previousSnapshotId}}` | the same snapshot id. Kept when the cursor watermark is dropped (an empty table, a value that does not match `cursor.type`); erased with `--full`. |
| `{{cache.previousSnapshotTimestamp}}` | **the watermark.** With a `cursor:` it is `max(<cursor column>)` over the cached rows, as text in the cursor's type. Without a cursor (full mode) it is the commit time of that same snapshot, i.e. `{{cache.snapshotTimestamp}}`. Erased with `--full`. |
| `{{cache.cursorColumn}}` / `{{cache.cursorType}}` | from `cache.cursor` |
| `{{cache.primaryKeys}}` | `cache.primary-key`, comma-joined |
| `{{params.cacheMode}}` | `full`, `append` or `merge` |
| `{{conn.*}}`, `{{env.*}}` | first connection's properties, whitelisted environment variables (read at render time) |

A 0-row incremental refresh writes no snapshot for the table, so the next refresh
still sees the last refresh that changed it.

`{{cache.previousSnapshotTimestamp}}` is **absent** (so `{{^...}}` renders) with
`--full`, for a table no snapshot has touched, and when a cursor is configured and:

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
- `{{ x }}` doubles single quotes, `{{{ x }}}` pastes the value as it is. Both
  work inside SQL string literals: `TIMESTAMP '{{{cache.previousSnapshotTimestamp}}}'`.
- The rendered text runs as one multi-statement script in one transaction
  (a failure changes nothing); separate statements with `;`. `rows_affected` is
  the sum of the DML statements' counts, NULL if there is none.
- `flapii templates expand <path> --cache` shows the rendered populate script and
  the variables without running it.

## Serving template (`template-source`)

Only `{{cache.catalog}}`, `{{cache.schema}}` and `{{cache.table}}` are set, plus
the usual `params.*`, `conn.*`, `env.*`, `auth.*`. Request values cannot override
them.

## Guards and proving a rollback

The populate script runs as one transaction, so a check that raises an error rolls
back everything before it, and the previous snapshot keeps serving. **Every**
statement must write something: a bare `SELECT` anywhere in the script is refused
before anything runs (`statement N of M is a bare SELECT; its result is
discarded`), so `SELECT error('...')` never fires. Write the check into a temp table:

```sql
CREATE OR REPLACE TABLE {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS
SELECT id, name FROM read_csv('{{{conn.path}}}');
CREATE OR REPLACE TEMP TABLE _chk AS
SELECT CASE WHEN (SELECT count(*) FROM {{cache.catalog}}.{{cache.schema}}.{{cache.table}}) > 100000
            THEN error('refusing: more than 100000 rows') END AS ok;
```

To prove the rollback, read a fingerprint of the cache table from an uncached
endpoint before and after a refresh that trips the check:

```sql
-- sqls/items-fingerprint.sql (endpoint without cache:, so name the table)
SELECT count(*) AS n, count(DISTINCT id) AS keys, bit_xor(hash(id, name)) AS checksum
FROM cache.cache.items_cache
```

Verified: the refresh answers `400 Cache refresh failed` with `reason` `Invalid Input
Error: refusing: ...`, the audit row is `error`, and the fingerprint is unchanged.

## Developing and checking a populate template

- `flapii cache get <path>` (table, schema, cursor, key, template file), `flapii
  cache template <path>` (the populate SQL the server reads), `flapii cache
  update-template <path> --file f.sql`. The last two need `template-file` (else
  `400 Cache template not configured`).
- `flapii templates expand <path> --cache` returns `target`, `mode`, `expanded`,
  `variables`; without `--cache` it renders the serving template. An unclosed
  `{{#section}}` is a 400 naming `<file>:<line>:<col>`.
- `POST /api/v1/_config/endpoints/{slug}/template/expand?target=cache&validate_only=1`
  parses each statement, applies the bare-SELECT rule and prepares each statement in
  a rolled-back transaction (binder errors such as unknown columns). A `CREATE TABLE
  IF NOT EXISTS ... AS ... WHERE false` bootstrap is created there `WITH NO DATA`, so
  a `MERGE` after it is bound even before the first build.
- The audit log is data: `flapii` prints it as the server sends it, snake_case
  (`duration_ms`, `rows_affected`, `snapshot_id`, `message`), whatever `--json-style`
  says. A successful `flapii cache refresh` prints mode, rows, duration and snapshot.
- A failed `flapii cache refresh` prints `[400] Bad Request: Cache refresh failed`
  with a `reason` (the DuckDB error); the audit row's `message` has it too.
  `--log-level debug` logs every rendered statement (`Processed query:`).
- Prove the incremental branch: refresh, change the source, refresh again, and
  check that the second audit row is the mode you expect with a small `rows_affected`.
