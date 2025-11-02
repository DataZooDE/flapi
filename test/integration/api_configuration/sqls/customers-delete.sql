-- Delete customer (DELETE)

DELETE FROM test_customers
WHERE id = {{{ params.customer_id }}};

