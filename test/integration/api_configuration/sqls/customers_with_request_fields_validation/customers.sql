
SELECT 
  *
FROM '{{{conn.path}}}'
WHERE 1=1
{{#params.id}}
  AND c_custkey = {{{ params.id }}}
{{/params.id}}
{{#params.segment}}
  AND c_mktsegment LIKE '%{{{ params.segment }}}%'
{{/params.segment}}
