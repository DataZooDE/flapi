-- Customers validation endpoint query
-- Same as customers-get but for strict validation testing

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
ORDER BY c_custkey

