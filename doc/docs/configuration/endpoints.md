---
sidebar_position: 3
---

# Endpoint Configuration

Define API endpoints using YAML configuration files.

## Basic Structure

```yaml
url-path: /api/data
request:
  - field-name: id
    field-in: query
    description: Record ID
    required: false
    validators:
      - type: int
        min: 1
template-source: query.sql
connection: my-database
```

## Request Parameters

### Parameter Types
- `query`: URL query parameters
- `path`: URL path parameters
- `header`: HTTP header parameters
- `body`: Request body parameters

### Validators
```yaml
validators:
  - type: string
    pattern: '^[A-Za-z0-9]+$'
  - type: int
    min: 1
    max: 1000
  - type: date
    format: 'YYYY-MM-DD'
```

## Response Configuration

```yaml
response:
  format: json
  headers:
    Cache-Control: 'max-age=3600'
  pagination:
    enabled: true
    limit: 100
``` 