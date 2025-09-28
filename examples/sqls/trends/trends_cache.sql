SELECT country_name, country_code, region_name, region_code, term, week, score, rank, refresh_date
FROM bigquery_scan('bigquery-public-data.google_trends.international_top_terms') 
WHERE country_code = 'DE'
ORDER BY refresh_date DESC
LIMIT 1000