-- Northwind Customers Query
-- Returns customer information with optional filtering by CustomerID

SELECT
    CustomerID,
    CompanyName,
    ContactName,
    ContactTitle,
    Address,
    City,
    Region,
    PostalCode,
    Country,
    Phone,
    Fax
FROM nw.Customers
WHERE 1=1
{{#params.customer_id}}
    AND CustomerID = '{{{ params.customer_id }}}'
{{/params.customer_id}}
{{#params.company_name}}
    AND CompanyName LIKE '%{{{ params.company_name }}}%'
{{/params.company_name}}
{{#params.country}}
    AND Country = '{{{ params.country }}}'
{{/params.country}}
{{#params.city}}
    AND City = '{{{ params.city }}}'
{{/params.city}}
ORDER BY CompanyName

