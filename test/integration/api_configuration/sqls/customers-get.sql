-- Customers GET query with optional filters
-- Maps parquet columns (c_*) to expected output format
-- Note: Some fields like email, registration_date, last_login_time, uuid don't exist in the parquet file
-- These filters will be ignored in the actual query

SELECT 
    c_custkey as key,
    c_name as name,
    {'segment': c_mktsegment} as segment,
    c_acctbal as balance,
    c_comment as comment,
    {
        'street': c_address,
        'address': {
            'nation': c_nationkey,
            'phone': c_phone
        }
    } as contact,
    NULL::DATE as registration_date,
    NULL::TIME as last_login_time,
    NULL::VARCHAR as uuid
FROM read_parquet('{{{conn.path}}}')
WHERE 1=1
    {{#params.id}}
    AND c_custkey = {{{ params.id }}}
    {{/params.id}}
    {{#params.name}}
    AND c_name LIKE '%{{{ params.name }}}%'
    {{/params.name}}
    {{#params.segment}}
    AND c_mktsegment = '{{{ params.segment }}}'
    {{/params.segment}}
    {{#params.email}}
    AND 1=0  -- Email column doesn't exist, filter will return no results
    {{/params.email}}
    {{#params.registration_date}}
    AND 1=0  -- Registration date column doesn't exist
    {{/params.registration_date}}
    {{#params.last_login_time}}
    AND 1=0  -- Last login time column doesn't exist
    {{/params.last_login_time}}
    {{#params.uuid}}
    AND 1=0  -- UUID column doesn't exist
    {{/params.uuid}}
ORDER BY c_custkey

