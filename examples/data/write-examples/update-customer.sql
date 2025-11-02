-- Update Customer SQL Template
-- This template updates an existing customer record and returns the updated data

UPDATE customers
SET
  {{#if params.name}}name = '{{params.name}}',{{/if}}
  {{#if params.email}}email = '{{params.email}}',{{/if}}
  {{#if params.phone}}phone = '{{params.phone}}',{{/if}}
  updated_at = CURRENT_TIMESTAMP
WHERE customer_id = {{params.customer_id}}
RETURNING customer_id, name, email, phone, created_at, updated_at

