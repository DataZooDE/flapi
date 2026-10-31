CREATE OR REPLACE TABLE {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS SELECT * FROM read_csv('{{{ conn.path }}}')
