---
sidebar_position: 1
---

# API Overview

flAPI automatically generates RESTful APIs from your SQL templates.

## Endpoint Structure

Each API endpoint consists of:
1. YAML configuration file
2. SQL template file

### Basic Example

`customers.yaml`:
```yaml
url-path: /customers/
request:
  - field-name: id
    field-in: query
    description: Customer ID
    required: false
```

`customers.sql`:
```sql
SELECT * FROM customers
WHERE 1=1
{{#params.id}}
  AND customer_id = {{{ params.id }}}
{{/params.id}}
```

## Common Response Format

All API responses follow this structure:
```json
{
  "next": "",
  "total_count": 100,
  "data": [
    // Your query results here
  ]
} 