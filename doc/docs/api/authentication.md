---
sidebar_position: 2
---

# Authentication

flAPI supports multiple authentication methods to secure your API endpoints.

## Basic Authentication

Configure basic auth in your endpoint YAML:
```yaml
auth:
  enabled: true
  type: basic
  users:
    - username: admin
      password: secret
      roles: [admin]
    - username: user
      password: password
      roles: [read]
```

## Bearer Token Authentication

For JWT-based authentication:
```yaml
auth:
  enabled: true
  type: bearer
  jwt:
    secret: your-secret-key
    algorithm: HS256
```

## Role-Based Access Control

Define permissions based on roles:
```yaml
auth:
  roles:
    admin:
      - READ
      - WRITE
      - DELETE
    read:
      - READ
``` 