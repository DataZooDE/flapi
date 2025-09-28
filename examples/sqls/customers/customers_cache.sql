-- ====================================================================================
-- DUCKLAKE CACHE TEMPLATE FOR CUSTOMERS
-- ====================================================================================
-- This template demonstrates how to use DuckLake-specific variables in cache SQL
-- The cache mode is automatically determined based on primary-key and cursor configuration
-- Schema is automatically created by the cache manager

-- Available DuckLake template variables:
-- {{cache.catalog}}              - DuckLake catalog alias (e.g., "cache")
-- {{cache.schema}}               - Cache schema (e.g., "analytics")  
-- {{cache.table}}                - Cache table name (e.g., "customers_cache")
-- {{cache.mode}}                 - Cache mode: "full", "append", or "merge"
-- {{cache.snapshotId}}           - Current snapshot ID
-- {{cache.snapshotTimestamp}}    - Current snapshot timestamp
-- {{cache.previousSnapshotId}}   - Previous snapshot ID (for incremental)
-- {{cache.previousSnapshotTimestamp}} - Previous snapshot timestamp
-- {{cache.cursorColumn}}         - Cursor column name (if configured)
-- {{cache.cursorType}}           - Cursor column type (if configured)
-- {{cache.primaryKeys}}          - Comma-separated primary key columns

-- MERGE MODE: Create table as select (CTAS) for initial load
-- Since this endpoint has both primary-key and cursor, it uses merge mode
CREATE OR REPLACE TABLE {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS
SELECT 
    id,
    name,
    email,
    segment,
    registration_date,
    CURRENT_TIMESTAMP as cache_updated_at,
    '{{cache.snapshotId}}' as cache_snapshot_id
FROM read_parquet('{{conn.path}}')
WHERE 1=1
{{#if request.id}}
    AND id = {{request.id}}
{{/if}}
{{#if request.segment}}
    AND segment = '{{request.segment}}'
{{/if}}
{{#if request.email}}
    AND email = '{{request.email}}'
{{/if}}
ORDER BY registration_date DESC;