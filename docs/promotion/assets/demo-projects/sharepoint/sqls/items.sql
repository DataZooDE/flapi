SELECT id, Title AS project, Number AS pct_complete, Date AS due
FROM sp.main."{{{ conn.list }}}"
WHERE Title IS NOT NULL
  AND Number >= {{#params.min_pct}}{{ params.min_pct }}{{/params.min_pct}}{{^params.min_pct}}10{{/params.min_pct}}
ORDER BY Number
