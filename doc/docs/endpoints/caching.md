---
sidebar_position: 9
---

# Caching

Learn about Flapi's caching capabilities and optimize API performance.

## Cache Configuration

Configure caching in your endpoint YAML:

```yaml
cache:
  cache-table-name: 'products_cache'
  cache-source: products_cache.sql
  refresh-time: 15m
```

## Caching Strategies

### Memory Cache

In-memory caching for fastest access:

```yaml
caching:
  type: memory
  ttl: 3600  # Time to live in seconds
```

### Redis Cache

Distributed caching with Redis:

```yaml
caching:
  type: redis
  host: localhost
  port: 6379
  ttl: 3600
```

## Cache Invalidation

### Time-based Invalidation

Cache entries automatically expire after TTL:

```yaml
caching:
  ttl: 3600  # Entries expire after 1 hour
```

### Manual Invalidation

Invalidate cache entries programmatically:

```yaml
endpoints:
  /data:
    cache:
      invalidate:
        - on: POST /update
        - on: PUT /update
```
