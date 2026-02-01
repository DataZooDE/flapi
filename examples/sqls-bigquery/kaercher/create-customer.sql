SET VARIABLE new_customer_id = gen_random_uuid()::VARCHAR;
SELECT * FROM bigquery_execute('{{{ conn.project_id }}}', concat('CALL `d-kaercher-kaadala-mgmt.products.create_customer`(''', '{{{ params.name }}}', ''', ''', getvariable('new_customer_id'), ''')'));
SELECT * FROM bigquery_query('{{{ conn.project_id }}}', concat('SELECT * FROM d-kaercher-kaadala-mgmt.products.customers WHERE customer_id = ''', getvariable('new_customer_id'), ''''))
