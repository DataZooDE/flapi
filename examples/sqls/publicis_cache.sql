CREATE TABLE {{cache.schema}}.{{cache.table}} AS
SELECT 
    p.country,
    p.product_category,
    p.campaign_type,
    p.channel,
    sum(p.clicks) as clicks
FROM bigquery_scan('d-kaercher-lakehouse.landing__publicis.kaercher_union_all') AS p
GROUP BY 1, 2, 3, 4
