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
DELETE FROM {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS c
WHERE NOT EXISTS (SELECT 1 FROM src.orders AS s WHERE s.id = c.id);
```

Use `NOT EXISTS`, not `WHERE id NOT IN (SELECT id FROM src.orders)`: a single NULL
`id` in the source makes `NOT IN` true for no row, so nothing is ever deleted and
the refresh still succeeds. (`NOT IN` is only safe when the key is `NOT NULL` at
the source.)

Put it before the `MERGE` (pattern 2). If the key set is too expensive to read on
every refresh, run a full rebuild periodically instead
(`flapii cache refresh <path> --full`, or `flapi cache refresh ... --full` from a job). `WHEN NOT MATCHED BY SOURCE THEN DELETE` is accepted by
DuckLake as the single delete action, but only works when the `USING` side is
the **whole** source, which defeats an incremental refresh.

### Tombstones and reconciliation together (patterns 1 + 3)

A source that soft-deletes most rows but sometimes hard-deletes (an archive job,
a GDPR purge) needs both: the `MERGE` carries the tombstones, the reconcile
removes keys that vanished. Two statements, one transaction:

```sql
{{^cache.previousSnapshotTimestamp}}
CREATE OR REPLACE TABLE {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS
SELECT id, customer, amount, updated_at, is_deleted FROM src.orders;
{{/cache.previousSnapshotTimestamp}}
{{#cache.previousSnapshotTimestamp}}
DELETE FROM {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS c
WHERE NOT EXISTS (SELECT 1 FROM src.orders AS s WHERE s.id = c.id);
MERGE INTO {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS t
USING (
  SELECT id, customer, amount, updated_at, is_deleted FROM src.orders
  WHERE updated_at >= TIMESTAMP '{{{cache.previousSnapshotTimestamp}}}'
) AS s
ON t.id = s.id
WHEN MATCHED THEN UPDATE SET customer = s.customer, amount = s.amount,
  updated_at = s.updated_at, is_deleted = s.is_deleted
WHEN NOT MATCHED THEN INSERT (id, customer, amount, updated_at, is_deleted)
  VALUES (s.id, s.customer, s.amount, s.updated_at, s.is_deleted);
{{/cache.previousSnapshotTimestamp}}
```

The serving template filters `WHERE NOT is_deleted` as in pattern 1. If the newest
row is the one a reconcile removed, the watermark drops below it and that window
is re-read once (a no-op under merge).

## What does not work

| Template | Error |
|---|---|
| `WHEN MATCHED AND s.is_deleted THEN DELETE` + `WHEN MATCHED THEN UPDATE ...` | `Not implemented Error: MERGE INTO with DuckLake only supports a single UPDATE/DELETE action currently` |
| `WHEN NOT MATCHED THEN INSERT (*) VALUES (s.*)` | `Parser Error: syntax error at or near "*"` (use `INSERT *`, bare `INSERT`, or a column list) |
| `MERGE` / `INSERT` into a table that does not exist yet | `Catalog Error: Table with name ... does not exist!` |
| `cache.delete-handling: soft` | deprecated, no effect (warning at startup) |

## Late rows in append mode

A strict `>` skips a row that arrives later with a cursor value at or below the watermark, and the refresh still reports success. Either accept
that, or re-read an overlap window and drop what is already cached (append has no
key, so the anti-join is yours). Use `>=` in **both** predicates: with `>` a row
exactly at the window edge (lateness = window) is lost forever; the anti-join
keeps `>=` idempotent.

```sql
INSERT INTO {{cache.catalog}}.{{cache.schema}}.{{cache.table}}
SELECT id, ts, kind FROM src_events.events AS s
WHERE s.ts >= TIMESTAMP '{{{cache.previousSnapshotTimestamp}}}' - INTERVAL 10 MINUTE
  AND NOT EXISTS (
    SELECT 1 FROM {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS c
    WHERE c.id = s.id
      AND c.ts >= TIMESTAMP '{{{cache.previousSnapshotTimestamp}}}' - INTERVAL 10 MINUTE);
```

Without the anti-join an overlap window duplicates the whole window on **every**
refresh. Use `NOT EXISTS`, not `NOT IN`: one NULL id in the subquery makes `NOT IN`
match nothing, so every late row is silently dropped. Size the window to the
measured maximum lateness plus a margin (`flapi-cache-tuning`); merge mode makes
overlap idempotent without the anti-join. Verified: a row exactly at the edge is
loaded with `>=`, and an unchanged source re-reads 0 rows.

## Detecting lost rows

A strict `>` loses late rows silently, a missing anti-join duplicates them, and
the refresh reports success either way. Reconcile now and then with an uncached
endpoint (cache variables are not set there, so name the table:
`<alias>.<schema>.<table>`):

```yaml
# flapi: endpoint group=order-feed file=order-feed-check.yaml
url-path: /order-feed/check
template-source: order-feed-check.sql
connection: [northwind]
# flapi: check GET /order-feed/check -> 200
```

```sql
-- flapi: template group=order-feed file=order-feed-check.sql
WITH src AS (SELECT OrderID, CustomerID, Freight FROM nw.Orders),
     c   AS (SELECT OrderID, CustomerID, Freight FROM cache.cache.order_feed)
SELECT
  (SELECT count(*) FROM src)                                   AS source_rows,
  (SELECT count(*) FROM c)                                     AS cached_rows,
  (SELECT count(*) FROM c) - (SELECT count(DISTINCT OrderID) FROM c) AS dup_rows,
  (SELECT count(*) FROM src
    WHERE NOT EXISTS (SELECT 1 FROM c WHERE c.OrderID = src.OrderID)) AS missing_rows,
  (SELECT bit_xor(hash(OrderID, CustomerID, Freight)) FROM src) AS source_checksum,
  (SELECT bit_xor(hash(OrderID, CustomerID, Freight)) FROM c)   AS cached_checksum
```

- `missing_rows > 0`: the cursor is not monotonic or rows arrive late. Widen the
  overlap window (it must cover the **maximum** lateness plus a margin, with `>=`)
  or switch to merge.
- `dup_rows > 0` in append mode: the overlap window has no anti-join (or it uses
  `NOT IN` over a nullable key). In merge mode it means the `ON` clause is not the key.
- Equal counts but different checksums: rows changed at the source without
  moving the cursor (an append cache over mutable rows, or an `updated_at` the
  source does not maintain). Hash the columns you serve. `bit_xor` cancels a row
  that appears twice, so read it together with `dup_rows`; `sum(hash(...))` (a
  `HUGEINT`) does not cancel but is order-independent too.
- Run it while no refresh is writing, or accept one refresh of skew.
- Source in a DuckDB file that only the populate template attaches? The check
  cannot read that alias before the first refresh or after a restart without
  warm-up. An `init` attach of its own keeps reading the file that was there at startup:
  if the writer publishes by rename it compares against a stale copy until restart, and a
  `missing_rows`/`extra_rows` it reports is then NOT a reason to widen the window. Prefer
  export drops, or compute the source side outside flapi (`flapi-setup`
  [references/connections.md](../../flapi-setup/references/connections.md#duckdb-file-as-a-cache-source)).
