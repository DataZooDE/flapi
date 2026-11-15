-- DuckLake cache populate template for the customers endpoints (full refresh).
--
-- The cache mode is inferred from the endpoint's cache block: no cursor = full
-- refresh (this file), cursor only = append, cursor + primary-key = merge.
-- See docs/guides/caching.md for append and merge templates.
--
-- Variables available here:
--   {{cache.catalog}} {{cache.schema}} {{cache.table}}   where to write
--   {{cache.snapshotId}} {{cache.snapshotTimestamp}}     newest snapshot before this refresh
--   {{cache.previousSnapshotId}} {{cache.previousSnapshotTimestamp}}  incremental watermark
--   {{cache.cursorColumn}} {{cache.cursorType}} {{cache.primaryKeys}} {{cache.schedule}}
--   {{params.cacheMode}}                                  full | append | merge
--   {{conn.*}}                                            connection properties
-- Request parameters are NOT available: a cache is built once, not per request.
CREATE OR REPLACE TABLE {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS
SELECT
    c_custkey,
    c_name,
    c_address,
    c_nationkey,
    c_phone,
    c_acctbal,
    c_mktsegment,
    c_comment
FROM read_parquet('{{{conn.path}}}');
