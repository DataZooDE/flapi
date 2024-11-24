---
sidebar_position: 1
---

# SQL Templates

flAPI uses Mustache-style templates to create dynamic SQL queries.

## Basic Syntax

```sql
SELECT * FROM users
WHERE 1=1
{{#params.id}}
  AND id = {{{params.id}}}
{{/params.id}}
{{#params.status}}
  AND status = '{{{params.status}}}'
{{/params.status}}
```

## Context Variables

Access various context data in your templates:

### User Context
```sql
SELECT * FROM orders
WHERE user_id = '{{{context.user.id}}}'
```

### Connection Properties
```sql
SELECT * FROM '{{{context.conn.table}}}'
WHERE region = '{{{context.conn.region}}}'
```

### Environment Variables
```sql
SELECT * FROM logs
WHERE environment = '{{{env.FLAPI_ENV}}}'
```

## Advanced Features

### Subqueries
```sql
WITH filtered_data AS (
  {{> common/filter_logic}}
)
SELECT * FROM filtered_data
```

### Conditional Logic
```sql
SELECT
  id,
  name,
  {{#context.user.roles.admin}}
    salary,
  {{/context.user.roles.admin}}
  department
FROM employees
``` 