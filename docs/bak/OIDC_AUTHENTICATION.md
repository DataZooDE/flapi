# OIDC/OAuth 2.0 Authentication for flAPI

This guide covers OpenID Connect (OIDC) and OAuth 2.0 authentication in flAPI, enabling token validation from external identity providers and service-to-service authentication.

## Table of Contents

- [Overview](#overview)
- [Supported Flows](#supported-flows)
- [OIDC Providers](#oidc-providers)
- [Configuration](#configuration)
- [REST Authentication](#rest-authentication)
- [MCP Authentication](#mcp-authentication)
- [Token Validation Flow](#token-validation-flow)
- [Client Credentials Flow](#client-credentials-flow)
- [Token Refresh](#token-refresh)
- [Security Best Practices](#security-best-practices)
- [Troubleshooting](#troubleshooting)

## Overview

OIDC is an authentication layer on top of OAuth 2.0 that enables flAPI to validate tokens issued by external identity providers. Unlike Basic or JWT Bearer authentication, OIDC delegates identity management to providers like Google, Microsoft Azure AD, Keycloak, Auth0, Okta, or GitHub.

### Key Benefits

- **No password storage**: Tokens come from external providers
- **Multiple providers**: Support for Google, Microsoft, Keycloak, Auth0, Okta, GitHub, and any OIDC-compliant provider
- **Token validation**: RSA signature verification using public keys (JWKS)
- **Claims extraction**: Automatic extraction of username, email, roles, and groups
- **Long-lived sessions**: Optional refresh token support for extended sessions
- **Service accounts**: Client Credentials Flow for machine-to-machine authentication

### Authentication Types

flAPI supports three authentication types:

1. **Basic**: Username/password authentication (MD5 hashed passwords)
2. **Bearer**: JSON Web Token (JWT) with symmetric key signature verification
3. **OIDC**: External provider tokens with RSA signature verification (NEW)

## Supported Flows

### 1. Token Validation Flow (Primary)

Validates tokens issued by external OIDC providers:

```
External Provider (Google, Keycloak, etc.)
  ↓
Issues JWT token to client
  ↓
Client includes token in Authorization header
  ↓
flAPI validates signature using provider's JWKS
  ↓
Extracts claims (username, roles, groups)
  ↓
Allows/denies access based on roles
```

**Use Case**: User authentication with external identity provider

### 2. Client Credentials Flow

Service-to-service authentication without user context:

```
Service Account (machine)
  ↓
Requests token from provider with client credentials
  ↓
Provider issues access token (no user involved)
  ↓
Service includes token in API requests
  ↓
flAPI validates token and allows access
```

**Use Case**: Integration between two services, scheduled jobs, APIs

**Note**: MCP does not support OAuth Authorization Code Flow (browser redirects). Only Token Validation and Client Credentials flows are supported for MCP.

## OIDC Providers

### Supported Providers

| Provider | Type | Auto-Config | Setup Complexity |
|----------|------|-------------|-----------------|
| **Google Workspace** | OIDC | Yes | Low |
| **Microsoft Azure AD** | OIDC | Yes | Medium |
| **Keycloak** | OIDC | Yes | Medium |
| **Auth0** | OIDC | Yes | Low |
| **Okta** | OIDC | Yes | Medium |
| **GitHub** | OAuth 2.0 | Yes | Low |
| **Generic OIDC** | Any Provider | No | High |

### Auto-Configuration

When you specify a `provider-type`, flAPI automatically configures:

- Issuer URL (with placeholder substitution)
- Default scopes
- Claim mappings (username, email, roles)
- Provider-specific settings

**Example**: `provider-type: google` automatically sets:
- `issuer-url: https://accounts.google.com`
- `username-claim: email`
- `scopes: [openid, profile, email]`

## Configuration

### Global Configuration (flapi.yaml)

Configure OIDC for REST endpoints or default setting:

```yaml
# flapi.yaml
auth:
  type: oidc
  oidc:
    provider-type: google
    client-id: your-client-id.apps.googleusercontent.com
    allowed-audiences:
      - your-client-id.apps.googleusercontent.com
    verify-expiration: true
    clock-skew-seconds: 300
```

### Endpoint-Level Configuration

Override global auth for specific endpoints:

```yaml
# sqls/customers.yaml
url-path: /customers
method: GET

auth:
  type: oidc
  oidc:
    provider-type: keycloak
    issuer-url: https://keycloak.company.com/realms/prod
    client-id: flapi-client
    allowed-audiences:
      - flapi-client
    role-claim-path: realm_access.roles
    username-claim: preferred_username
```

### MCP Configuration

Configure OIDC for MCP server authentication:

```yaml
# flapi.yaml
mcp:
  auth:
    type: oidc
    oidc:
      provider-type: auth0
      issuer-url: https://your-domain.auth0.com
      client-id: flapi-mcp-client
      allowed-audiences:
        - flapi-mcp-client
```

### Configuration Reference

#### Basic OIDC Settings

| Field | Type | Required | Default | Description |
|-------|------|----------|---------|-------------|
| `provider-type` | string | No | "generic" | Provider: google, microsoft, keycloak, auth0, okta, github, generic |
| `issuer-url` | string | Required* | - | OIDC issuer URL (discovery endpoint base) |
| `client-id` | string | Required | - | OAuth 2.0 client ID |
| `client-secret` | string | No | - | OAuth 2.0 client secret (from env: `${VAR_NAME}`) |
| `allowed-audiences` | array | No | [] | Expected values in `aud` claim |

*Required only for generic provider type; auto-configured for known providers.

#### Token Validation Settings

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `verify-expiration` | boolean | true | Check token expiration (`exp` claim) |
| `clock-skew-seconds` | integer | 300 | Time tolerance for token expiration (5 minutes) |

#### Claim Mapping

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `username-claim` | string | "sub" | Claim for username (email, preferred_username, etc.) |
| `email-claim` | string | "email" | Claim for user email |
| `roles-claim` | string | "roles" | Claim for user roles array |
| `groups-claim` | string | "groups" | Claim for user groups array |
| `role-claim-path` | string | "" | Nested claim path: "realm_access.roles" (Keycloak) |

#### OAuth 2.0 Flows

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `enable-client-credentials` | boolean | false | Enable client credentials flow |
| `enable-refresh-tokens` | boolean | false | Enable refresh token handling |
| `scopes` | array | [] | OAuth scopes to request |

#### JWKS Caching

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `jwks-cache-hours` | integer | 24 | How long to cache JWKS (1-24 hours) |

## REST Authentication

### Setting Up OIDC for REST

1. **Configure endpoint or global auth**:

```yaml
# Option 1: Global default
auth:
  type: oidc
  oidc:
    provider-type: google
    client-id: my-app
```

```yaml
# Option 2: Endpoint-specific
auth:
  type: oidc
  oidc:
    provider-type: keycloak
    issuer-url: https://keycloak.example.com/realms/prod
    client-id: my-app
```

2. **Client requests with token**:

```bash
curl -H "Authorization: Bearer <token>" http://localhost:8080/customers
```

3. **flAPI validates token**:
   - Extracts token from Authorization header
   - Decodes JWT header to get key ID (`kid`)
   - Fetches public key from provider's JWKS endpoint
   - Verifies RSA signature (RS256/RS384/RS512)
   - Validates claims: issuer, audience, expiration
   - Allows or denies based on roles/groups

### Token Format

OIDC tokens are JWT format: `header.payload.signature`

Example with decoded payload:

```json
{
  "sub": "user@example.com",
  "iss": "https://accounts.google.com",
  "aud": "my-app",
  "email": "user@example.com",
  "email_verified": true,
  "name": "John Doe",
  "iat": 1234567890,
  "exp": 1234571490
}
```

### Accessing Auth Context in Templates

Auth context available in SQL template variables:

```sql
SELECT * FROM users
WHERE username = '{{{ auth.username }}}'
  AND created_by = '{{{ auth.username }}}'
```

Template context variables:

- `auth.username` - Authenticated username
- `auth.roles` - Array of user roles
- `auth.authenticated` - Boolean (true if authenticated)

## MCP Authentication

### Setting Up OIDC for MCP

1. **Configure MCP server auth**:

```yaml
# flapi.yaml
mcp:
  auth:
    type: oidc
    oidc:
      provider-type: auth0
      issuer-url: https://your-domain.auth0.com
      client-id: flapi-mcp
      allowed-audiences:
        - flapi-mcp
```

2. **Client sends token in initialize request**:

```json
{
  "jsonrpc": "2.0",
  "method": "initialize",
  "params": {
    "protocolVersion": "2024-11-05",
    "capabilities": {},
    "clientInfo": {
      "name": "my-client",
      "version": "1.0.0"
    }
  },
  "headers": {
    "Authorization": "Bearer <token>"
  }
}
```

3. **flAPI validates token and creates session**:
   - Validates OIDC token
   - Binds token JTI (unique ID) to session (prevents hijacking)
   - Tracks token expiration
   - Returns session ID in response header: `Mcp-Session-Id`
   - Subsequent requests use session ID (token not needed again)

4. **Client continues with session**:

```json
{
  "jsonrpc": "2.0",
  "method": "list_tools",
  "params": {},
  "headers": {
    "Mcp-Session-Id": "session-id-from-initialize"
  }
}
```

### Session Binding

For security, OIDC tokens are bound to MCP sessions via JWT ID (`jti` claim):

- `bound_token_jti`: Token's unique ID bound to session
- `token_expires_at`: When token expires
- `needsTokenRefresh()`: Check if token expiring soon (5 min window)

This prevents attackers from hijacking tokens.

## Token Validation Flow

### Detailed Validation Pipeline

```
OIDC Token Request
  ↓
[1] Extract "Bearer <token>" from Authorization header
  ↓
[2] Decode JWT without verification to extract:
    - Header: algorithm (alg), key ID (kid)
    - Payload: claims (sub, aud, exp, etc.)
  ↓
[3] Get public key from JWKS cache
    - Cache key: issuer_url + "/.well-known/openid-configuration"
    - Get jwks_uri from discovery endpoint
    - Fetch JWKS and cache (1-24 hours)
    - Look up key by key ID (kid)
    - If not found: refresh JWKS and retry
  ↓
[4] Verify RSA signature
    - Algorithm: RS256, RS384, or RS512
    - Input: header.payload (original bytes)
    - Signature: base64url decoded
    - Public key: from JWKS
    - Result: valid or tampered
  ↓
[5] Validate standard claims
    - issuer (iss): matches configured issuer_url
    - audience (aud): contains one of allowed_audiences
    - expiration (exp): not exceeded (with clock_skew_seconds tolerance)
    - not before (nbf): token not yet valid
    - issued at (iat): token issued recently
  ↓
[6] Extract claims for auth context
    - username: from configurable username_claim
    - email: from email_claim
    - roles: from roles_claim or role_claim_path (nested)
    - groups: from groups_claim
  ↓
[7] Create auth context
    - authenticated: true
    - username, email, roles, groups
    - auth_type: "oidc"
    - (MCP) bound_token_jti, token_expires_at
  ↓
Allow/Deny based on:
  - Required roles (if configured in endpoint auth)
  - Other business logic
```

### Clock Skew Tolerance

Default 5-minute tolerance for token expiration:

- Token `exp: 1234571490`
- Current time: `1234571200` (5 minutes later)
- Clock skew: 300 seconds (5 minutes)
- Result: Token still valid (exp + skew > now)

Configure in YAML: `clock-skew-seconds: 300`

### Key Rotation

Providers rotate signing keys automatically. flAPI handles this:

1. Token arrives with key ID (`kid`) not in cache
2. JWKS cache automatically refreshes from provider
3. New key is fetched and used for verification
4. Subsequent tokens use cached keys

## Client Credentials Flow

### OAuth 2.0 Service-to-Service Authentication

Exchange client credentials for access token:

```
Service Account
  ↓
POST /token endpoint with:
  - grant_type: client_credentials
  - client_id: xxx
  - client_secret: yyy
  - scope: optional
  ↓
Provider issues access_token
  ↓
Service includes token in Authorization header
  ↓
flAPI validates token as usual
```

### Configuration

Enable and configure client credentials:

```yaml
auth:
  type: oidc
  oidc:
    provider-type: auth0
    issuer-url: https://your-domain.auth0.com
    client-id: service-account
    client-secret: ${SERVICE_CLIENT_SECRET}
    enable-client-credentials: true
    scopes:
      - api://flapi
```

### Obtaining Token Programmatically

Currently in development (Phase 2 implementation):

```cpp
// C++ API (when HTTP client is integrated)
OIDCAuthHandler handler(config);
auto token_response = handler.getClientCredentialsToken({"api://flapi"});
if (token_response) {
  std::string access_token = token_response->access_token;
  // Use token in API requests
}
```

### Token Response Format

```json
{
  "access_token": "eyJhbGciOiJSUzI1NiIs...",
  "token_type": "Bearer",
  "expires_in": 3600,
  "scope": "api://flapi"
}
```

## Token Refresh

### Refresh Token Support

Enable refresh tokens for long-lived sessions:

```yaml
auth:
  type: oidc
  oidc:
    provider-type: keycloak
    issuer-url: https://keycloak.example.com/realms/prod
    client-id: flapi
    enable-refresh-tokens: true
```

### MCP Session Refresh

For MCP, token expiration is tracked per session:

```cpp
if (session->auth_context->needsTokenRefresh()) {
  // Refresh token when within 5 minutes of expiration
  auto new_token = oidc_handler->refreshAccessToken(
    session->auth_context->refresh_token
  );
  // Update session with new token
}
```

**Note**: Client application responsible for detecting token expiration and requesting refresh.

## Security Best Practices

### 1. HTTPS Required

OIDC authentication requires HTTPS:

```bash
# ✅ Correct: HTTPS
Authorization: Bearer eyJhbGc...
→ Encrypted connection to flAPI server

# ❌ Wrong: HTTP
Authorization: Bearer eyJhbGc...
→ Token exposed in plaintext
```

Configure with valid SSL certificate:

```bash
./flapi --config flapi.yaml --cert server.crt --key server.key
```

### 2. Token Storage

Never log or store full tokens:

```cpp
// ✅ Correct: Truncate in logs
CROW_LOG_DEBUG << "Token: " << token.substr(0, 10) << "...";

// ❌ Wrong: Full token in logs
CROW_LOG_DEBUG << "Token: " << token;
```

### 3. Issuer and Audience Validation

Always validate issuer and audience:

```yaml
auth:
  type: oidc
  oidc:
    issuer-url: https://auth.example.com
    allowed-audiences:
      - my-app
```

This prevents:
- Token reuse across different APIs
- Tokens from wrong providers
- Token substitution attacks

### 4. Token Expiration

Always verify expiration (enabled by default):

```yaml
auth:
  type: oidc
  oidc:
    verify-expiration: true
    clock-skew-seconds: 300
```

Recommended token lifetimes:
- Short-lived: 15-60 minutes
- Long-lived (with refresh): 24 hours
- Refresh tokens: 7-30 days

### 5. Role-Based Access Control

Use roles to control access:

```yaml
auth:
  type: oidc
  oidc:
    roles-claim: roles
    # Endpoint can check: user has 'admin' role?
```

### 6. JWKS Caching

Cache JWKS for performance, but refresh periodically:

```yaml
auth:
  type: oidc
  oidc:
    jwks-cache-hours: 24
```

flAPI automatically refreshes when key not found.

### 7. Provider Authentication

Verify provider configuration:

```bash
# Check issuer endpoint responds
curl https://accounts.google.com/.well-known/openid-configuration | jq

# Verify JWKS available
curl https://www.googleapis.com/oauth2/v3/certs | jq
```

### 8. Rate Limiting

Apply rate limiting to auth endpoints:

```yaml
rate-limit:
  enabled: true
  paths:
    /mcp/jsonrpc:
      requests-per-minute: 100
    /customers:
      requests-per-minute: 1000
```

## Troubleshooting

### Common Issues

#### 1. "Invalid token format"

**Symptom**: `Error: Invalid token format - not a valid JWT`

**Causes**:
- Token not in JWT format
- Missing Bearer prefix
- Token corrupted/truncated

**Solution**:
```bash
# Verify token is valid JWT (3 parts separated by dots)
echo $TOKEN | awk -F. '{print NF}'  # Should print 3

# Check Authorization header format
curl -H "Authorization: Bearer <token>" ...
```

#### 2. "Key not found"

**Symptom**: `Error: Key not found (kid: xyz)`

**Causes**:
- Provider rotated keys
- JWKS cache expired
- Key ID mismatch

**Solution**:
- flAPI automatically refreshes JWKS when key not found
- Check log level set to DEBUG to see refresh attempts
- Verify provider endpoint accessible: `curl <issuer-url>/.well-known/openid-configuration`

#### 3. "Signature verification failed"

**Symptom**: `Error: Signature verification failed`

**Causes**:
- Token tampered with
- Token from different provider
- Wrong issuer URL configured
- Clock skew too large

**Solution**:
```yaml
auth:
  type: oidc
  oidc:
    issuer-url: https://accounts.google.com  # Verify exact URL
    clock-skew-seconds: 300  # Increase if time drift
```

#### 4. "Token expired"

**Symptom**: `Error: Token expired`

**Causes**:
- Token lifetime exceeded
- System clock too far ahead
- Clock skew too small

**Solution**:
- Get fresh token from provider
- Increase clock-skew-seconds if time sync issues
- Check system clock: `date` command

#### 5. "Invalid audience"

**Symptom**: `Error: Invalid audience in token`

**Causes**:
- `aud` claim not in configured `allowed-audiences`
- Provider uses different audience value
- Case mismatch

**Solution**:
```bash
# Decode and check audience claim
echo "<token>" | jq -R 'split(".") | .[1] | @base64d | fromjson | .aud'

# Update configuration
auth:
  oidc:
    allowed-audiences:
      - correct-audience-value
```

#### 6. "OIDC configuration error"

**Symptom**: `Error: OIDC configuration error: ...`

**Causes**:
- Missing required configuration fields
- Placeholder not substituted in issuer URL
- Provider-type with missing required parameters

**Solution**:
```yaml
# For keycloak: must include {realm} placeholder
auth:
  oidc:
    provider-type: keycloak
    issuer-url: https://keycloak.example.com/realms/{realm}
    # OR substitute at config time

# For microsoft: must include {tenant} placeholder
auth:
  oidc:
    provider-type: microsoft
    issuer-url: https://login.microsoftonline.com/{tenant}/v2.0
```

### Debug Logging

Enable debug logging to see detailed token validation steps:

```bash
# Run with debug logging
./build/debug/flapi --config flapi.yaml --log-level debug

# Look for messages:
# - "Applying OIDC preset for provider: ..."
# - "OIDC token validation"
# - "Verifying signature..."
# - "Validating claims..."
```

### Manual Token Validation

Decode and inspect tokens without validating:

```bash
# Decode JWT header
TOKEN="<your-token>"
echo $TOKEN | cut -d. -f1 | base64 -d | jq

# Decode JWT payload
echo $TOKEN | cut -d. -f2 | base64 -d | jq

# Check provider JWKS
curl "<issuer-url>/.well-known/openid-configuration" | jq '.jwks_uri'
curl "<jwks-uri>" | jq '.keys[]'
```

### Provider Health Checks

Verify provider endpoints are accessible:

```bash
# Check discovery endpoint
curl https://accounts.google.com/.well-known/openid-configuration
# Should return JSON with issuer, jwks_uri, authorization_endpoint, token_endpoint

# Check JWKS endpoint
curl https://www.googleapis.com/oauth2/v3/certs
# Should return JSON with "keys" array

# Check token endpoint (if testing client credentials)
curl https://oauth2.googleapis.com/token \
  -X POST \
  -d "grant_type=client_credentials&client_id=xxx&client_secret=yyy"
```

## See Also

- [OIDC Provider Setup Guides](OIDC_PROVIDERS.md) - Provider-specific configurations
- [OIDC Security Guide](OIDC_SECURITY.md) - Security considerations and best practices
