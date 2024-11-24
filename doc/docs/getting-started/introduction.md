---
sidebar_position: 1
---

# Introduction to flAPI

flAPI is a powerful service that automatically generates read-only APIs for datasets by utilizing SQL templates. Built on top of [DuckDB](https://duckdb.org/) and leveraging its SQL engine and extension ecosystem, flAPI offers a seamless way to connect to various data sources and expose them as RESTful APIs.

## Key Features

### 🚀 Automatic API Generation
Create APIs for your datasets without coding. Just write SQL templates and flAPI handles the rest.

### 🔌 Multiple Data Sources
Connect to:
- BigQuery
- SAP ERP & BW (via ERPL)
- Parquet files
- Apache Iceberg
- PostgreSQL
- MySQL
- And more through DuckDB's extension ecosystem

### 📝 SQL Templates
Use Mustache-like syntax for dynamic queries that adapt to API parameters.

### ⚡ Built-in Caching
- Improve performance with intelligent caching
- Reduce database costs
- Configurable refresh intervals
- Version management of cache tables

### 🔒 Security Features
- Row-level security
- Column-level security
- Authentication support
- Rate limiting

### 📦 Easy Deployment
Deploy flAPI with a single binary file or use our Docker image.

## Quick Start

The easiest way to get started with flAPI is to use our pre-built Docker image:

```bash
# Pull the Docker image
docker pull ghcr.io/datazoode/flapi:latest

# Run flAPI
docker run -it --rm -p 8080:8080 -v $(pwd)/examples/:/config \
  ghcr.io/datazoode/flapi -c /config/flapi.yaml
```

Visit [http://localhost:8080/doc](http://localhost:8080/doc) to see your API documentation.

## Next Steps

- [Installation Guide](./installation)
- [Quick Start Guide](./quickstart)
- [SQL Templates](../features/sql-templates)
- [Security](../features/security)
- [Caching](../features/caching) 