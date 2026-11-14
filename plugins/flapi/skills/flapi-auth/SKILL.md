---
name: flapi-auth
description: >
  Securing flAPI: per-endpoint Basic auth with PBKDF2 hashes, bearer (HS256 JWT)
  auth, OIDC with provider presets (google, microsoft, keycloak, auth0, okta,
  github), roles and the auth context in SQL templates, MCP authentication and
  allowed-roles, OAuth protected-resource metadata, the config-service token,
  CORS, TLS (enforce-https) and the startup security warnings. Use when a user
  asks to require a login or token, restrict who may call an endpoint or tool,
  debug a 401/403, or harden a deployment.
version: 26.10.31
user-invocable: false
---

# flAPI Authentication & Authorization Cheat Sheet

**flAPI:** v26.10.31 | **Skill:** Develop | **See also:** `flapi-rest`, `flapi-mcp`, `flapi-live-config`, `flapi-deploy`

Three independent gates: the endpoint's own `auth:` block (REST), `mcp.auth`
(everything on `/mcp/jsonrpc`), and the config-service token (`/api/v1/_config/*`,
`/config`).

## Critical gotchas

- **REST auth is per endpoint only.** A top-level `auth:` block in `flapi.yaml`
  protects no REST endpoint (its `users` and `jwt-secret` are never used); only
  its `oidc:` sub-block is copied into endpoints that declare `type: oidc` without
  their own. Put the block in every endpoint, or share it with
  `{{include:auth from ...}}` (see `flapi-rest`).
- **The REST types are `basic`, `bearer` and `oidc`.** `type: jwt` matches nothing
  and refuses every request with 401 (startup warning `AUTH_UNKNOWN_TYPE`).
  A signed HS256 JWT is `type: bearer`.
- **REST has no role check and never returns 403.** An `auth.roles:` list on an
  endpoint is not read; any authenticated user gets 200. Separate audiences with
  separate endpoints/user lists, or filter in SQL with `auth.roles`. Role-based
  403s exist only for MCP tools/resources/prompts (`allowed-roles`).
- **Bearer tokens must carry `sub`, an `iss` equal to `jwt-issuer`, and a `roles`
  array** (REST rejects a token without `roles`). Set `jwt-issuer`; an empty one
  matches no token. An expired `exp` is 401. An empty `jwt-secret` (unset env
  var) refuses all tokens (`AUTH_EMPTY_JWT_SECRET`).
- **Passwords: use `$pbkdf2-sha256$<iter>$<salt>$<hash>` with URL-safe base64
  (`-_`, no padding).** bcrypt hashes never verify; passlib's `pbkdf2_sha256`
  output (`./` alphabet) usually does not either. The startup auditor wrongly
  prints `AUTH_PLAINTEXT_PASSWORD` for PBKDF2 hashes (and suggests bcrypt);
  ignore that one line for PBKDF2.
- **With `mcp.auth.enabled: true`, a tool without `allowed-roles` is denied to
  everyone** (403). `tools/list` still lists every tool.
- **Secrets only via whitelisted `{{env.X}}`**; an empty user password from an
  unset variable stops startup, an empty `jwt-secret` does not (it fails closed).
- **CORS default is `Access-Control-Allow-Origin: *`** (empty `allow-origins`).
  Set an allowlist for anything browser-facing.
- **OIDC: `issuer-url` must equal the token's `iss` byte for byte** (Auth0 has a
  trailing `/`). Presets never substitute `{tenant}`/`{realm}`/`{domain}`.

## Basic auth (runnable)

Generate a hash (600 000 iterations, 16-byte salt, URL-safe base64):

```bash
python3 -c "import hashlib,base64,os,sys; s=os.urandom(16); e=lambda b: base64.urlsafe_b64encode(b).decode().rstrip('='); print('\$pbkdf2-sha256\$600000\$'+e(s)+'\$'+e(hashlib.pbkdf2_hmac('sha256',sys.argv[1].encode(),s,600000)))" 'reader-pass'
```

```yaml
# flapi: endpoint group=basic file=me.yaml
url-path: /me
method: GET
with-pagination: false
auth:
  enabled: true
  type: basic
  users:
    - username: reader      # password: reader-pass
      password: '$pbkdf2-sha256$600000$FpkEyJklqXpwdoxWhoRzvw$RA15LygYf0O0yiG8OPQ91NWeeSNqcxTwMl4pRXLcmq0'
      roles: [read]
template-source: me.sql
connection: [customers]
# flapi: check GET /me -> 401
```

```sql
-- flapi: template group=basic file=me.sql
SELECT '{{ auth.username }}' AS username,
       '{{ auth.roles }}'    AS roles,
       '{{ auth.type }}'     AS auth_type
```

```yaml
# flapi: endpoint group=basic file=public.yaml
url-path: /public
method: GET
template-source: public.sql
connection: [customers]
# flapi: check GET /public -> 200
```

```sql
-- flapi: template group=basic file=public.sql
SELECT count(*) AS customers FROM read_parquet('{{{ conn.path }}}')
```

A hash is not a secret you must hide, but keep real ones out of git anyway:

```yaml
template:
  environment-whitelist: ['^FLAPI_.*']   # in flapi.yaml
# in the endpoint:
auth:
  enabled: true
  type: basic
  users:
    - username: admin
      password: '{{env.FLAPI_ADMIN_PASSWORD_HASH}}'   # unset or empty -> flAPI refuses to start
      roles: [admin]
```

```bash
curl -i http://localhost:8080/me                      # 401, WWW-Authenticate: Basic realm="flAPI"
curl -i -u reader:wrong http://localhost:8080/me      # 401
curl -s -u reader:reader-pass http://localhost:8080/me
# [{"username":"reader","roles":"read","auth_type":"basic"}]
```

Users can also come from AWS Secrets Manager:
`auth.from-aws-secretmanager: {secret-name, region, secret-id, secret-key, secret-table, init}`.

## Bearer (HS256 JWT)

```yaml
auth:
  enabled: true
  type: bearer
  jwt-secret: '{{env.FLAPI_JWT_SECRET}}'   # >= 32 random bytes
  jwt-issuer: my-issuer                    # token "iss" must equal this
```

Mint a test token without extra packages:

```bash
TOKEN=$(python3 - "$FLAPI_JWT_SECRET" <<'EOF'
import base64, hmac, hashlib, json, sys, time
b = lambda d: base64.urlsafe_b64encode(d).rstrip(b"=").decode()
h = b(json.dumps({"alg": "HS256", "typ": "JWT"}).encode())
p = b(json.dumps({"sub": "alice", "iss": "my-issuer", "roles": ["analyst"],
                  "exp": int(time.time()) + 3600}).encode())
s = b(hmac.new(sys.argv[1].encode(), f"{h}.{p}".encode(), hashlib.sha256).digest())
print(f"{h}.{p}.{s}")
EOF
)
curl -H "Authorization: Bearer $TOKEN" http://localhost:8080/orders
```

## OIDC

```yaml
auth:
  enabled: true
  type: oidc
  oidc:
    provider-type: keycloak                      # generic | google | microsoft | keycloak | auth0 | okta | github
    issuer-url: https://sso.example.com/realms/sales
    client-id: flapi-api
    allowed-audiences: [flapi-api]               # empty = any audience
    # optional overrides:
    username-claim: preferred_username
    roles-claim: roles
    role-claim-path: realm_access.roles          # dotted path, wins over roles-claim
    verify-expiration: true                      # default true
    clock-skew-seconds: 300                      # default 300
    jwks-cache-hours: 24                         # default 24
```

Keys discovered from `<issuer-url>/.well-known/openid-configuration`; JWKS
cached. Other keys read: `client-secret`, `email-claim`, `groups-claim`,
`scopes`, `enable-client-credentials`, `enable-refresh-tokens` (groups are not
merged into roles). An `auth.oidc` block in `flapi.yaml` is inherited by every
endpoint that has `type: oidc` and no `oidc:` of its own (the endpoint still
needs `enabled: true` and `type: oidc`).

| `provider-type` | Default `issuer-url` (when empty) | `username-claim` (when unset/`sub`) | Roles from |
|---|---|---|---|
| `generic` / unset | none, required | `sub` | `roles` |
| `google` | `https://accounts.google.com` | `email` | `roles` |
| `microsoft` | `https://login.microsoftonline.com/{tenant}/v2.0` | `preferred_username` | `roles` |
| `keycloak` | `https://keycloak.example.com/realms/{realm}` | `preferred_username` | `realm_access.roles` |
| `auth0` | `https://{domain}.auth0.com` | `email` | `roles` |
| `okta` | `https://{domain}.okta.com/oauth2/default` | `preferred_username` | `roles` |
| `github` | `https://github.com` | `login` | `roles` (not real OIDC; logs a warning) |

Always set the real `issuer-url`. For microsoft/keycloak/auth0/okta a real URL
(without the `{...}` placeholder) makes flAPI log `OIDC configuration error:
... requires 'issuer-url' with {tenant} placeholder` on first use; validation
still works, so that log line is harmless.

## Roles and the auth context in SQL

After authentication the template sees `auth.username`, `auth.roles`
(comma-joined), `auth.email`, `auth.type`, `auth.authenticated` (`"true"`).
Clients cannot spoof them: `__auth_*` query/body keys are dropped. Quote them
with double braces (HTML-escaped, so a `'` cannot end the literal):

```sql
SELECT * FROM orders
WHERE owner = '{{ auth.username }}'
   OR list_contains(string_split('{{ auth.roles }}', ','), 'admin')
```

That is row filtering (empty result), not a 403. For MCP the same variables are
set from the MCP principal.

## MCP auth (runnable)

```yaml
# flapi: config group=mcpauth
project-name: mcp-auth-demo
project-description: MCP tools behind basic auth with role checks
template:
  path: ./sqls
  environment-whitelist: ['^FLAPI_.*']
connections:
  customers:
    properties:
      path: './data/customers.parquet'
mcp:
  enabled: true
  auth:
    enabled: true
    type: basic                 # basic | bearer (default) | oidc
    users:
      - username: agent         # password: reader-pass
        password: '$pbkdf2-sha256$600000$FpkEyJklqXpwdoxWhoRzvw$RA15LygYf0O0yiG8OPQ91NWeeSNqcxTwMl4pRXLcmq0'
        roles: [analyst]
```

```yaml
# flapi: endpoint group=mcpauth file=customer-count.yaml
mcp-tool:
  name: customer_count
  description: Count customers, optionally for one nation key (0-24).
  allowed-roles: [analyst]
request:
  - field-name: nation
    field-in: query
    validators:
      - type: int
        min: 0
        max: 24
template-source: customer-count.sql
connection: [customers]
# flapi: check POST /mcp/jsonrpc body={"jsonrpc":"2.0","id":1,"method":"tools/list"} -> 401
# flapi: check POST /mcp/jsonrpc body={"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"customer_count","arguments":{}}} -> 401
# flapi: check GET /.well-known/oauth-protected-resource -> 404
# flapi: check GET /api/v1/_config/endpoints -> 401
```

```sql
-- flapi: template group=mcpauth file=customer-count.sql
SELECT count(*) AS customers FROM read_parquet('{{{ conn.path }}}')
WHERE ({{ params.nation }} IS NULL OR c_nationkey = {{ params.nation }})
```

```bash
MCP=http://localhost:8080/mcp/jsonrpc
call='{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"customer_count","arguments":{"nation":5}}}'
curl -s -o /dev/null -w '%{http_code}\n' $MCP -H 'Content-Type: application/json' -d "$call"                      # 401
curl -s -w ' %{http_code}\n' -u agent:reader-pass $MCP -H 'Content-Type: application/json' -d "$call"           # 200
# a caller whose roles miss allowed-roles: 403, error "Permission denied: Tool 'x' requires one of [...]",
# header WWW-Authenticate: Bearer error="insufficient_scope"
```

- `mcp.auth.type: bearer` reads `jwt-secret` and `jwt-issuer` (default `flapi`);
  the `roles` claim is optional there. `type: oidc` reads `mcp.auth.oidc` (same
  keys as above).
- Every MCP method needs auth, `initialize` included; `server/discover` is
  always public. Relax one method with `mcp.auth.methods.<method>.required: false`
  (e.g. a `methods:` map under `mcp.auth` with key `tools/list` and child
  `required: false`, letting clients list tools anonymously).
- `allowed-roles` also exists on `mcp-resource` and `mcp-prompt`.
- Endpoint-level `auth:` does not apply to the MCP side of a dual REST+MCP file.

### OAuth protected-resource metadata (RFC 9728)

Only with `mcp.auth.type: oidc`: `GET /.well-known/oauth-protected-resource`
returns `{resource, authorization_servers:[issuer-url], bearer_methods_supported,
scopes_supported}`, and 401s carry
`WWW-Authenticate: Bearer resource_metadata="<origin>/.well-known/oauth-protected-resource"`.
Otherwise it is 404.

```yaml
mcp:
  auth:
    enabled: true
    type: oidc
    canonical-resource-uri: https://api.example.com/mcp/jsonrpc   # else derived from Host / X-Forwarded-*
    scopes-supported: [flapi.read]
    oidc:
      issuer-url: https://sso.example.com/realms/sales
      client-id: flapi-mcp
      allowed-audiences: [https://api.example.com/mcp/jsonrpc]
```

## Config-service token

`flapi --config-service` enables `/api/v1/_config/*` and `/config`; the token
comes from `--config-service-token`, else `FLAPI_CONFIG_SERVICE_TOKEN`, else a
generated one printed at startup. Clients send `Authorization: Bearer <token>`
(or `X-Config-Token`); `flapii` reads `--config-service-token` /
`FLAPI_CONFIG_SERVICE_TOKEN`. Without the flag `/api/v1/_config/*` is not
registered and `/config` always answers 401.

```bash
export FLAPI_CONFIG_SERVICE_TOKEN=$(openssl rand -hex 32)
flapi -c flapi.yaml --config-service
curl -s -o /dev/null -w '%{http_code}\n' http://localhost:8080/api/v1/_config/endpoints   # 401
curl -s -H "Authorization: Bearer $FLAPI_CONFIG_SERVICE_TOKEN" http://localhost:8080/api/v1/_config/endpoints
```

## CORS and TLS

```yaml
cors:
  allow-origins: [https://app.example.com]   # [] or absent = "*"; any "*" entry = "*"
  allow-headers: [Authorization, Content-Type]
  allow-methods: [GET, POST]
enforce-https:
  enabled: true
  ssl-cert-file: ./ssl/cert.pem
  ssl-key-file: ./ssl/key.pem
```

A disallowed `Origin` gets no `Access-Control-Allow-*` headers. With
`enforce-https.enabled` the listener speaks TLS only (no HTTP redirect); a
top-level `https:` block is ignored with a warning.

## Startup security warnings

Printed to stderr as `SECURITY WARNINGS (n)` on every start and on
`--validate-config`; they never stop startup.

| Code | Meaning |
|---|---|
| `AUTH_PLAINTEXT_PASSWORD` | a user password is neither MD5 hex nor bcrypt (also printed, wrongly, for PBKDF2) |
| `AUTH_MD5_PASSWORD` | 32-hex MD5 password: unsalted, migrate to PBKDF2 |
| `AUTH_EMPTY_JWT_SECRET` | bearer endpoint with empty `jwt-secret`: all tokens refused |
| `AUTH_UNKNOWN_TYPE` | `type` is not basic/bearer/oidc: all requests 401 |
| `MCP_UNAUTHENTICATED_TOOLS` | `mcp.enabled` with tools and `mcp.auth.enabled: false` |
| `CREDENTIAL_TOO_SHORT_TO_REDACT` | a credential connection property under 4 chars: error detail is withheld |

## Canonical auth setup

1. Pick the gate: REST endpoint `auth:`, `mcp.auth`, or both (they are separate).
2. Put secrets in env vars matching `template.environment-whitelist`; hash
   Basic passwords with the PBKDF2 one-liner.
3. Add the `auth:` block to every protected endpoint (an include keeps it in
   one file); for MCP add `allowed-roles` to every tool.
4. `flapi -c flapi.yaml --validate-config` and read `SECURITY WARNINGS`.
5. Prove each path with curl: no credentials -> 401, wrong credentials -> 401,
   right credentials -> 200; for MCP a role-less caller -> 403.
6. Lock down CORS, terminate TLS (`enforce-https` or a proxy), and run with a
   strong `FLAPI_CONFIG_SERVICE_TOKEN` or without `--config-service`.
