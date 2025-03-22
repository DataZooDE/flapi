
SELECT 
  c.*
FROM {{{cache.schema}}}.{{{cache.table}}} AS c
WHERE 1=1
{{#params.id}}
  AND c.c_custkey = {{{ params.id }}}
{{/params.id}}

