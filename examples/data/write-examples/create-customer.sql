-- Create Customer SQL Template
-- This template inserts a new customer record and returns the created data

INSERT INTO customers (name, email, phone, created_at, updated_at)
VALUES (
  '{{params.name}}',
  '{{params.email}}',
  {{#if params.phone}}'{{params.phone}}'{{else}}NULL{{/if}},
  CURRENT_TIMESTAMP,
  CURRENT_TIMESTAMP
)
RETURNING customer_id, name, email, phone, created_at, updated_at

