
SELECT 
  *
FROM '{{{conn.path}}}'
WHERE 1=1
{{#params.id}}
  AND c_custkey = {{{ params.id }}}
{{/params.id}}

