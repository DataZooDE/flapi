-- Simple resource that returns schema information
SELECT
    'id' as column_name,
    'INTEGER' as data_type,
    'NO' as is_nullable,
    NULL as column_default
UNION ALL
SELECT
    'name' as column_name,
    'VARCHAR' as data_type,
    'YES' as is_nullable,
    NULL as column_default
UNION ALL
SELECT
    'segment' as column_name,
    'VARCHAR' as data_type,
    'YES' as is_nullable,
    NULL as column_default;
