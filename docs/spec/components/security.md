# Security Architecture

This document describes the authentication, authorization, and input validation architecture in flAPI.

## Overview

flAPI implements defense-in-depth security:
1. **Authentication** - Verify identity (JWT, Basic, OIDC)
2. **Authorization** - Check permissions (roles)
3. **Input Validation** - Sanitize inputs (validators)
4. **Output Escaping** - Prevent injection (Mustache braces)

## Architecture

```mermaid
graph TB
    subgraph "Request Flow"
        REQ[HTTP Request]
    end

    subgraph "Authentication Layer"
        AM[AuthMiddleware]
        JWT[JWT Validator]
        Basic[Basic Auth]
        OIDC[OIDC Handler]
    end

    subgraph "Authorization Layer"
        RoleCheck[Role Check]
        EndpointAuth[Endpoint Auth Config]
    end

    subgraph "Validation Layer"
        RV[RequestValidator]
        TypeVal[Type Validators]
        PatternVal[Pattern Validators]
    end

    subgraph "Execution Layer"
        STP[SQLTemplateProcessor]
        Escape[Quote Escaping]
    end

    REQ --> AM
    AM --> JWT
    AM --> Basic
    AM --> OIDC
    AM --> RoleCheck
    RoleCheck --> EndpointAuth
    EndpointAuth --> RV
    RV --> TypeVal
    RV --> PatternVal
    RV --> STP
    STP --> Escape
```

## Authentication

### AuthMiddleware (src/auth_middleware.cpp)

Middleware that validates credentials before request processing.

```cpp
class AuthMiddleware {
public:
    struct context {
        bool authenticated = false;
        std::string username;
        std::vector<std::string> roles;
    };

    void before_handle(crow::request& req, crow::response& res, context& ctx);
    void after_handle(crow::request& req, crow::response& res, context& ctx);
};
```

### Authentication Types

#### JWT Authentication

```yaml
auth:
  enabled: true
  type: jwt
  jwt_secret: ${JWT_SECRET}
  jwt_issuer: my-app
```

**Token validation:**
```cpp
bool validateJWT(const std::string& token) {
    auto decoded = jwt::decode(token);
    auto verifier = jwt::verify()
        .allow_algorithm(jwt::algorithm::hs256{jwt_secret_})
        .with_issuer(jwt_issuer_);
    verifier.verify(decoded);
    return true;
}
```

**Expected header:**
```
Authorization: Bearer eyJhbGciOiJIUzI1NiIs...
```

#### Basic Authentication

```yaml
auth:
  enabled: true
  type: basic
  users:
    - username: admin
      password: ${ADMIN_PASSWORD}
      roles: [admin, user]
    - username: reader
      password: ${READER_PASSWORD}
      roles: [user]
```

**Expected header:**
```
Authorization: Basic YWRtaW46cGFzc3dvcmQ=
```

#### OIDC Authentication (src/oidc_auth_handler.cpp)

```yaml
auth:
  enabled: true
  type: oidc
  oidc:
    provider_type: google          # or microsoft, keycloak, generic
    issuer_url: https://accounts.google.com
    client_id: ${GOOGLE_CLIENT_ID}
    allowed_audiences:
      - my-app-client-id
    username_claim: email
    roles_claim: groups
```

**OIDC components:**
- `OIDCDiscoveryClient` - Fetches `.well-known/openid-configuration`
- `OIDCJWKSManager` - Caches and refreshes JWKS keys
- `OIDCProviderPresets` - Pre-configured settings for Google, Microsoft, etc.

### AWS Secrets Manager Integration

```yaml
auth:
  enabled: true
  from_aws_secretmanager:
    secret_name: my-app/api-users
    region: us-east-1
```

Loads user credentials from AWS Secrets Manager at startup.

## Authorization

### Role-Based Access Control

Endpoints can require specific roles:

```yaml
# sqls/admin_report.yaml
url-path: /admin/report
auth:
  required: true
  roles: [admin]       # Only admin role can access
```

**Role checking:**
```cpp
bool checkRoles(const context& ctx, const EndpointConfig& endpoint) {
    if (!endpoint.auth.required) return true;
    if (endpoint.auth.roles.empty()) return ctx.authenticated;

    for (const auto& required_role : endpoint.auth.roles) {
        if (std::find(ctx.roles.begin(), ctx.roles.end(), required_role)
            != ctx.roles.end()) {
            return true;
        }
    }
    return false;
}
```

### Per-Endpoint Auth Override

Each endpoint can override global auth settings:

```yaml
# Public endpoint (no auth)
url-path: /public/status
auth:
  required: false

# Protected endpoint with specific roles
url-path: /admin/users
auth:
  required: true
  roles: [admin]
```

## Input Validation

### RequestValidator (src/request_validator.cpp)

Validates all incoming parameters against configured rules.

```cpp
class RequestValidator {
public:
    ValidationResult validate(const std::map<std::string, std::string>& params,
                             const std::vector<RequestFieldConfig>& fields);

private:
    bool validateInt(const std::string& value, const ValidatorConfig& config);
    bool validateString(const std::string& value, const ValidatorConfig& config);
    bool validateEmail(const std::string& value);
    bool validateUUID(const std::string& value);
    bool validateEnum(const std::string& value, const ValidatorConfig& config);
    bool validateDate(const std::string& value, const ValidatorConfig& config);
    bool validatePattern(const std::string& value, const std::string& pattern);
};
```

### Validator Types

| Type | Options | Example |
|------|---------|---------|
| `int` | `min`, `max` | `min: 1, max: 1000` |
| `string` | `min-length`, `max-length`, `pattern` | `max-length: 200` |
| `email` | - | Validates email format |
| `uuid` | - | Validates UUID format |
| `enum` | `values` | `values: [active, inactive]` |
| `date` | `min`, `max` | `min: 2020-01-01` |
| `time` | `min`, `max` | `min: 09:00:00` |

### Validator Configuration

```yaml
request:
  - field-name: customer_id
    field-in: query
    required: true
    validators:
      - type: int
        min: 1
        max: 999999

  - field-name: email
    field-in: body
    validators:
      - type: email

  - field-name: status
    field-in: query
    validators:
      - type: enum
        values: [active, inactive, pending]

  - field-name: name
    field-in: body
    validators:
      - type: string
        max-length: 200
        pattern: "^[a-zA-Z0-9 ]+$"
```

### SQL Injection Prevention

```yaml
validators:
  - type: string
    preventSqlInjection: true  # Default: true
```

When enabled, rejects inputs containing:
- SQL keywords in dangerous positions
- Comment sequences (`--`, `/*`)
- Common injection patterns

## Output Escaping (Mustache)

### Triple Braces for Strings

Triple braces `{{{ }}}` escape quotes for safe SQL interpolation:

```sql
-- Template
WHERE name = '{{{ params.name }}}'

-- Input: O'Brien
-- Output: WHERE name = 'O''Brien'
```

### Double Braces for Numbers

Double braces `{{ }}` for numeric values (no escaping):

```sql
-- Template
LIMIT {{ params.limit }}

-- Input: 100
-- Output: LIMIT 100
```

### Safe Query Pattern

```sql
SELECT * FROM customers
WHERE 1=1
{{#params.name}}
  AND name = '{{{ params.name }}}'   -- String: escaped
{{/params.name}}
{{#params.id}}
  AND id = {{ params.id }}           -- Number: raw
{{/params.id}}
```

## Security Layers Summary

```
Request arrives
    ↓
[Layer 1: Authentication]
  - Validate JWT/Basic/OIDC token
  - Extract identity and roles
  - Reject if auth fails → 401
    ↓
[Layer 2: Authorization]
  - Check if endpoint requires auth
  - Verify user has required roles
  - Reject if unauthorized → 403
    ↓
[Layer 3: Input Validation]
  - Apply type validators
  - Check patterns and constraints
  - Reject SQL injection attempts
  - Reject if invalid → 400
    ↓
[Layer 4: Template Escaping]
  - Triple braces escape quotes
  - Prevents injection in SQL
    ↓
[Query Execution]
```

## Rate Limiting

`RateLimitMiddleware` (src/rate_limit_middleware.cpp) prevents abuse:

```yaml
rate_limit:
  enabled: true
  max: 100               # Max requests
  interval: 60           # Per minute
```

**Per-endpoint override:**
```yaml
url-path: /expensive-endpoint
rate_limit:
  enabled: true
  max: 10
  interval: 60
```

## MCP Authentication

MCP has separate auth configuration:

```yaml
mcp:
  auth:
    enabled: true
    type: bearer
    jwt_secret: ${MCP_JWT_SECRET}
    methods:
      tools/call:
        required: true
      resources/read:
        required: false  # Allow anonymous reads
```

## Security Best Practices

1. **Always use validators** - Define type and constraints for all inputs
2. **Use triple braces** - For string values in SQL templates
3. **Require authentication** - For sensitive endpoints
4. **Define roles** - Use RBAC for admin/write operations
5. **Rate limit** - Protect against abuse
6. **Whitelist env vars** - Only expose needed environment variables
7. **Use HTTPS** - Enable TLS in production

```yaml
https:
  enabled: true
  ssl_cert_file: /path/to/cert.pem
  ssl_key_file: /path/to/key.pem
```

## Source Files

| File | Purpose |
|------|---------|
| `src/auth_middleware.cpp` | HTTP auth middleware |
| `src/mcp_auth_handler.cpp` | MCP auth handler |
| `src/oidc_auth_handler.cpp` | OIDC token validation |
| `src/oidc_discovery_client.cpp` | OIDC discovery |
| `src/oidc_jwks_manager.cpp` | JWKS key management |
| `src/request_validator.cpp` | Input validation |
| `src/rate_limit_middleware.cpp` | Rate limiting |

## Related Documentation

- [DESIGN_DECISIONS.md](../DESIGN_DECISIONS.md#8-defense-in-depth-security) - Security philosophy
- [../../CONFIG_REFERENCE.md](../../CONFIG_REFERENCE.md) - Auth configuration options
