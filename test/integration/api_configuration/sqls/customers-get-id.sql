-- Get customer by ID
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
WHERE c_custkey = {{{ params.customer_id }}}
LIMIT 1

