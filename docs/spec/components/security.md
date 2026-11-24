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

#### Bearer / JWT Authentication

`type: jwt` is an alias of `type: bearer` (normalised at load, `ConfigManager::normalizeAuthType`).

```yaml
auth:
  enabled: true
  type: bearer            # or jwt
  jwt-secret: '{{env.JWT_SECRET}}'
  jwt-issuer: my-app
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
      password: '{{env.ADMIN_PASSWORD}}'
      roles: [admin, user]
    - username: reader
      password: '{{env.READER_PASSWORD}}'
      roles: [user]
```

**Expected header:**
```
Authorization: Basic YWRtaW46cGFzc3dvcmQ=
```

**Stored passwords** (`src/password_hasher.cpp`): `$pbkdf2-sha256$…` MCF strings are verified
(passlib's ab64 alphabet as well as standard and URL-safe base64). bcrypt hashes are recognised and
**always refused** — the verifier never fails open on a format it cannot check — and the startup
auditor (`src/security_auditor.cpp`) reports them as `AUTH_BCRYPT_UNSUPPORTED`. MD5 hex and
plaintext still verify but are reported. The auditor's advice for all of them is
`pbkdf2_sha256.using(rounds=600000).hash(...)` from passlib.

**Verification cache** (`PasswordVerificationCache`, `src/password_hasher.cpp`): Basic auth
(REST and `mcp.auth`) goes through `AuthMiddleware::verifyCredentials`, which remembers a
*successful* PBKDF2 check for 5 minutes. The key is HMAC-SHA256 under a per-process random key
over user, password and stored hash (length-prefixed), so no password is kept and a changed hash
never matches; failures are never cached; at most 10 000 entries (expired ones swept first, then
arbitrary eviction); every parse of an auth block (load, reload, config-service update) clears
it. Derivations are capped at half the cores; a caller that cannot get a slot within 2 s is
refused (fail closed). Before this every request paid ~165 ms of CPU, wrong passwords included.

An enabled auth block with an unknown `type` is a load/reload error (`AUTH_UNKNOWN_TYPE`), not
an endpoint that refuses everyone.

#### OIDC Authentication (src/oidc_auth_handler.cpp)

```yaml
auth:
  enabled: true
  type: oidc
  oidc:
    provider-type: google          # or microsoft (azure, azure-ad, entra), keycloak, auth0, okta, github, generic
    issuer-url: https://accounts.google.com
    client-id: '{{env.GOOGLE_CLIENT_ID}}'
    allowed-audiences:
      - my-app-client-id
    username-claim: email
    roles-claim: groups
```

Presets with a templated issuer take `tenant-id` (alias `tenant`), `realm` and `domain`, which replace
`{tenant}`, `{realm}` and `{domain}` in `issuer-url` (`OIDCProviderPresets::substitutePlaceholders`).
A placeholder still present after substitution is a startup error naming the key to set.

**OIDC components:**
- `OIDCDiscoveryClient` - Fetches `.well-known/openid-configuration`
- `OIDCJWKSManager` - Caches and refreshes JWKS keys
- `OIDCProviderPresets` - Pre-configured settings for Google, Microsoft, etc.

### AWS Secrets Manager Integration

```yaml
auth:
  enabled: true
  from-aws-secretmanager:
    secret-name: my-app/api-users
    region: us-east-1
```

Loads Basic-auth users (`{"auth":[{username, password, roles}]}`) from AWS Secrets Manager at startup
(`AuthMiddleware::initializeAwsSecretsManager` -> `AwsHelper::loadUsers`, `src/auth_middleware.cpp`).

- **Where the users live.** Parsed once into `AwsHelper`'s map, keyed by `secret-name`, behind a mutex. They
  used to be a table `auth_<name>` in the main DuckDB, readable by every SQL template and listed by the
  schema API (#254). A secret shared by several endpoints is fetched once; there is no refresh until restart.
- **Credentials.** `secret-id`/`secret-key` become an `Aws::Auth::SimpleAWSCredentialsProvider`; with neither,
  `DefaultAWSCredentialsProviderChain`. No DuckDB secret and no SQL are involved, so nothing has to be quoted.
  A legacy `init:` runs in `OpenPrivateDuckDb` (`src/vfs_adapter.cpp`: an in-memory instance of its own,
  `allow_persistent_secrets=false`, same extension directory and offline rules as the main one), which is
  closed once the secret is read; only if it creates no matching S3 secret is the main database's secret of
  that name used, with a deprecation warning.
- **Default credential chain.** It reads `~/.aws/config` and `~/.aws/credentials` (`AWS_CONFIG_FILE`,
  `AWS_SHARED_CREDENTIALS_FILE`, `AWS_PROFILE`) at startup, and a profile's `credential_process` runs that
  command as the flAPI user. Whoever can write those files chooses the identity that fetches the user list.
- **Endpoint and region.** `AwsHelper::resolveEndpointOverride` (`AWS_ENDPOINT_URL_SECRETS_MANAGER` only - not the
  generic `AWS_ENDPOINT_URL`, often MinIO - unless `AWS_IGNORE_CONFIGURED_ENDPOINT_URLS`);
  `AwsHelper::checkEndpointOverride` refuses plain http to a non-loopback host unless
  `FLAPI_ALLOW_INSECURE_SECRETS_ENDPOINT` is set, and an active override is a startup warning: that endpoint
  decides who can log in. `AwsHelper::resolveRegion`: config, `AWS_REGION`, `AWS_DEFAULT_REGION`, the profile's
  region. The client does not probe instance metadata for a region.
- **Only basic, and consistent.** Only enabled `type: basic` blocks fetch. Blocks sharing a `secret-name` must
  agree on region, credential source and `init`, or startup stops naming both endpoints.
- **Upgrade.** `AuthMiddleware::dropLegacyUserTables` drops `auth_<name>` and a configured `secret-table` from the
  main database at startup (a file `db_path` kept them), with a warning.
- **Timing.** An unknown username is verified against `PasswordHasher::dummyHashLike` of the stored passwords
  (same format, same PBKDF2 iterations), inline users included; plaintext and MD5 compare SHA-256 digests with
  `CRYPTO_memcmp`.
- **Logging.** Neither the credentials, the init SQL nor the secret document is logged; the parser logs the
  credential *source* only. `DatabaseManager::executeInitStatement` (connection `init:`, DuckLake `ATTACH`) and
  the private-instance runner report the failing statement's position and `redactSqlErrorMessage(message,
  statement)` (`src/redaction.cpp`): DuckDB's `LINE n:` excerpt dropped; every literal, quoted identifier and
  dollar-quoted body of the statement replaced wherever the message repeats it; the token after "at or near"
  masked; then connection-string secrets and URL credentials. AWS error messages: first line, 300 characters,
  key ids masked (`AwsHelper::sanitiseAwsErrorMessage`). The config APIs show a connection `init` through
  `redactSqlLiterals`.
- `Aws::InitAPI` runs once per process and is never shut down.

**Persistent secrets.** The main DuckDB loads `~/.duckdb/stored_secrets` of the user flAPI runs as, and
templates can list those secrets (redacted) and use them. `duckdb: {allow_persistent_secrets: false}` turns that
off.

## Authorization

### Role-Based Access Control

Endpoints can require specific roles:

```yaml
# sqls/admin_report.yaml
url-path: /admin/report
auth:
  enabled: true
  type: basic
  roles: [admin]       # Only admin role can access
  users: [...]
```

**Role checking** happens in `AuthMiddleware::before_handle`, right after a successful authentication
(`AuthMiddleware::hasRequiredRole`): an empty `roles` list admits any authenticated principal; otherwise
holding any one listed role suffices. A principal with none of them gets `403`
`{"error":"Forbidden","message":"The authenticated principal does not hold any role this endpoint requires."}`
and an info log line `Authorization denied for user '<name>' on <endpoint>`. A failed authentication is
`401` with the endpoint scheme's challenge (`AuthMiddleware::challengeFor`: `Basic realm="flAPI"`, or
`Bearer realm="flAPI"` plus `error="invalid_token"` when a bearer token was refused) and a JSON body
`{"error":"Unauthorized","message":...}`; the audit line's `auth_kind` is the endpoint's scheme. MCP tools
are authorised separately, by `mcp-tool.allowed-roles`.

### Global Auth and Per-Endpoint Override

The top-level `auth:` block is the default for every REST endpoint **without its own `auth:` block**
(`ConfigManager::applyGlobalAuthDefault`, applied at load, on reload, and to endpoints created through
the config service). Such an endpoint is marked `inherited`, which the config service reports as
`auth.inherited`. An endpoint block that names a `type` or credentials replaces the global one outright —
nothing is merged. A block that names neither (`roles:` only) takes the global type and credentials and
keeps its own roles (`ConfigManager::inheritGlobalCredentials`, marked `credentials_inherited`; the YAML
serialiser then writes its roles but not the type); with no enabled global block, or with credentials but
no `type`, loading fails with `AUTH_NO_TYPE`. Keys the chosen type never reads are named in a load warning
(`ConfigManager::warnIgnoredAuthKeys`, also for the global block and `mcp.auth`). Inside any
`auth:` block (global or endpoint) `enabled` defaults to `true` when the block configures authentication
(it sets `type`, `roles`, `users`, `jwt-secret`, `jwt-issuer`, `oidc` or `from-aws-secretmanager`), else to `false`; only an explicit `enabled: false` turns it off or opts an
endpoint out (`parseEndpointAuth`, `parseAuthConfig`, `DeclaresAuthentication`; the effective value is
logged):

```yaml
# Public endpoint (no auth), even with a global auth: block
url-path: /public/status
auth:
  enabled: false

# Protected endpoint with specific roles
url-path: /admin/users
auth:
  enabled: true
  type: basic
  roles: [admin]
  users: [...]
```

### Route precedence and shadowing

An endpoint's auth protects its own route entry. A request is resolved once per layer
(rate limit, auth, handler), all through `ConfigManager::findEndpoint`, which picks the
most specific matching route of the request's method - a literal segment beats a
`:param`, left to right - independent of load order, so the three layers never disagree.
Endpoints that could let an open route answer a protected one's URLs are refused at
admission:

- same route shape + same method (`/x/:id` vs `/x/:cid`; `get` vs `GET`): duplicate;
- overlapping routes whose effective auth differs in any way, in either direction:
  `Overlapping routes with different auth`, unless the more specific endpoint sets
  `allow-shadowing: true` (then a WARNING). The comparison covers enabled, type, roles
  and the identity source - users (name, password digest, per-user roles), a SHA-256
  digest of `jwt-secret`, `jwt-issuer`, OIDC issuer/client/audiences/claims, and the
  `from-aws-secretmanager` source; messages name the differing field, never a value.
  Identity, not spelling: a plaintext password equals its hash (verified), two salted
  PBKDF2 hashes of one password cannot be proven equal and are refused; OIDC is compared
  with the provider preset applied (`email-claim`, `jwks-cache-hours`,
  `enable-client-credentials`, `enable-refresh-tokens` are not identity). A literal is also
  accepted without its own `allow-shadowing` when a covering route of the same method,
  already present, beats the less specific route and has the literal's auth
  (`answeredByEquivalentRoute`); `flapi_create_endpoint` never sets the flag.
  A stricter literal is refused too: it protects one path string, while other spellings
  of the same value (`/customers/07`, `/customers/+7`) still reach the general route;
- `url-path` segments outside `[A-Za-z0-9._~-]+` / `:name`, `.` / `..` segments, a
  repeated `:name`, and methods other than the
  five REST methods: load error / 400. Literal segments are regex-escaped, and a route
  that fails to compile matches nothing rather than crashing the worker.

Route matching is exact and case-sensitive: trailing-slash, case, `//`, `%3A` and `HEAD`
variants do not reach a different endpoint.

### CORS and credentials

`SecurityAuditor` warns `CORS_WILDCARD_WITH_AUTH` when any authentication is enabled and
`cors.allow-origins` is empty or contains `*`: every origin then gets `Access-Control-Allow-Origin: *`,
so a hostile page can call the API with a credential the browser holds and read the answer.

Preflights: Crow answers `OPTIONS` as soon as the request line is parsed, before any header is read, so
middlewares used to see a preflight without `Origin` and dropped every `Access-Control-Allow-*` header.
`FlapiApp` specialises `handle_initial`/`handle` (`src/cors_middleware.cpp`) to defer the OPTIONS answer
until the request is complete; `FlapiCorsMiddleware` then sets Allow-Origin, Allow-Methods, Allow-Headers
(configured list, else the requested headers echoed, else `*` plus `Authorization, Content-Type,
X-Config-Token, Mcp-Session-Id` when auth is on), `Access-Control-Max-Age: 600` and `Vary: Origin`.

The auditor names REST endpoints with their method (`endpoint GET /orders`) and reports
`AUTH_EMPTY_JWT_SECRET` for the global block, an endpoint and `mcp.auth`: authentication fails closed and
every token is refused.

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
| `string` | `min-length`, `max-length`, `regex` (alias `pattern`) | `max-length: 200` |
| `email` | - | Validates email format |
| `uuid` | - | Validates UUID format |
| `enum` | `allowedValues` (alias `values`) | `allowedValues: [active, inactive]` |
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
        allowedValues: [active, inactive, pending]

  - field-name: name
    field-in: body
    validators:
      - type: string
        max-length: 200
        regex: "^[a-zA-Z0-9 ]+$"
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

Triple braces `{{{ }}}` insert the value **unescaped**. They do not double
quotes, so wherever a site stays raw text it is only as safe as the field's
validator. Plain value sites of a typed field outside a section are **bound**
(`'{{{ x }}}'`, `{{{ x }}}`, `'%{{{ x }}}%'` become `?`,
`?` and `('%' || ? || '%')` in `src/prepared_template_rewriter.cpp`); inside
sections, for enum identifiers, typed literals (`DATE '...'`) and
`AT (VERSION|TIMESTAMP => ...)` they stay raw:

```sql
-- Template
WHERE name = '{{{ params.name }}}'

-- Input: O'Brien
-- Output: WHERE name = 'O'Brien'     (syntax error; a crafted value is injection)
```

### Double Braces for Numbers

Outside a `{{#...}}` section, on a field with a typed validator, `{{ }}` is
replaced by `?` and bound as a prepared-statement parameter. Elsewhere it is
HTML-escaped text (`'` becomes `&#39;`), which is not SQL escaping:

```sql
-- Template (field `id` has `validators: [{type: int}]`)
WHERE id = {{ params.id }}

-- Executed as: WHERE id = ?   with the value bound as an integer
```

### Safe Query Pattern

```sql
SELECT * FROM customers
WHERE 1=1
{{#params.name}}
  AND name = '{{{ params.name }}}'   -- String: raw text, relies on the validator
{{/params.name}}
{{#params.id}}
  AND id = {{ params.id }}           -- Number: HTML-escaped text (inside a section, not bound)
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
[Layer 4: Template Rendering]
  - Typed {{ }} outside sections become bound ? parameters
  - Everything else is text: quoting discipline + validators
    ↓
[Query Execution]
  - Any failure → 500 with a fixed body + request_id;
    SQL, paths and database errors go to the server log only
    ↓
[Layer 5: Telemetry Egress]
  - Redact credential-shaped keys, then clamp
  - Export route templates, never filled paths or query strings
  - Enumerated error types, never exception messages
  - Values only at the payload tier, only from declared fields
```

### Error responses

A request that fails inside flAPI answers `500` with a JSON body that carries no SQL, no file paths and
no database error text (`src/request_handler.cpp`):

```json
{"error": "Internal Server Error",
 "message": "The request failed on the server. The details are in the server log under this request id.",
 "request_id": "req-46788500aef28a89"}
```

The detail is logged with the same request id, which is also the `X-Request-Id` response header, so an
operator can find it and a caller learns nothing about the schema.

### Layer 5: Telemetry egress

The first four layers guard what reaches the database. This one guards what
leaves the process — to a collector, to a trace file, or into `audit.jsonl`. It
applies whether or not tracing is enabled, because the audit log is written
either way.

**Never exported, at any tier:**

| Not exported | Why |
|---|---|
| The filled path and the query string | A query string on a data API is by definition a filter over customer data. Only the route *template* is exported. |
| Header values, with one exception | `Authorization`, `Cookie` and everything else are never read into a span. The exception is `User-Agent`, exported as `user_agent.original` clamped to 256 bytes — caller-controlled, so untrusted text downstream, but credential-free and diagnostically useful (`src/request_context_middleware.cpp`). |
| Exception messages | Error status is an enumerated `error.type`. A free-form message is the most reliable way to leak a row value into a trace. |
| Unmatched paths | They collapse to a single `<unmatched>` bucket, so a scanner hitting a thousand URLs produces one label — a cardinality *and* a cost control. |
| Caller-supplied MCP method and tool names | Both are whitelisted or resolved before they can become a span name. |

**The two denylists match differently, deliberately.** The built-in credential
stems are matched as **substrings** of the normalised key, so an operator cannot
forget a variant (`x-api-key`, `auth_token`, `user_password` are all caught). The
operator's `audit.redact` entries are matched on the **whole** normalised key,
because those names were chosen deliberately and silently redacting every field
containing them would surprise. A short exact-match exception set keeps LLM token
counters (`max_tokens`, `token_count`) readable, since this is an MCP tool
surface.

**DuckLake catalog paths.** A Postgres/MySQL `ducklake.metadata-path` is a DSN
with `password=...`, often a resolved `{{env.X}}`. `RedactConnectionSecrets`
(`src/config_manager.cpp`) masks `password`/`passwd`/`pwd`/`*secret*`/`*token*`/
`api_key`/`access_key` values and the password of a `scheme://user:pw@host` URL.
It applies to `metadata-path` / `data-path` in `GET /api/v1/_config/project`
(`ConfigManager::getFlapiConfig`, also behind `/config`) and to the DuckLake
attach error (`DatabaseManager::DuckLakeAttachErrorMessage`), which quoted the
whole `ATTACH` statement in the log and on stderr.

**Redaction** (`src/redaction.cpp`) applies two denylists. The credential stems
are unconditional — they do not depend on the operator's list being complete —
and the operator's `audit.redact` list is reused rather than duplicated, because
two lists diverge and the forgotten half is the one that leaks.

Matching is a **substring test over a normalised key** (lower-cased, `-` and `_`
removed), not equality. Equality was the original design and it let `Token`,
`x-api-key` and `user_password` straight through. Stems are chosen long enough
not to swallow ordinary field names — `authorization` rather than `auth`, so a
field named `author` survives.

**Redact first, clamp second.** Clamping first can truncate mid-value and leave a
partial secret behind; a partial secret is still a secret. Truncation walks back
over UTF-8 continuation bytes so it never splits a character.

**Request ids are always server-minted.** An inbound `X-Request-Id` is never
honoured, so a caller cannot choose their own id, collide with another caller's,
or inject formatting into a log line.

**The `_meta` body peek runs before auth.** It is in the first middleware, so it
is deliberately bounded at 64 KiB rather than the full body limit — an
unauthenticated caller reaches it.

**Denials are never audit-suppressed.** MCP tool calls suppress the HTTP-level
audit line to avoid double-counting, but 401, 403 and 429 are exempt. Silence on
exactly the events a reviewer is looking for is the worst available default.

At the `payload` tier flAPI becomes a processor exporting personal data to a
third destination, with DPA/AVV implications. It is off by default and logs a
warning at startup when enabled.

See [observability.md](./observability.md) for the mechanism and
[../../OBSERVABILITY.md](../../OBSERVABILITY.md) for configuration.

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
rate-limit:
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
    jwt-secret: '{{env.MCP_JWT_SECRET}}'
    methods:
      tools/call:
        required: true
      resources/read:
        required: false  # Allow anonymous reads
```

## Remote configuration trust model

With `--config` or `template.path` in object storage (s3://, gs://, az://, http(s)://), the files read from there are
code: endpoint YAML and SQL templates run with the server's privileges and the credentials flAPI holds. There is no
checksum or signature verification, so **write access to the bucket is equivalent to code execution** on every
server that reads it. What flAPI does enforce for remote origins: a `template-source` outside `template.path` (an
absolute local path, `..` in any encoding, another bucket or host) is refused, `{{include}}` in remote YAML is limited
to files below the same remote directory, `{{env.X}}` stays behind `template.environment-whitelist`, duplicate routes
abort startup, built-in routes (`/health`, `/mcp`, `/api/v1/_config`) cannot be overridden, and URLs are redacted
(`user:pass@`, `?query`, `#fragment`) in logs, errors and config-service/MCP answers. Recommended: read-only,
prefix-scoped credentials for flAPI; write access only through your normal review pipeline; object versioning.
S3 credentials are kept as a redacted DuckDB secret (`zz_flapi_s3`), not as `s3_*` settings that SQL could read back.

## Security Best Practices

1. **Always use validators** - Define type and constraints for all inputs
2. **Use triple braces** - For string values in SQL templates
3. **Require authentication** - For sensitive endpoints
4. **Define roles** - Use RBAC for admin/write operations
5. **Rate limit** - Protect against abuse
6. **Whitelist env vars** - Only expose needed environment variables
7. **Use HTTPS** - Enable TLS in production
8. **Never bundle secrets** - Credentials come from environment
   variables at runtime, never from a config file checked into version
   control. See _Secrets and the bundle_ below.

```yaml
enforce-https:
  enabled: true
  ssl-cert-file: /path/to/cert.pem
  ssl-key-file: /path/to/key.pem
```

## Secrets and the bundle

When `flapi pack` produces a self-contained binary, it must _not_
include credentials. A bundled binary that contains a database
password is itself a secret: it can be `scp`-ed, shared, attached to
a bug report, or pushed to a public registry by mistake. The single
artifact value disappears the moment the artifact is sensitive.

Two mechanisms enforce this:

1. **Pack-time refusal of the default deny list.** `Pack()` walks
   the input tree and aborts with a non-zero exit if any file
   matches:
   - `*.env`              at any depth (e.g. `.env`, `config/.env`)
   - `secrets/` segment   at any depth (e.g. `secrets/db.token`,
     `nested/secrets/api.key`)
   - `*.pem`              at any depth
   - `*.key`              at any depth

   The error message names the offending file and points at the
   `--allow-secrets` testing-only override. The override exists so
   we can integration-test the deny list; production users must not
   flip it.

2. **Runtime expects credentials from the environment.** Every
   credential flapi understands is read from a documented
   environment variable -- never from a bundled YAML field:
   - AWS S3:    `AWS_ACCESS_KEY_ID` / `AWS_SECRET_ACCESS_KEY` / `AWS_REGION`
   - GCS:       an HMAC key in a `CREATE SECRET (TYPE gcs, …)` in a connection `init`, its
                values from `{{env.NAME}}` (`GOOGLE_*` variables are detected but not applied)
   - Azure:     `AZURE_STORAGE_CONNECTION_STRING` / `AZURE_STORAGE_ACCOUNT` / `AZURE_STORAGE_KEY`
   - Mgmt API:  `FLAPI_CONFIG_SERVICE_TOKEN`
   - JWT:       configured via `{{env.JWT_SECRET}}` against a
                whitelisted env var

   YAML strings may interpolate `{{env.VARNAME}}` for any env var
   declared in `environment-whitelist`. The whitelist is itself part
   of the configuration, so it ships in the bundle -- which is fine
   because it names env vars by _key_, not value.

See [DESIGN_DECISIONS.md §9](../DESIGN_DECISIONS.md#9-self-packaging-via-appended-zip)
for the broader rationale and
[CONFIG_REFERENCE.md §1.4](../../CONFIG_REFERENCE.md) for the
12-factor checklist of every env var flapi reads.

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
| `src/pack.cpp` (`IsSecretExcluded`) | Default secret deny list for `flapi pack` |
| `src/redaction.cpp` | Credential-key stems, shared by audit and spans |
| `src/trace_capture_policy.cpp` | Capture tiers, redact-then-clamp |
| `src/request_context_middleware.cpp` | Telemetry egress enforcement |
| `src/audit_logger.cpp` | Audit records and their redaction |

## Related Documentation

- [DESIGN_DECISIONS.md](../DESIGN_DECISIONS.md#8-defense-in-depth-security) - Security philosophy
- [../../CONFIG_REFERENCE.md](../../CONFIG_REFERENCE.md) - Auth configuration options
- [observability.md](./observability.md) - How telemetry egress is enforced
- [../../OBSERVABILITY.md](../../OBSERVABILITY.md) - Capture tiers and redaction config
