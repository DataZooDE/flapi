-- Northwind Products Create (INSERT)
-- Creates a new product and returns the created record

INSERT INTO nw.Products (
    ProductName,
    SupplierID,
    CategoryID,
    QuantityPerUnit,
    UnitPrice,
    UnitsInStock,
    UnitsOnOrder,
    ReorderLevel,
    Discontinued
)
VALUES (
    '{{{ params.product_name }}}',
    {{{ params.supplier_id }}},
    {{{ params.category_id }}},
    '{{{ params.quantity_per_unit }}}',
    {{{ params.unit_price }}},
    {{{ params.units_in_stock }}},
    {{{ params.units_on_order }}},
    {{{ params.reorder_level }}},
    {{{ params.discontinued }}}
)
RETURNING
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

