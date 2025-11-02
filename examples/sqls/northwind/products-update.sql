-- Northwind Products Update (UPDATE)
-- Updates an existing product and returns the updated record
-- Uses conditional updates: only updates fields that are provided

UPDATE nw.Products
SET
{{#params.product_name}}
    ProductName = '{{{ params.product_name }}}',
{{/params.product_name}}
{{#params.supplier_id}}
    SupplierID = {{{ params.supplier_id }}},
{{/params.supplier_id}}
{{#params.category_id}}
    CategoryID = {{{ params.category_id }}},
{{/params.category_id}}
{{#params.quantity_per_unit}}
    QuantityPerUnit = '{{{ params.quantity_per_unit }}}',
{{/params.quantity_per_unit}}
{{#params.unit_price}}
    UnitPrice = {{{ params.unit_price }}},
{{/params.unit_price}}
{{#params.units_in_stock}}
    UnitsInStock = {{{ params.units_in_stock }}},
{{/params.units_in_stock}}
{{#params.units_on_order}}
    UnitsOnOrder = {{{ params.units_on_order }}},
{{/params.units_on_order}}
{{#params.reorder_level}}
    ReorderLevel = {{{ params.reorder_level }}},
{{/params.reorder_level}}
{{#params.discontinued}}
    Discontinued = {{{ params.discontinued }}}
{{/params.discontinued}}
{{^params.discontinued}}
    -- Ensure at least one field is always updated to avoid trailing comma issue
    Discontinued = Discontinued
{{/params.discontinued}}
WHERE ProductID = {{{ params.product_id }}};
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
WHERE ProductID = {{{ params.product_id }}}
