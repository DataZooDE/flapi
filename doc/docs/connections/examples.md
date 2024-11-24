---
sidebar_position: 2
---

# Connection Examples

Examples of connecting to various data sources using flAPI.

## Local Files

### Parquet Files

```yaml
connections:
  customers-parquet:
    properties:
      path: './data/customers.parquet'

# Usage in SQL template:
SELECT * FROM '{{ conn.path }}'
```

### CSV Files

```yaml
connections:
  sales-csv:
    init: |
      SET csv_auto_detect=true;
    properties:
      path: './data/sales.csv'
```

## Databases

### PostgreSQL

```yaml
connections:
  postgres-db:
    init: |
      INSTALL postgres;
      LOAD postgres;
    properties:
      host: localhost
      port: 5432
      database: mydb
      username: ${PG_USER}
      password: ${PG_PASSWORD}

# Usage in SQL template:
SELECT * FROM postgres_scan('{{ conn.database }}', 'public', 'users')
```

### MySQL

```yaml
connections:
  mysql-db:
    init: |
      INSTALL mysql;
      LOAD mysql;
    properties:
      host: localhost
      port: 3306
      database: mydb
```

### SQLite

```yaml
connections:
  sqlite-db:
    init: |
      INSTALL sqlite;
      LOAD sqlite;
    properties:
      path: './data/local.db'
```

## Cloud Services

### BigQuery

```yaml
connections:
  bigquery-lakehouse:
    init: |
      INSTALL bigquery;
      LOAD bigquery;
    properties:
      project_id: 'my-project'
      dataset: 'my_dataset'
      credentials_path: '${GOOGLE_APPLICATION_CREDENTIALS}'
```

### Snowflake

```yaml
connections:
  snowflake-dw:
    init: |
      INSTALL snowflake;
      LOAD snowflake;
    properties:
      account: 'my_account'
      warehouse: 'my_warehouse'
      database: 'my_database'
```

## Enterprise Systems

### SAP ERP (via ERPL)

```yaml
connections:
  sap-erp:
    init: |
      INSTALL 'erpl' FROM 'http://get.erpl.io';
      LOAD 'erpl';
    properties:
      ashost: 'sap.example.com'
      sysnr: '00'
      client: '100'
      user: ${SAP_USER}
      password: ${SAP_PASSWORD}

# Usage in SQL template:
SELECT * FROM sap_read_table('VBAK')
```

### Apache Iceberg

```yaml
connections:
  iceberg-lake:
    init: |
      INSTALL iceberg;
      LOAD iceberg;
    properties:
      warehouse_path: 's3://my-bucket/warehouse'
      aws_region: 'us-east-1'
```

## Advanced Examples

### Multiple Connections

```yaml
connections:
  source-db:
    init: |
      INSTALL postgres;
      LOAD postgres;
    properties:
      host: source.example.com
      database: source_db
      
  target-dw:
    init: |
      INSTALL snowflake;
      LOAD snowflake;
    properties:
      account: 'target_account'
      warehouse: 'target_wh'

# Usage in SQL template for data migration:
INSERT INTO snowflake_table 
SELECT * FROM postgres_scan('source_db', 'public', 'users')
``` 