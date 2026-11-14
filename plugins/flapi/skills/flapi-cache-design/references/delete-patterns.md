# Deletes in a merge cache

flAPI never detects deleted source rows: an incremental refresh only sees the rows
its delta query returns. Deletes are SQL in your populate template. DuckLake's
`MERGE` accepts **one** `UPDATE` or `DELETE` action (plus `INSERT`), so
"delete if flagged, else update" cannot be one `MERGE`.

All patterns below were run against flAPI v26.10.31 (DuckDB 1.5.5, DuckLake) over
two or more refreshes with source changes in between.

## 1. Tombstones (recommended)

Keep `is_deleted` in the cache, update it like any column, filter it when serving.
One `MERGE`, and the tombstone's `updated_at` moves the watermark past the
delete. The full worked example is in
[SKILL.md](../SKILL.md#incremental-merge-with-deletes-as-tombstones).

```sql
-- serving template
SELECT id, name, tier FROM {{cache.catalog}}.{{cache.schema}}.{{cache.table}}
WHERE NOT is_deleted
```

Purge old tombstones only if they matter for size; a purge removes rows the
watermark is computed over, which is harmless here because newer rows exist.

## 2. Remove flagged rows physically

The template runs as one transaction, and `rows_affected` is the sum of every
statement's count, **including the temp-table CTAS** (measured: 3 staged + 1
deleted + 2 merged = 6). Drop the temp table and repeat the delta subquery in
both statements if you want the number to mean "rows changed".

```sql
{{^cache.previousSnapshotTimestamp}}
CREATE OR REPLACE TABLE {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS
SELECT id, customer, amount, updated_at FROM src.orders WHERE NOT is_deleted;
{{/cache.previousSnapshotTimestamp}}
{{#cache.previousSnapshotTimestamp}}
CREATE OR REPLACE TEMP TABLE delta AS
SELECT id, customer, amount, updated_at, is_deleted FROM src.orders
WHERE updated_at > TIMESTAMP '{{{cache.previousSnapshotTimestamp}}}';
DELETE FROM {{cache.catalog}}.{{cache.schema}}.{{cache.table}}
WHERE id IN (SELECT id FROM delta WHERE is_deleted);
MERGE INTO {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS t
USING (SELECT id, customer, amount, updated_at FROM delta WHERE NOT is_deleted) AS s
ON t.id = s.id
WHEN MATCHED THEN UPDATE SET customer = s.customer, amount = s.amount, updated_at = s.updated_at
WHEN NOT MATCHED THEN INSERT (id, customer, amount, updated_at)
  VALUES (s.id, s.customer, s.amount, s.updated_at);
{{/cache.previousSnapshotTimestamp}}
```

Trade-off: the deleted row is gone from the cache, so if the newest change was a
delete the watermark stays below it and that delete is re-read (a no-op) on every
refresh until a newer row arrives. The temp table keeps the source read to one
pass.

## 3. Hard deletes (no flag in the source)

A delta query cannot see a row that is gone. Reconcile on the key, which reads
only the key column of the source:

```sql
DELETE FROM {{cache.catalog}}.{{cache.schema}}.{{cache.table}}
WHERE id NOT IN (SELECT id FROM src.orders);
```

Put it before the `MERGE` (pattern 2). If the key set is too expensive to read on
every refresh, run a full rebuild periodically instead
(`flapii cache refresh <path> --full`, or `flapi cache refresh ... --full` from a job). `WHEN NOT MATCHED BY SOURCE THEN DELETE` is accepted by
DuckLake as the single delete action, but only works when the `USING` side is
the **whole** source, which defeats an incremental refresh.

## What does not work

| Template | Error |
|---|---|
| `WHEN MATCHED AND s.is_deleted THEN DELETE` + `WHEN MATCHED THEN UPDATE ...` | `Not implemented Error: MERGE INTO with DuckLake only supports a single UPDATE/DELETE action currently` |
| `WHEN NOT MATCHED THEN INSERT (*) VALUES (s.*)` | `Parser Error: syntax error at or near "*"` (use `INSERT *`, bare `INSERT`, or a column list) |
| `MERGE` / `INSERT` into a table that does not exist yet | `Catalog Error: Table with name ... does not exist!` |
| `cache.delete-handling: soft` | deprecated, no effect (warning at startup) |

## Detecting lost rows

A strict `>` loses late rows silently, and the refresh still
reports success. Reconcile now and then with an uncached endpoint (cache
variables are not set there, so name the table: `<alias>.<schema>.<table>`):

```yaml
# flapi: endpoint group=order-feed file=order-feed-check.yaml
url-path: /order-feed/check
template-source: order-feed-check.sql
connection: [northwind]
# flapi: check GET /order-feed/check -> 200
```

```sql
-- flapi: template group=order-feed file=order-feed-check.sql
SELECT
  (SELECT count(*) FROM nw.Orders)          AS source_rows,
  (SELECT count(*) FROM cache.cache.order_feed) AS cached_rows,
  (SELECT count(*) FROM nw.Orders
    WHERE OrderID NOT IN (SELECT OrderID FROM cache.cache.order_feed)) AS missing_rows
```

`missing_rows > 0` means the cursor is not monotonic or rows arrive late: widen
the overlap window (it must cover the **maximum** lateness) or switch to merge.
