-- Patch Customer SQL Template
-- This template performs a partial update (only email in this example)

UPDATE customers
SET
  {{#if params.email}}email = '{{params.email}}',{{/if}}
  updated_at = CURRENT_TIMESTAMP
WHERE customer_id = {{params.customer_id}}
RETURNING customer_id, name, email, phone, created_at, updated_at

