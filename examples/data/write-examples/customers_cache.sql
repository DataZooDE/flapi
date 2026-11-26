-- Rebuilds customers_cache after a write (invalidate-on-write / refresh-on-write)
-- and on every scheduled refresh. A write endpoint's cache needs its own
-- template-file: without one the populate SQL would be the endpoint's write.
CREATE OR REPLACE TABLE {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS
SELECT customer_id, name, email, phone, created_at, updated_at FROM customers
