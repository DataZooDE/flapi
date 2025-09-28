-- Test DuckLake cache template
-- Demonstrates basic cache operations for integration testing
-- Schema is automatically created by the cache manager

-- This template works for all cache modes (full, append, merge)
-- The cache mode is determined by the presence of cursor and primary-key configuration

-- For full refresh mode: CREATE OR REPLACE TABLE
-- For append mode: INSERT INTO (table must exist)
-- For merge mode: MERGE INTO (table must exist)

-- Create table if it doesn't exist (for append/merge modes)
CREATE TABLE IF NOT EXISTS {{cache.catalog}}.{{cache.schema}}.{{cache.table}} (
    message VARCHAR,
    snapshot_id VARCHAR,
    cached_at TIMESTAMP,
    id INTEGER,
    segment VARCHAR
);

-- Insert test data
INSERT INTO {{cache.catalog}}.{{cache.schema}}.{{cache.table}}
SELECT 
    'Test cached data for mode {{cache.mode}}' as message,
    '{{cache.snapshotId}}' as snapshot_id,
    CURRENT_TIMESTAMP as cached_at,
    ROW_NUMBER() OVER () as id,
    '{{cache.mode}}' as segment;
