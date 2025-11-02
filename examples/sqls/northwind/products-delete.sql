-- Northwind Products Delete (DELETE)
-- Deletes a product from the Northwind database

DELETE FROM nw.Products
WHERE ProductID = {{{ params.product_id }}}

