CREATE TABLE {{cache.schema}}.{{cache.table}} AS
SELECT * 
FROM bigquery_scan('d-kaercher-lakehouse.application__sales.krass__recommendations_by_device')
