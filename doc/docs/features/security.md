---
sidebar_position: 3
---

# Security

flAPI provides multiple security features to protect your data.

## Row-Level Security

Restrict access to specific rows based on user roles:
```sql
SELECT * FROM data
WHERE 1=1
{{#context.user.roles.admin}}
  -- Admins see all data
{{/context.user.roles.admin}}
{{^context.user.roles.admin}}
  AND department = '{{{context.user.department}}}'
{{/context.user.roles.admin}}
```

## Column-Level Security

Control access to specific columns:
```yaml
columns:
  restricted:
    - salary
    - ssn
  roles:
    admin:
      - '*'
    user:
      - name
      - department
```

## HTTPS Enforcement

Enable HTTPS in configuration:
```yaml
enforce-https:
  enabled: true
  ssl-cert-file: './ssl/cert.pem'
  ssl-key-file: './ssl/key.pem'
```

## Query Validation

Prevent SQL injection and validate queries:
```yaml
validators:
  preventSqlInjection: true
  validateSyntax: true
``` 