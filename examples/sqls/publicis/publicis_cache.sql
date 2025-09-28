-- DuckLake cache template for Publicis data
-- This template creates/refreshes the cache table in DuckLake
-- Schema is automatically created by the cache manager

-- FULL REFRESH MODE: Create table as select (CTAS)
-- Since this endpoint is configured without primary-key or cursor, it's always full refresh
CREATE OR REPLACE TABLE {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS
SELECT 
    p.country,
    p.product_category,
    p.campaign_type,
    p.channel,
    sum(p.clicks) as clicks
FROM bigquery_scan('d-kaercher-lakehouse.landing__publicis.kaercher_union_all') AS p
GROUP BY 1, 2, 3, 4;
