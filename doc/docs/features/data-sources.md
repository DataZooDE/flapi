---
sidebar_position: 4
---

# Data Sources

flAPI supports multiple data sources through DuckDB's extension ecosystem.

## Supported Sources

### BigQuery
Connect to Google BigQuery:
```yaml
connections:
  bigquery:
    init: |
      INSTALL 'bigquery';
      LOAD 'bigquery';
    properties:
      project_id: 'my-project'
```

### SAP ERP & BW
Connect via ERPL:
```yaml
connections:
  sap:
    init: |
      INSTALL 'erpl';
      LOAD 'erpl';
```

### File Formats
- Parquet files
- CSV files
- JSON files
- Apache Iceberg

### Databases
- PostgreSQL
- MySQL
- SQLite 