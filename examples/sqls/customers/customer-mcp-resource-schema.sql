-- MCP Resource: Customer Schema Definition
-- This SQL generates a JSON description of the customer table schema
SELECT json_object(
  'table_name', 'customers',
  'description', 'Customer information from TPC-H dataset',
  'columns', json_array(
    json_object('name', 'c_custkey', 'type', 'INTEGER', 'description', 'Unique customer key'),
    json_object('name', 'c_name', 'type', 'VARCHAR', 'description', 'Customer name'),
    json_object('name', 'c_address', 'type', 'VARCHAR', 'description', 'Customer address'),
    json_object('name', 'c_nationkey', 'type', 'INTEGER', 'description', 'Nation key reference'),
    json_object('name', 'c_phone', 'type', 'VARCHAR', 'description', 'Customer phone number'),
    json_object('name', 'c_acctbal', 'type', 'DOUBLE', 'description', 'Account balance'),
    json_object('name', 'c_mktsegment', 'type', 'VARCHAR', 'description', 'Market segment (AUTOMOBILE, BUILDING, FURNITURE, HOUSEHOLD, MACHINERY)'),
    json_object('name', 'c_comment', 'type', 'VARCHAR', 'description', 'Customer comment')
  )
) as schema_definition;
