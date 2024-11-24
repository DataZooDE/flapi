---
sidebar_position: 2
---

# Database Configuration

Configure DuckDB and database connections in flAPI.

## DuckDB Settings

```yaml
duckdb:
  db_path: ./flapi_cache.db    # Path to database file
  access_mode: READ_WRITE      # Database access mode
  threads: 8                   # Number of threads
  max_memory: 8GB             # Maximum memory usage
  default_order: DESC         # Default sorting order
```

## Connection Types

### BigQuery Connection
```yaml
connections:
  bigquery:
    init: |
      INSTALL 'bigquery';
      LOAD 'bigquery';
    properties:
      project_id: 'my-project'
```

### Parquet Files
```yaml
connections:
  parquet:
    properties:
      path: './data/*.parquet'
```

### PostgreSQL
```yaml
connections:
  postgres:
    init: |
      INSTALL postgres;
      LOAD postgres;
    properties:
      host: localhost
      port: 5432
      database: mydb
``` 