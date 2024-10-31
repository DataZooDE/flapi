CREATE TABLE {{cache.schema}}.{{cache.table}} AS
SELECT * 
FROM bigquery_query(
    'd-kaercher-lakehouse',
    'SELECT * FROM `d-kaercher-lakehouse.application__sales.krass__recommendations_by_device`'
)
