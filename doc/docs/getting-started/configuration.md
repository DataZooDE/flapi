---
sidebar_position: 4
---

# Configuration

flAPI uses YAML files for configuration. The main configuration file (`flapi.yaml`) defines global settings and connections.

## Basic Configuration

```yaml
# Project information
project_name: example-flapi-project
project_description: An example flAPI project

# Template configuration
template:
  path: './sqls'            # Path to SQL templates and endpoint configs
  environment-whitelist:    # Optional: Environment variable whitelist
    - '^FLAPI_.*'
```

## DuckDB Settings

Configure the embedded DuckDB instance:

```yaml
duckdb:
  db_path: ./flapi_cache.db  # Optional: Path for persistent storage
  access_mode: READ_WRITE    # Database access mode
  threads: 8                 # Number of threads to use
  max_memory: 8GB           # Maximum memory usage
  default_order: DESC       # Default sorting order
```

## Connection Definitions

Define your data source connections:

```yaml
connections:
  # BigQuery connection example
  bigquery-lakehouse: 
    init: |
      INSTALL 'bigquery';
      LOAD 'bigquery';
    properties:
      project_id: 'my-project-id'

  # Parquet files connection
  customers-parquet:
    properties:
      path: './data/customers.parquet'
```

## Security Settings

Configure HTTPS and other security features:

```yaml
enforce-https:
  enabled: false            # Force HTTPS for all connections
  # ssl-cert-file: './ssl/cert.pem'
  # ssl-key-file: './ssl/key.pem'
```

## Heartbeat Configuration

Set up periodic health checks:

```yaml
heartbeat:
  enabled: true            # Enable heartbeat worker
  worker-interval: 10      # Interval in seconds
```

## Environment Variables

You can use environment variables in your configuration using the `${VAR_NAME}` syntax:

```yaml
connections:
  bigquery:
    properties:
      project_id: '${GOOGLE_CLOUD_PROJECT}'
```

Only variables matching the patterns in `environment-whitelist` will be available.

## Complete Example

Here's a complete configuration example:

```yaml
project_name: example-flapi-project
project_description: An example flAPI project

template:
  path: './sqls'
  environment-whitelist:
    - '^FLAPI_.*'

connections:
  bigquery-lakehouse: 
    init: |
      INSTALL 'bigquery';
      LOAD 'bigquery';
    properties:
      project_id: 'my-project-id'

  customers-parquet:
    properties:
      path: './data/customers.parquet'

heartbeat:
  enabled: true
  worker-interval: 10

enforce-https:
  enabled: false

duckdb:
  db_path: ./flapi_cache.db
  access_mode: READ_WRITE
  threads: 8
  max_memory: 8GB
  default_order: DESC
```

## Next Steps

- Learn about [creating your first API](./first-api)
- Explore [connection types](../connections/overview)
- Configure [endpoints](../endpoints/overview) 