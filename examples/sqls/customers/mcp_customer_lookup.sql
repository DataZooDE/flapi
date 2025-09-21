-- MCP Tool: Customer Lookup
-- Description: Retrieve customer information by ID with optional order history
SELECT
  c.customer_id,
  c.name,
  c.email,
  c.registration_date,
  c.segment,
  {{#params.include_orders}}
  -- Include recent orders if requested
  ARRAY_AGG(
    JSON_OBJECT(
      'order_id', o.order_id,
      'order_date', o.order_date,
      'total_amount', o.total_amount,
      'status', o.status
    )
  ) FILTER (WHERE o.order_id IS NOT NULL) as recent_orders
  {{/params.include_orders}}
  {{^params.include_orders}}
  NULL as recent_orders
  {{/params.include_orders}}
FROM customers c
{{#params.include_orders}}
LEFT JOIN orders o ON c.customer_id = o.customer_id
  AND o.order_date >= DATE('now', '-90 days')  -- Last 90 days
{{/params.include_orders}}
WHERE c.customer_id = '{{{ params.customer_id }}}'
{{#params.include_orders}}
GROUP BY c.customer_id, c.name, c.email, c.registration_date, c.segment
{{/params.include_orders}}
LIMIT 1;
