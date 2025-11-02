-- Delete Order SQL Template
-- This template deletes an order record
-- Note: In a production system, you might want to soft-delete (mark as deleted)

DELETE FROM orders
WHERE order_id = {{params.order_id}}

