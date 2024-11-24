---
sidebar_position: 1
---

# Configuration Overview

flAPI uses YAML configuration files to define its behavior. The main configuration file typically contains the following sections:

## Project Settings
```yaml
project_name: example-flapi-project
project_description: A basic flAPI project
```

## Template Configuration
```yaml
template:
  path: './sqls'            # Path to SQL templates
  environment-whitelist:    # Allowed environment variables
    - '^FLAPI_.*'
```

## Database Configuration
```yaml
duckdb:
  db_path: ./flapi_cache.db
  access_mode: READ_WRITE
  threads: 8
  max_memory: 8GB
  default_order: DESC
```

## Connection Settings
```yaml
connections:
  bigquery-lakehouse:
    init: |
      INSTALL 'bigquery';
      LOAD 'bigquery';
    properties:
      project_id: 'my-project-id'
```

## Security Settings
```yaml
enforce-https:
  enabled: false
  # ssl-cert-file: './ssl/cert.pem'
  # ssl-key-file: './ssl/key.pem'
``` 