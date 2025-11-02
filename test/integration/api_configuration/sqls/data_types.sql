-- Data types endpoint query
-- Returns all columns from data_types.parquet to test various data types

SELECT *
FROM read_parquet('{{{conn.path}}}')
ORDER BY 1

