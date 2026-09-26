SELECT id, name, segment, balance
FROM read_csv('{{{conn.path}}}')
WHERE 1=1
{{#params.id}}
  AND id = {{ params.id }}
{{/params.id}}
LIMIT 3
