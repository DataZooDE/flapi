CREATE TABLE {{cache.schema}}.{{cache.table}} AS
SELECT 
  *
FROM '{{{conn.path}}}'
