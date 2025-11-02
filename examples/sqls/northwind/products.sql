-- Northwind Products Query
-- Returns product information with optional filtering by ProductID

SELECT
    ProductID,
    ProductName,
    SupplierID,
    CategoryID,
    QuantityPerUnit,
    UnitPrice,
    UnitsInStock,
    UnitsOnOrder,
    ReorderLevel,
    Discontinued
FROM nw.Products
WHERE 1=1
{{#params.product_id}}
    AND ProductID = {{{ params.product_id }}}
{{/params.product_id}}
{{#params.product_name}}
    AND ProductName LIKE '%{{{ params.product_name }}}%'
{{/params.product_name}}
{{#params.category_id}}
    AND CategoryID = {{{ params.category_id }}}
{{/params.category_id}}
{{#params.supplier_id}}
    AND SupplierID = {{{ params.supplier_id }}}
{{/params.supplier_id}}
{{#params.discontinued}}
    AND Discontinued = {{{ params.discontinued }}}
{{/params.discontinued}}
ORDER BY ProductName

