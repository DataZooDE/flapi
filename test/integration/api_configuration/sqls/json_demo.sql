-- Reproduction of issue #38. `::JSON` triggers DuckDB's autoload of the
-- json extension, so no explicit LOAD is needed here.
SELECT
    1 AS id,
    '{"a": 1, "b": [10, 20], "c": {"nested": true}}'::JSON AS payload
