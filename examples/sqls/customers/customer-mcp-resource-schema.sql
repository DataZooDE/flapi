-- MCP Resource: Customer Schema Definition
-- This SQL generates a JSON description of the customer table schema
SELECT json_object(
  'table_name', 'customers',
  'description', 'Customer information table',
  'columns', json_array(
    json_object('name', 'customer_id', 'type', 'INTEGER', 'description', 'Unique customer identifier'),
    json_object('name', 'name', 'type', 'VARCHAR', 'description', 'Customer full name'),
    json_object('name', 'email', 'type', 'VARCHAR', 'description', 'Customer email address'),
    json_object('name', 'segment', 'type', 'VARCHAR', 'description', 'Customer segment (retail, corporate, vip)'),
    json_object('name', 'registration_date', 'type', 'DATE', 'description', 'Customer registration date'),
    json_object('name', 'total_orders', 'type', 'INTEGER', 'description', 'Total number of orders'),
    json_object('name', 'lifetime_value', 'type', 'DECIMAL', 'description', 'Customer lifetime value')
  )
) as schema_definition;
