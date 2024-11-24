---
sidebar_position: 1
---

# Quickstart

## What is Flapi?

Flapi is a lightweight framework that helps you create REST APIs from SQL queries without writing any code. It's particularly useful when you need to serve data from files or databases to applications through a standardized API interface.

## Quick Start Example

Let's solve a common problem: You have customer data in a Parquet file and want to serve it through a REST API with proper validation and filtering capabilities.

### 1. Get Flapi

You can either download the binary:

```bash
curl -L https://github.com/datazoode/flapi/releases/latest/download/flapi -o flapi
chmod +x flapi
```

Or use Docker:

```bash
docker pull ghcr.io/datazoode/flapi:latest
```

### 2. Create Minimal Configuration

Create a `flapi.yaml` file:

```yaml
project_name: customer-api
project_description: API for customer data

template:
  path: './sqls'

connections:
  customers-parquet:
    properties:
      path: './data/customers.parquet'

duckdb:
  access_mode: READ_WRITE
```

### 3. Create Your First Endpoint

1. Create the SQL templates directory:
```bash
mkdir sqls
```

2. Create an endpoint configuration (`sqls/customers.yaml`):
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
connection: 
  - customers-parquet
```

3. Create the SQL template (`sqls/customers.sql`):
```sql
SELECT * FROM customers
WHERE 1=1
{{#params.id}}
  AND customer_id = {{{ params.id }}}
{{/params.id}}
```

### 4. Run Flapi

Using the binary:
```bash
./flapi -c flapi.yaml
```

Or with Docker:
```bash
docker run -it --rm -p 8080:8080 -v $(pwd):/config \
  ghcr.io/datazoode/flapi -c /config/flapi.yaml
```

Your API is now available at `http://localhost:8080/customers/`!

## Why Flapi?

Data teams often face challenges when sharing data with applications and services. Traditional approaches require:

1. Writing custom API code
2. Implementing data validation
3. Setting up authentication
4. Managing database connections
5. Handling error cases
6. Writing documentation

Flapi solves these challenges by providing:

### Declarative API Definition
- Define APIs using YAML configuration
- Automatic parameter validation
- Built-in SQL injection prevention
- OpenAPI documentation generation

### Powerful Data Access
- Connect to Parquet files, DuckDB, and BigQuery
- SQL templating with Mustache
- Query result caching
- Connection pooling

### Enterprise Features
- Authentication (Basic Auth)
- Rate limiting
- CORS support
- HTTPS enforcement
- Health checks

## Main Features

### Data Sources
- **File Formats**: Direct connection to Parquet files
- **Databases**: DuckDB and BigQuery support
- **Extensible**: Plugin system for additional data sources

### API Features
- **Parameter Validation**: Type checking, ranges, enums, regex
- **SQL Templates**: Mustache templating for dynamic queries
- **Caching**: Query result caching with DuckDB
- **Authentication**: Basic auth with user roles
- **Rate Limiting**: Configurable request limits

### Developer Experience
- **Zero Code**: Create APIs with just YAML and SQL
- **OpenAPI**: Automatic API documentation
- **Docker Support**: Easy deployment with containers
- **Monitoring**: Built-in health checks and metrics

## Next Steps

Now that you have your first API endpoint running, you can:

1. Add more complex validations to your endpoints
2. Configure authentication for secure access
3. Connect to different data sources like BigQuery
4. Set up rate limiting and monitoring

Check out the rest of our documentation to learn more about these features.