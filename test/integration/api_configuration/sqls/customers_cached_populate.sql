-- Populate template for /customers_cached/. A cache template has to WRITE the
-- cache table: this endpoint used to point template-file at its own serving
-- SELECT, which "refreshed" successfully while building nothing.
CREATE OR REPLACE TABLE {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS
SELECT * FROM read_parquet('{{{conn.path}}}')
