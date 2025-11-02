-- Northwind Orders Query with Order Details
-- Returns order information with associated order details

SELECT
    o.OrderID,
    o.CustomerID,
    o.EmployeeID,
    o.OrderDate,
    o.RequiredDate,
    o.ShippedDate,
    o.ShipVia,
    o.Freight,
    o.ShipName,
    o.ShipAddress,
    o.ShipCity,
    o.ShipRegion,
    o.ShipPostalCode,
    o.ShipCountry,
    -- Aggregate order details into a JSON array using LIST
    LIST(
        {
            'ProductID': od.ProductID,
            'UnitPrice': od.UnitPrice,
            'Quantity': od.Quantity,
            'Discount': od.Discount
        }
    ) FILTER (WHERE od.ProductID IS NOT NULL) AS OrderDetails
FROM nw.Orders o
LEFT JOIN nw."Order Details" od ON o.OrderID = od.OrderID
WHERE 1=1
{{#params.order_id}}
    AND o.OrderID = {{{ params.order_id }}}
{{/params.order_id}}
{{#params.customer_id}}
    AND o.CustomerID = '{{{ params.customer_id }}}'
{{/params.customer_id}}
{{#params.order_date}}
    AND DATE(o.OrderDate) = DATE('{{{ params.order_date }}}')
{{/params.order_date}}
{{#params.shipped_date}}
    AND DATE(o.ShippedDate) = DATE('{{{ params.shipped_date }}}')
{{/params.shipped_date}}
GROUP BY
    o.OrderID,
    o.CustomerID,
    o.EmployeeID,
    o.OrderDate,
    o.RequiredDate,
    o.ShippedDate,
    o.ShipVia,
    o.Freight,
    o.ShipName,
    o.ShipAddress,
    o.ShipCity,
    o.ShipRegion,
    o.ShipPostalCode,
    o.ShipCountry
ORDER BY o.OrderDate DESC, o.OrderID

