---
sidebar_position: 3
---

# Rate Limiting

Control access to your API endpoints with built-in rate limiting.

## Configuration

In your endpoint YAML:
```yaml
rate-limit:
  enabled: true
  max: 100        # Maximum requests
  interval: 60    # Time interval in seconds
```

## Rate Limit Headers

Response headers include rate limit information:
```
X-RateLimit-Limit: 100
X-RateLimit-Remaining: 95
X-RateLimit-Reset: 1640995200
```

## Per-User Limits

Configure different limits for different users:
```yaml
rate-limit:
  enabled: true
  rules:
    - user: admin
      max: 1000
      interval: 60
    - user: basic
      max: 100
      interval: 60
``` 