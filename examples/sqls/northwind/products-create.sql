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
    {{#params.quantity_per_unit}}
    '{{{ params.quantity_per_unit }}}'
    {{/params.quantity_per_unit}}
    {{^params.quantity_per_unit}}
    NULL
    {{/params.quantity_per_unit}},
    {{#params.unit_price}}
    {{{ params.unit_price }}}
    {{/params.unit_price}}
    {{^params.unit_price}}
    0.0
    {{/params.unit_price}},
    {{#params.units_in_stock}}
    {{{ params.units_in_stock }}}
    {{/params.units_in_stock}}
    {{^params.units_in_stock}}
    0
    {{/params.units_in_stock}},
    {{#params.units_on_order}}
    {{{ params.units_on_order }}}
    {{/params.units_on_order}}
    {{^params.units_on_order}}
    0
    {{/params.units_on_order}},
    {{#params.reorder_level}}
    {{{ params.reorder_level }}}
    {{/params.reorder_level}}
    {{^params.reorder_level}}
    0
    {{/params.reorder_level}},
    {{#params.discontinued}}
    {{{ params.discontinued }}}
    {{/params.discontinued}}
    {{^params.discontinued}}
    0
    {{/params.discontinued}}
);

SELECT *
FROM nw.Products
WHERE ProductID = (SELECT MAX(ProductID) FROM nw.Products);
