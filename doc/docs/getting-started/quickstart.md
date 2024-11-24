---
sidebar_position: 3
---

# Quick Start Guide

This guide will help you get flAPI up and running with a basic configuration.

## 1. Basic Setup

Create a basic configuration file `flapi.yaml`:

```yaml
project_name: example-flapi-project
project_description: A basic flAPI project

template:
  path: './sqls'
  environment-whitelist:
    - '^FLAPI_.*'

duckdb:
  db_path: ./flapi_cache.db
  access_mode: READ_WRITE
  threads: 8
  max_memory: 8GB
```

## 2. Create Your First Endpoint

1. Create a directory for SQL templates:
```bash
mkdir sqls
```

2. Create your first endpoint configuration (`sqls/customers.yaml`):
```yaml
url-path: /customers/
request:
  - field-name: id
    field-in: query
    description: Customer ID
    required: false
    validators:
      - type: int
        min: 1
template-source: customers.sql
```

3. Create the SQL template (`sqls/customers.sql`):
```sql
SELECT * FROM customers
WHERE 1=1
{{#params.id}}
  AND customer_id = {{{ params.id }}}
{{/params.id}}
```

## 3. Run flAPI

Start the server:

```bash
docker run -it --rm -p 8080:8080 -v $(pwd):/config \
  ghcr.io/datazoode/flapi -c /config/flapi.yaml
```

Your API is now available at `http://localhost:8080`! 