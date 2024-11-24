---
sidebar_position: 2
---

# Caching

flAPI implements a powerful caching mechanism to optimize query performance and reduce load on data sources.

## Cache Configuration

In your endpoint YAML file:
```yaml
cache:
  cache-table-name: 'products_cache'
  cache-source: products_cache.sql
  refresh-time: 15m
```

## Cache Creation

Define how to populate the cache in your SQL file:
```sql
CREATE TABLE {{cache.schema}}.{{cache.table}} AS
SELECT 
    category,
    brand,
    COUNT(*) as product_count,
    AVG(price) as avg_price
FROM source_table
GROUP BY 1, 2
```

## Cache Management

- Automatic refresh based on `refresh-time`
- Version management of cache tables
- Garbage collection of old caches
- Manual invalidation via HTTP DELETE 