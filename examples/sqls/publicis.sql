SELECT 
    p.country,
    p.product_category,
    p.campaign_type,
    p.channel,
    p.clicks as clicks
FROM {{{cache.table}}} AS p
WHERE 1=1
{{#params.country}}
 AND p.country LIKE '{{{ params.country }}}'
{{/params.country}}
{{#params.channel}}
 AND p.channel LIKE '{{{ params.channel }}}'
{{/params.channel}}
