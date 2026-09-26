SELECT name, SUM(number) AS total
FROM bigquery_scan('bigquery-public-data.usa_names.usa_1910_2013',
                   billing_project='{{{ conn.project_id }}}')
WHERE state = {{ params.state }}
GROUP BY name
ORDER BY total DESC
LIMIT {{#params.top}}{{ params.top }}{{/params.top}}{{^params.top}}5{{/params.top}}
