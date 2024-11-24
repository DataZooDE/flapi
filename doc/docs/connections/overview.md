---
sidebar_position: 1
---

# Connections Overview

flAPI supports multiple database connections through DuckDB's extensive ecosystem of extensions. Configure your connections in the `@flapi.yaml` file.

## Basic Configuration

Connections are defined in the `connections` section of your `@flapi.yaml`:

```yaml
connections:
  my-connection-name:
    init: |
      # SQL commands to initialize the connection
      INSTALL extension_name;
      LOAD extension_name;
    properties:
      # Connection-specific properties
      property1: value1
      property2: value2
```

## Configuration Options

| Option | Description | Required | Default |
|--------|-------------|----------|---------|
| `init` | SQL commands to initialize the connection (e.g., loading extensions) | No | - |
| `properties` | Connection-specific properties accessible in templates | No | - |
| `max_connections` | Maximum number of concurrent connections | No | 5 |
| `timeout` | Connection timeout in seconds | No | 30 |

## Environment Variables

You can use environment variables in your connection configuration:

```yaml
connections:
  my-db:
    properties:
      username: ${DB_USER}
      password: ${DB_PASSWORD}
```

## Connection Pooling

flAPI automatically manages connection pools for each configured database:

```yaml
connections:
  my-db:
    pool:
      min_size: 1
      max_size: 10
      idle_timeout: 300
```

## Health Checks

Configure health checks for your connections:

```yaml
connections:
  my-db:
    health_check:
      enabled: true
      interval: 60
      query: SELECT 1
```