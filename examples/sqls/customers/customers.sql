
SELECT
  c_custkey as key,
  c_name as name,
  c_acctbal as balance,
  c_comment as comment,
  { 'street': c_address, 'address': {'nation': c_nationkey, 'phone': c_phone }} AS contact,
  { 'segment': c_mktsegment } AS segment
FROM '{{{conn.path}}}'
WHERE 1=1
{{#params.id}}
  AND c_custkey = {{{ params.id }}}
{{/params.id}}
{{#params.segment}}
  AND c_mktsegment LIKE '%{{{ params.segment }}}%'
{{/params.segment}}
