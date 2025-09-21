-- Simple resource that returns sample data summary
SELECT
    'Sample Customer Data Summary' as summary_title,
    '3' as total_customers,
    '2' as unique_segments,
    '100%' as customers_with_registration_date,
    '2023-01-01' as earliest_registration,
    '2023-12-31' as latest_registration;
