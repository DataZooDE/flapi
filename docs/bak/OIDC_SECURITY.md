# OIDC Security Best Practices

Security considerations and best practices for using OIDC authentication in flAPI.

## Table of Contents

- [Transport Security](#transport-security)
- [Token Security](#token-security)
- [Key Management](#key-management)
- [Clock Synchronization](#clock-synchronization)
- [Rate Limiting](#rate-limiting)
- [Audit Logging](#audit-logging)
- [Common Attacks and Mitigations](#common-attacks-and-mitigations)
- [Provider-Specific Security](#provider-specific-security)

---

## Transport Security

### HTTPS Required

OIDC tokens **MUST** be transmitted over HTTPS:

```
⚠️  DANGER: HTTP
Authorization: Bearer eyJhbGciOiJSUzI1NiIs...
→ Token exposed in plaintext
→ Anyone on network can see token
→ Token can be intercepted and reused
```

```
✅ CORRECT: HTTPS
Authorization: Bearer eyJhbGciOiJSUzI1NiIs...
→ Token encrypted in transit
→ Only endpoint and client can decrypt
→ Even packet sniffing won't expose token
```

### Configure HTTPS in flAPI

```bash
# Run flAPI with TLS certificate
./flapi --config flapi.yaml \
  --cert /path/to/server.crt \
  --key /path/to/server.key
```

### Certificate Requirements

- **Self-signed**: Acceptable for development/testing
- **Proper certificate**: Required for production
  - Use Let's Encrypt (free) for public servers
  - Enterprise CA certificate for private/internal services

```bash
# Generate self-signed cert (testing only)
openssl req -x509 -newkey rsa:4096 -keyout key.pem -out cert.pem -days 365 -nodes
```

### HSTS Headers

For production, enable HSTS (HTTP Strict Transport Security):

```
Strict-Transport-Security: max-age=31536000; includeSubDomains
```

This prevents downgrade attacks (HTTPS → HTTP).

---

## Token Security

### Never Log Full Tokens

**❌ WRONG - Exposes Token**:

```cpp
CROW_LOG_INFO << "Received token: " << token;
// Logs: Received token: eyJhbGciOiJSUzI1NiIs...complete.token.here...
```

**✅ CORRECT - Safe Logging**:

```cpp
CROW_LOG_INFO << "Received token: " << token.substr(0, 20) << "...";
// Logs: Received token: eyJhbGciOiJSUzI1Ni...
```

### Token Storage

- **Memory**: Store only in memory (not disk/database)
- **Never cache tokens**: Except JWKS (public keys are safe)
- **Clear on logout**: Remove from memory immediately
- **Session storage**: For MCP, bind to session ID (prevents token theft)

```yaml
# For MCP: Token binding prevents hijacking
MCPSession:
  session_id: "session-abc123"
  bound_token_jti: "token-jti-from-oidc"  # Prevent reuse
```

### Token Rotation

For long-lived applications:

```yaml
auth:
  oidc:
    enable-refresh-tokens: true
```

Client logic should:

1. Detect token expiration (5 minutes before `exp`)
2. Request refresh token before expiration
3. Update with new token
4. Discard old token

### Token Lifetime

Recommended durations:

| Type | Lifetime | Use Case |
|------|----------|----------|
| **ID Token** | 5-15 minutes | Short-lived, immediate use |
| **Access Token** | 15-60 minutes | REST API calls |
| **Refresh Token** | 7-30 days | Long-lived sessions |

Configure in provider settings, not flAPI.

---

## Key Management

### JWKS Caching

flAPI automatically caches JWKS (public keys):

```yaml
auth:
  oidc:
    jwks-cache-hours: 24
```

**Security implications**:
- **Caching window**: Until cache expires, old keys usable
- **Key rotation**: Provider must notify if immediate rotation needed
- **Mitigations**:
  - Keep cache window reasonable (1-24 hours)
  - Monitor provider key rotation announcements
  - flAPI auto-refreshes cache when key not found

### Key Rotation Handling

When provider rotates signing keys:

1. New token arrives with unknown `kid` (key ID)
2. flAPI attempts token validation → fails (key not cached)
3. flAPI **automatically** refreshes JWKS cache
4. Retry validation with new keys
5. Token validated successfully

**This is automatic** - no manual intervention needed.

### JWK Validation

Verify JWKS from provider:

```bash
# Check provider's JWKS endpoint
curl https://accounts.google.com/.well-known/openid-configuration | jq '.jwks_uri'

# Fetch JWKS
curl https://www.googleapis.com/oauth2/v3/certs | jq '.keys'

# Each key should have:
# - kid: Key ID
# - kty: Key type (RSA)
# - alg: Algorithm (RS256, RS384, RS512)
# - n, e: RSA modulus and exponent
```

### Public Key Security

JWKS keys are **public** - safe to cache/display.

Only **private** keys are secret:
- Provider has private key
- JWKS contains public keys only
- Public keys cannot decrypt or forge signatures

---

## Clock Synchronization

### Clock Skew Tolerance

Token expiration has built-in tolerance:

```yaml
auth:
  oidc:
    clock-skew-seconds: 300  # 5 minutes
```

**Why needed**:
- Server clocks not perfectly synchronized
- Minor time drifts across systems
- Network latency variations

**Validation logic**:

```
Token exp claim: 1234571490
Current time: 1234571590 (100 seconds later)
Clock skew: 300 seconds
Result: Valid (1234571490 + 300 > 1234571590)
```

### Sync System Clocks

Ensure all systems running flAPI have synchronized time:

```bash
# Linux: Check NTP
timedatectl status

# Sync if needed
sudo ntpdate -s time.nist.gov

# Or use systemd-timesyncd
sudo systemctl start systemd-timesyncd
```

**For production**, always use NTP or similar time synchronization.

### Clock Skew Tuning

- **Too small** (< 60s): Legitimate tokens rejected due to minor drift
- **Too large** (> 600s): Security window increases (10 minutes)
- **Recommended**: 300 seconds (5 minutes) - good balance

If frequent "token expired" errors with valid tokens:
```yaml
auth:
  oidc:
    clock-skew-seconds: 600  # Increase to 10 minutes
```

Then investigate clock sync issues.

---

## Rate Limiting

### OIDC Token Validation

Rate limit OIDC authentication endpoints to prevent brute force:

```yaml
# flapi.yaml
rate-limit:
  enabled: true
  paths:
    /customers:                    # REST endpoint using OIDC
      requests-per-minute: 1000
      burst: 100
    /mcp/jsonrpc:                  # MCP endpoint
      requests-per-minute: 100     # Stricter for auth
      burst: 10
```

### JWKS Refresh

Limit refresh frequency to prevent JWKS endpoint abuse:

```cpp
// Internal: JWKS auto-refresh only when key not found
// Prevents repeated JWKS fetches for same missing key
```

No configuration needed - handled automatically.

### Provider Rate Limits

Check provider's JWKS endpoint rate limits:

| Provider | Rate Limit |
|----------|-----------|
| Google | 1000 req/sec per IP |
| Microsoft | 5000 req/min per app |
| Keycloak | Depends on config |
| Auth0 | 10000 req/minute per tenant |
| Okta | 5000 req/minute per client |

JWKS is cached locally, so rate limits rarely reached unless:
- Very frequent key rotations
- JWKS cache disabled
- Invalid `issuer-url` causing repeated 404s

---

## Audit Logging

### Log Authentication Events

Enable debug logging for security events:

```bash
./flapi --config flapi.yaml --log-level debug
```

Look for messages:
- `OIDC token validation` - Auth attempt started
- `OIDC token validation failed` - Auth failure
- `Verifying signature` - Signature check in progress
- `Invalid issuer` - Wrong provider
- `Token expired` - Expiration detected
- `Invalid audience` - Audience mismatch

### What to Log

For audit trail, log:
- **Success**: Username, timestamp, endpoint, roles
- **Failure**: Reason (expired, invalid sig, wrong aud), token truncated, IP
- **Key rotation**: When JWKS refreshed, old/new key count
- **Errors**: Claims extraction failures, provider unavailability

### Security Monitoring

Monitor logs for:

```bash
# Failed validations (possible attacks)
grep "token validation failed" flapi.log | wc -l

# Expired tokens (clock sync issues)
grep "Token expired" flapi.log | wc -l

# Key rotation activity
grep "refreshed JWKS" flapi.log | tail -20

# Invalid audience (wrong endpoint?)
grep "Invalid audience" flapi.log
```

### Log Retention

- **Development**: Minimal retention (1-7 days)
- **Production**: 30-90 days (compliance)
- **PII**: Truncate PII in logs (emails, names)

---

## Common Attacks and Mitigations

### 1. Token Theft (Man-in-the-Middle)

**Attack**: Attacker intercepts token in transit

**Impact**: Attacker can impersonate user

**Mitigation**:
- ✅ **HTTPS required**: All tokens encrypted in transit
- ✅ **Token truncation in logs**: Can't see full token in logs
- ✅ **Short token lifetime**: 15-60 minutes reduces window
- ✅ **Secure transport**: Use TLS 1.3+

```yaml
auth:
  oidc:
    # Keep defaults (https + short-lived tokens from provider)
```

### 2. Token Replay

**Attack**: Attacker replays captured token

**Impact**: Unauthorized access after token expires

**Mitigation**:
- ✅ **Expiration validation**: Tokens rejected after `exp`
- ✅ **JTI (JWT ID) binding** (MCP): Token bound to session, can't be reused
- ✅ **HTTPS only**: Prevents token capture in first place
- ✅ **Audience validation**: Token usable only for intended API

```yaml
auth:
  oidc:
    allowed-audiences: [my-api]  # Token only valid for this API
```

### 3. Token Forging

**Attack**: Attacker creates fake JWT token

**Impact**: Attacker gains access as any user

**Mitigation**:
- ✅ **RSA signature verification**: Token must be signed by provider
- ✅ **Key validation**: Public key fetched from provider's JWKS
- ✅ **Issuer validation**: Token must be from trusted provider
- ✅ **Algorithm validation**: Verify RS256/RS384/RS512 (never HS256 with public key)

```yaml
auth:
  oidc:
    issuer-url: https://accounts.google.com  # Validate issuer
    provider-type: google  # Verify provider type
```

Forged tokens **cannot** pass signature verification - cryptographically impossible.

### 4. Clock Skew Exploitation

**Attack**: Attacker uses expired token if clock skew too large

**Impact**: Extended window for using captured/stolen tokens

**Mitigation**:
- ✅ **Reasonable clock skew**: 5 minutes (default)
- ✅ **Time synchronization**: Sync system clocks with NTP
- ✅ **Monitor clock drift**: Alert if NTP out of sync

```yaml
auth:
  oidc:
    clock-skew-seconds: 300  # 5 minutes - don't increase without reason
```

### 5. Key Confusion Attack

**Attack**: Attacker uses HMAC (HS256) with public key as secret

**Impact**: Attacker can forge tokens (if algorithm allows)

**Mitigation**:
- ✅ **Algorithm enforcement**: Only RS256/RS384/RS512 (asymmetric)
- ✅ **Never use HS256 with OIDC**: Not safe for asymmetric keys

flAPI only supports RS256/RS384/RS512 - **immune to this attack**.

### 6. JWKS Poisoning

**Attack**: Attacker injects fake keys into JWKS response

**Impact**: Attacker can forge tokens with fake key

**Mitigation**:
- ✅ **HTTPS only**: JWKS fetched over encrypted connection
- ✅ **Issuer verification**: JWKS URL must come from trusted issuer
- ✅ **Provider validation**: Verify JWKS endpoint before use
- ✅ **JWKS caching**: Repeated requests use cached copy, detect changes

```bash
# Verify JWKS endpoint is legitimate
curl https://accounts.google.com/.well-known/openid-configuration | jq '.jwks_uri'
# Should be: https://www.googleapis.com/oauth2/v3/certs
```

### 7. Audience Confusion

**Attack**: Attacker uses token meant for service A on service B

**Impact**: Unauthorized access to different API

**Mitigation**:
- ✅ **Audience validation**: Each API validates `aud` claim
- ✅ **Unique audiences**: Each service has unique audience value
- ✅ **Check allowed-audiences**: Configured in flAPI

```yaml
# Service A
auth:
  oidc:
    allowed-audiences: [service-a]

# Service B
auth:
  oidc:
    allowed-audiences: [service-b]

# Token for A cannot be used for B
```

### 8. Role Elevation

**Attack**: Attacker modifies token claims to add admin role

**Impact**: Unauthorized admin access

**Mitigation**:
- ✅ **Signature verification**: Modified token fails signature check
- ✅ **Read-only claims**: Claims come from provider, client can't modify
- ✅ **Provider role assignment**: Only provider can add roles

Token signed by provider cannot be modified without provider's private key.

---

## Provider-Specific Security

### Google

✅ **Strengths**:
- Industry-standard OIDC
- Frequent key rotation
- Email verification built-in
- Strong encryption

⚠️ **Considerations**:
- Verify `email_verified` claim if sensitive operations

```yaml
auth:
  oidc:
    provider-type: google
    allowed-audiences: [YOUR_CLIENT_ID.apps.googleusercontent.com]
```

### Microsoft Azure AD

✅ **Strengths**:
- Enterprise security features
- App role RBAC support
- Conditional access policies
- MFA integration

⚠️ **Considerations**:
- Complex tenant/app registration setup
- Requires proper audience configuration

```yaml
auth:
  oidc:
    provider-type: microsoft
    issuer-url: https://login.microsoftonline.com/{tenant}/v2.0
    allowed-audiences: [api://APPLICATION_ID]
```

### Keycloak (Self-Hosted)

✅ **Strengths**:
- Full control over cryptography
- Custom claim support
- Self-hosted (private)

⚠️ **Risks**:
- You manage security/updates
- Private key security is your responsibility
- Network access controls important

```yaml
auth:
  oidc:
    provider-type: keycloak
    issuer-url: https://keycloak.private.company.com/realms/prod
    # Ensure HTTPS in production
```

### Auth0

✅ **Strengths**:
- Managed security (Auth0 handles key rotation)
- Custom claims support
- Log aggregation
- Web hooks for monitoring

⚠️ **Considerations**:
- Managed service (vendor dependent)
- Requires internet access to Auth0

```yaml
auth:
  oidc:
    provider-type: auth0
    issuer-url: https://your-domain.auth0.com
```

### Okta

✅ **Strengths**:
- Enterprise OIDC/OAuth
- Advanced MFA support
- Audit logging
- Risk-based access

⚠️ **Considerations**:
- Primarily enterprise pricing
- Complex authorization servers

```yaml
auth:
  oidc:
    provider-type: okta
    issuer-url: https://your-domain.okta.com/oauth2/default
```

### GitHub

⚠️ **Note**: OAuth 2.0, not full OIDC
- No standard OIDC discovery
- Custom claim extraction
- Good for GitHub users only

```yaml
auth:
  oidc:
    provider-type: github
    client-id: YOUR_GITHUB_APP_ID
```

---

## Security Checklist

Before deploying OIDC authentication:

### Configuration
- [ ] HTTPS enabled with valid certificate
- [ ] `issuer-url` verified (matches provider's official URL)
- [ ] `client-id` and `client-secret` obtained from provider
- [ ] `allowed-audiences` configured (matches provider's audience)
- [ ] `verify-expiration: true` (default)
- [ ] `clock-skew-seconds` is reasonable (300 default)

### Transport
- [ ] HTTPS only (no HTTP fallback)
- [ ] TLS 1.2+ (1.3 recommended)
- [ ] Valid certificate chain
- [ ] HSTS headers enabled (production)

### Token Handling
- [ ] Full tokens never logged
- [ ] Tokens stored in memory only
- [ ] No token caching (except JWKS)
- [ ] Token truncation in error messages
- [ ] Refresh tokens enabled for long sessions

### Rate Limiting
- [ ] Authentication endpoints rate-limited
- [ ] JWKS endpoint rate limits checked
- [ ] Brute force protection configured

### Monitoring
- [ ] Debug logging enabled
- [ ] Auth failures logged
- [ ] Key rotation monitored
- [ ] Clock synchronization verified
- [ ] Provider endpoint health monitored

### Testing
- [ ] Expired tokens rejected
- [ ] Invalid signatures rejected
- [ ] Wrong issuer rejected
- [ ] Wrong audience rejected
- [ ] Modified tokens rejected
- [ ] Valid tokens accepted

---

## Resources

- [OWASP JWT Cheat Sheet](https://cheatsheetseries.owasp.org/cheatsheets/JSON_Web_Token_for_Java_Cheat_Sheet.html)
- [NIST OAuth 2.0 Security Best Practices](https://tools.ietf.org/html/draft-ietf-oauth-security-topics)
- [OpenID Connect Security Concerns](https://openid.net/specs/openid-connect-core-1_0.html#Security)
- [JWT Best Practices](https://tools.ietf.org/html/rfc8725)
