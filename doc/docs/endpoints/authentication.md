---
sidebar_position: 8
---

# Authentication

Secure your API endpoints with various authentication methods.

## Basic Authentication

Basic authentication is the simplest form of authentication where credentials are sent in the HTTP header.

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

## JWT Authentication

JSON Web Tokens provide a secure way to authenticate and authorize API requests.

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
