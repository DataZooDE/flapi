---
name: flapi-auth
description: >
  Securing flAPI: per-endpoint Basic auth with PBKDF2 hashes, bearer (HS256 JWT)
  auth, OIDC with provider presets (google, microsoft, keycloak, auth0, okta),
  roles and the auth context in SQL templates, MCP authentication and
  allowed-roles, OAuth protected-resource metadata, the config-service token,
  CORS, TLS (enforce-https) and the startup security warnings. Covers global
  auth inherited by every endpoint, opt-out, and REST roles (403). Use when a user
  asks to require a login or token, restrict who may call an endpoint or tool,
  debug a 401/403, or harden a deployment.
version: 26.11.26
user-invocable: false
---

# flAPI Authentication & Authorization Cheat Sheet

**flAPI:** v26.11.26 | **Skill:** Develop | **See also:** `flapi-rest`, `flapi-mcp`, `flapi-live-config`, `flapi-deploy`

Three independent gates: the endpoint's own `auth:` block (REST), `mcp.auth`
(everything on `/mcp/jsonrpc`), and the config-service token (`/api/v1/_config/*`,
`/config`).

## Critical gotchas

- **A top-level `auth:` block in `flapi.yaml` protects every REST endpoint that
  has no `auth:` block of its own.** An endpoint's own block REPLACES the global
  one (no merge: repeat `users`/`jwt-secret` there). `auth: {enabled: false}` on
  an endpoint opts it out. A block without `enabled` that sets `type`, `roles`,
  `users`, `jwt-*`, `oidc` or `from-aws-secretmanager` is ON (it used to be off,
  i.e. open); only an explicit `enabled: false` turns auth off. Startup logs `Global auth (type 'basic') applies to N
  endpoint(s) ...; M endpoint(s) opt out ...`. Upgrading a config that had a
  top-level block: endpoints that were open now answer 401.
- **Exception: a block with no scheme and no credentials (`auth: {roles: [admin]}`)
  inherits the global type and credentials** and applies its own roles. With no
  enabled global block that is a load error `AUTH_NO_TYPE`; so is `users:` or a
  secret without `type:`. One scheme per block (and per `mcp.auth`): keys the
  type ignores (`users:` under bearer, `jwt-secret:` under basic) log a warning.
- **An endpoint's auth protects its own route entry, not a URL space.** A request
  is answered by the most specific matching route of its method (literal segment
  beats `:param`, left to right; file order never matters), and that endpoint's
  auth applies. Overlapping routes whose effective auth differs IN ANY WAY are
  refused at load, reload, create and update, in either direction (open
  `/customers/7` beside a protected `/customers/:id`, or a protected literal beside
  an open one: `Overlapping routes with different auth: ...`), unless the more
  specific one sets `allow-shadowing: true` (then a WARNING). "Different" includes
  the identity source: other `users` or passwords, `jwt-secret`, `jwt-issuer`, OIDC
  issuer/client/audience, AWS secret - a literal with its own `guest` user is refused.
  `method: get` is GET, so it cannot slip a second copy in.
  Audit with `flapii endpoints list` (method, url-path and auth of each).
- **A literal route protects that exact path string, not a record.** `/customers/07`
  and `/customers/+7` still reach `/customers/:id` and select row 7. To protect a
  record, give both routes the same auth, or protect the general endpoint and filter
  by the caller in its SQL:
  `AND (id <> 7 OR list_contains(string_split('{{{ auth.roles }}}', ','), 'admin'))`.
- **`auth.*` goes inside a plain top-level `'...'` literal of the outer query.**
  `auth.username`, `auth.email`, `auth.roles`, `auth.type` and each list item have every `'` doubled
  (`O'Brien` -> `O''Brien`), so write `WHERE owner = '{{{ auth.username }}}'`. Refused
  (startup, reload, `--validate-config`, the config service, and on the rendered SQL of every
  request): unquoted, `E'...'`, a `--` or `/* */` comment, a `"..."` identifier, a `$$`
  string, and SQL text another function runs - a literal inside `query()`,
  `bigquery_query()`, `postgres_query()` or any `*_query`/`*_execute` call, a literal holding
  `''`, `$$` or `"`, or one beside such a literal in a call (a macro handed SQL). Push down
  without `auth.*` and filter in the outer query. A quoted value still means something to
  `LIKE` (`%`, `_`), regexes, `GLOB` and file paths (`*`, `?`, `[`): compare with `=`. An
  identity holding a control character (newline, NUL, ...) or a role containing a comma is
  refused with 401 (a comma in a group is fine).
- **A 401 names the endpoint's scheme**: `WWW-Authenticate: Basic realm="flAPI"`
  or `Bearer realm="flAPI"` (`, error="invalid_token"` for a refused token), body
  `{"error":"Unauthorized","message":...}`.
- **The REST types are `basic`, `bearer` and `oidc`; `type: jwt` is an alias of
  `bearer`.** Any other type is a load error (below).
- **`auth.roles` is enforced on REST.** An authenticated caller holding none of
  the listed roles gets **403** `{"error":"Forbidden","message":...}`; no or
  wrong credentials stay 401; `roles: []` (or absent) = any authenticated user.
  The global block accepts `roles` too.
- **Bearer tokens must carry a string `sub` and an `exp`** (a token without
  `exp` is 401 unless `jwt-require-exp: false`; an expired one always is). `iss`
  must equal `jwt-issuer` and `aud` contain one of `jwt-audience` (string or
  list) where those are set; unset `jwt-issuer` = any issuer, startup warns
  `AUTH_JWT_ISSUER_UNCHECKED`. `roles` is optional (missing = no roles, so a
  `roles:` list answers 403); `email` fills `auth.email`. REST and `mcp.auth`
  run the same check. An empty `jwt-secret` (unset env var) refuses all
  tokens (`AUTH_EMPTY_JWT_SECRET`).
- **Passwords: store a PBKDF2-SHA256 hash** `$pbkdf2-sha256$<iter>$<salt>$<hash>`.
  passlib's `pbkdf2_sha256` output, standard and URL-safe base64 all verify.
  **bcrypt never verifies** (startup warning `AUTH_BCRYPT_UNSUPPORTED`: that user
  cannot log in). **PBKDF2 costs ~165 ms CPU per check at 600k iterations.** A
  successful check is remembered for 5 minutes (per user + password + hash, at most
  10,000 entries), so only the first request with a password pays it; a wrong password
  pays it every time. At most half the cores derive at once; a request that waits
  more than 2 s for a slot is refused (401, log `too many password checks in
  progress`). Reloading the endpoint file (or any auth change) clears the cache. For
  hot paths and many distinct clients prefer bearer/OIDC (a signature check, no KDF).
- **`type` must be `basic`, `bearer` (alias `jwt`) or `oidc`.** A typo fails to
  load / reload with `AUTH_UNKNOWN_TYPE` naming the endpoint and the valid types.
- **With `mcp.auth.enabled: true`, a tool without `allowed-roles` is denied to
  everyone** (403). `tools/list` still lists every tool. MCP uses only
  `mcp.auth`; the REST `auth:` blocks (global or endpoint) do not apply to it.
- **Secrets only via whitelisted `{{env.X}}`**; an empty user password from an
  unset variable stops startup, an empty `jwt-secret` does not (it fails closed).
- **CORS default is `Access-Control-Allow-Origin: *`** (empty `allow-origins`).
  With any auth enabled that prints `CORS_WILDCARD_WITH_AUTH`; set an allowlist.
  Preflights (`OPTIONS` + `Access-Control-Request-Method`, REST and
  `/mcp/jsonrpc`) answer 204 with Allow-Origin, Allow-Methods, Allow-Headers
  (requested headers echoed unless `cors.allow-headers` is set), Max-Age 600 and
  `Vary: Origin`.
- **OIDC: `issuer-url` must equal the token's `iss` byte for byte** (the Auth0
  preset adds Auth0's trailing `/`). Preset placeholders `{tenant}`/`{realm}`/`{domain}`
  are filled from `tenant-id`/`realm`/`domain`; one left over stops startup naming the key.
- **OIDC without `allowed-audiences`: `aud` must contain `client-id`.** Access
  tokens usually name the API (`api://...`), so list it in `allowed-audiences`.
- **`provider-type: github` stops startup** (GitHub OAuth apps issue no OIDC
  tokens). GitHub Actions tokens: `generic` + `issuer-url:
  https://token.actions.githubusercontent.com` + `allowed-audiences`.

## Basic auth with global inheritance (runnable)

Generate a hash with passlib (or the stdlib one-liner below, same format):

```bash
python3 -c "from passlib.hash import pbkdf2_sha256; import getpass; print(pbkdf2_sha256.using(rounds=600000).hash(getpass.getpass()))"
python3 -c "import hashlib,base64,os,sys; s=os.urandom(16); e=lambda b: base64.urlsafe_b64encode(b).decode().rstrip('='); print('\$pbkdf2-sha256\$600000\$'+e(s)+'\$'+e(hashlib.pbkdf2_hmac('sha256',sys.argv[1].encode(),s,600000)))" 'reader-pass'
```

```yaml
# flapi: config group=basic
project-name: basic-auth-demo
template:
  path: ./sqls
connections:
  customers:
    properties:
      path: './data/customers.parquet'
cors:
  allow-origins: [https://app.example.com]   # "*" with auth -> CORS_WILDCARD_WITH_AUTH
auth:                                        # applies to every endpoint without its own auth:
  enabled: true
  type: basic
  users:
    - username: reader      # password: reader-pass
      password: '$pbkdf2-sha256$600000$FpkEyJklqXpwdoxWhoRzvw$RA15LygYf0O0yiG8OPQ91NWeeSNqcxTwMl4pRXLcmq0'
      roles: [read]
    - username: admin       # password: reader-pass (demo only)
      password: '$pbkdf2-sha256$600000$FpkEyJklqXpwdoxWhoRzvw$RA15LygYf0O0yiG8OPQ91NWeeSNqcxTwMl4pRXLcmq0'
      roles: [admin]
```

```yaml
# flapi: endpoint group=basic file=me.yaml
url-path: /me
method: GET
with-pagination: false
template-source: me.sql
connection: [customers]
# no auth: block -> inherits the global one
# flapi: check GET /me -> 401
```

```sql
-- flapi: template group=basic file=me.sql
SELECT '{{{ auth.username }}}' AS username,
       '{{{ auth.roles }}}'    AS roles,
       '{{{ auth.type }}}'     AS auth_type
```

```yaml
# flapi: endpoint group=basic file=public.yaml
url-path: /public
method: GET
template-source: public.sql
connection: [customers]
auth:
  enabled: false            # opt out of the global block
# flapi: check GET /public -> 200
```

```sql
-- flapi: template group=basic file=public.sql
SELECT count(*) AS customers FROM read_parquet('{{{ conn.path }}}')
```

```bash
curl -i http://localhost:8080/me                      # 401, WWW-Authenticate: Basic realm="flAPI"
curl -i -u reader:wrong http://localhost:8080/me      # 401
curl -s -u reader:reader-pass http://localhost:8080/me
# [{"username":"reader","roles":"read","auth_type":"basic"}]
```

Keep real hashes out of git with a whitelisted env var:

```yaml
template:
  environment-whitelist: ['^FLAPI_.*']   # in flapi.yaml
auth:
  enabled: true
  type: basic
  users:
    - username: admin
      password: '{{env.FLAPI_ADMIN_PASSWORD_HASH}}'   # unset or empty -> flAPI refuses to start
      roles: [admin]
```

Users can also come from AWS Secrets Manager (endpoint or top-level `auth:`, `type: basic`):

```yaml
auth:
  enabled: true
  type: basic
  from-aws-secretmanager:
    secret-name: prod/flapi/users      # SecretString: {"auth":[{"username","password","roles":[...]}]}
    region: eu-central-1               # else AWS_REGION, else AWS_DEFAULT_REGION
    # secret-id / secret-key: both or neither; neither = the AWS SDK default credential chain
```

- Passwords in the secret take the `users:` formats (PBKDF2 preferred); `roles` gives 403s as usual.
- Fetched **once at startup** per `secret-name` (shared by every endpoint naming it). Rotate = update the
  secret and restart. Blocks sharing a `secret-name` must agree (region, credentials, init). Region falls back to
  `AWS_REGION`, `AWS_DEFAULT_REGION`, then the AWS profile.
- Endpoint override: only `AWS_ENDPOINT_URL_SECRETS_MANAGER` (not `AWS_ENDPOINT_URL`); plain `http://` only to
  loopback unless `FLAPI_ALLOW_INSECURE_SECRETS_ENDPOINT=1`; startup warns while it is set.
- The users and the AWS credentials never reach the DuckDB templates run on: a template cannot read them.
- `secret-table` has no effect (startup warns); `init:` is deprecated (legacy SQL creating a DuckDB S3 secret,
  run in a private DuckDB).

## Bearer (HS256 JWT)

```yaml
auth:
  enabled: true
  type: bearer                             # `jwt` is an alias; auth.type in SQL reads "bearer"
  jwt-secret: '{{env.FLAPI_JWT_SECRET}}'   # >= 32 random bytes
  jwt-issuer: my-issuer                    # token "iss" must equal this (unset: not checked)
  jwt-audience: orders-api                 # optional, string or list: "aud" must contain one
  jwt-require-exp: true                    # default; false accepts tokens without "exp"
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
    provider-type: keycloak                      # generic | google | microsoft (azure, azure-ad, entra) | keycloak | auth0 | okta
    domain: sso.example.com                      # fills {domain}; issuer https://sso.example.com/realms/sales
    realm: sales                                 # fills {realm}
    client-id: flapi-api
    allowed-audiences: [flapi-api]               # empty = aud must contain client-id
    # optional overrides (an explicit claim wins over the preset):
    username-claim: preferred_username
    roles-claim: roles                           # name as written first: https://x.io/roles works
    role-claim-path: realm_access.roles          # dotted path, wins over roles-claim
    groups-claim: groups                         # -> list auth.groups / auth.groups_sql, grants nothing
    verify-expiration: true                      # default true
    clock-skew-seconds: 300                      # default 300
    jwks-cache-hours: 24                         # default 24
    jwks-refetch-cooldown-seconds: 60            # default 60: unknown kid -> at most 1 JWKS fetch per minute
```

Keys discovered from `<issuer-url>/.well-known/openid-configuration` once per
issuer (REST and MCP share it); JWKS cached, and a token with an unknown `kid`
triggers at most one refetch per cooldown (within it: 401, the issuer is not
asked, so a rotated-in key works within a minute). Signatures: RS256/384/512, PS256/384/512 (RSA), ES256/384/512 (EC
P-256/P-384/P-521); the token `alg` must fit its key (type, curve, and the
JWK `alg` if set); `none`/HS* refused. Also read: `email-claim`. `client-secret`, `scopes`,
`enable-client-credentials` and `enable-refresh-tokens` have no effect (startup
warns `parsed but has no effect`): flAPI only validates tokens. `nbf`/`iat`
more than `clock-skew-seconds` in the future are refused. A role with a comma,
or any value with a control character, refuses the token (401). A global `auth:` block with `type: oidc` protects every
endpoint without its own block (see the gotchas); an endpoint with `type: oidc`
and no `oidc:` of its own uses the global `auth.oidc`.

| `provider-type` | Default `issuer-url` (when empty) | `username-claim` (when not written) | Roles from |
|---|---|---|---|
| `generic` / unset | none, required | `sub` | `roles` |
| `google` | `https://accounts.google.com` | `email` | `roles` |
| `microsoft` (`azure`, `azure-ad`, `entra`) | `https://login.microsoftonline.com/{tenant}/v2.0` | `preferred_username` | `roles` |
| `keycloak` | `https://{domain}/realms/{realm}` | `preferred_username` | `realm_access.roles` |
| `auth0` | `https://{domain}.auth0.com/` (a dotted `domain` is the full host; always a trailing `/`) | `email` | set `roles-claim` to your namespaced claim, e.g. `https://example.com/roles` |
| `okta` | `https://{domain}.okta.com/oauth2/default` | `preferred_username` | `roles` |

Placeholders in the default (or your own) `issuer-url` are substituted from
`tenant-id` (alias `tenant`), `realm` and `domain`. A placeholder left unfilled
is a load error, e.g. `OIDC: 'issuer-url' (https://login.microsoftonline.com/{tenant}/v2.0)
still contains the {tenant} placeholder. Set `tenant-id` ...`. Writing the full
real `issuer-url` works too.

## Roles: 403 on REST

```yaml
# endpoint (its own block replaces the global one, so it lists users again)
auth:
  enabled: true
  type: basic
  roles: [admin]            # caller needs ANY one of these
  users:
    - username: reader
      password: '$pbkdf2-sha256$600000$FpkEyJklqXpwdoxWhoRzvw$RA15LygYf0O0yiG8OPQ91NWeeSNqcxTwMl4pRXLcmq0'
      roles: [read]
```

```bash
curl -s -w ' %{http_code}\n' -u reader:reader-pass http://localhost:8080/admin
# {"message":"The authenticated principal does not hold any role this endpoint requires.","error":"Forbidden"} 403
```

The denial is logged as `Authorization denied for user 'reader' on REST
endpoint: /admin: requires one of the endpoint's roles request_id=req-...`.
`flapii endpoints get /admin` shows `auth.roles` and `auth.inherited` (true when
the block came from `flapi.yaml`).

## The auth context in SQL

After authentication the template sees `auth.username`, `auth.roles`
(comma-joined), `auth.email`, `auth.type`, `auth.authenticated` (`"true"`), and
the lists `auth.role_list` and `auth.groups` (OIDC `groups-claim`; commas in a
group name are fine) for a section, `{{#auth.groups}}'{{.}}', {{/auth.groups}}`.
`auth.roles_sql` / `auth.groups_sql` render them as one SQL value,
`['a','CN=x,OU=y']::VARCHAR[]`, written UNQUOTED where a value goes:
`list_contains({{{ auth.groups_sql }}}, 'CN=admins,OU=staff')`; refused inside
quotes, comments, identifiers and SQL another function runs. A plain
`{{{ auth.groups }}}` tag is refused at load (it is a list).
Clients cannot spoof them: `__auth_*` query/body keys are dropped. Each `'` in a
value is doubled, so put them inside a plain `'...'` literal of the outer query
(`{{ }}` and `{{{ }}}` render the same value); any other place is refused:

```sql
SELECT * FROM orders
WHERE owner = '{{{ auth.username }}}'
   OR list_contains(string_split('{{{ auth.roles }}}', ','), 'admin')
```

That is row filtering (empty result); use `auth.roles` for a 403. For MCP the same variables are
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
cors:
  allow-origins: [https://agent-console.example.com]   # never "*" with auth
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

- `mcp.auth.type: bearer` reads `jwt-secret`, `jwt-issuer` (no default: unset
  = not checked), `jwt-audience` and `jwt-require-exp`, the same check as REST. `type: oidc` reads `mcp.auth.oidc` (same
  keys as above).
- Every MCP method needs auth, `initialize` included; `server/discover` is
  always public. Relax one method with `mcp.auth.methods.<method>.required: false`
  (e.g. a `methods:` map under `mcp.auth` with key `tools/list` and child
  `required: false`, letting clients list tools anonymously).
- `allowed-roles` also exists on `mcp-resource` and `mcp-prompt`.
- Endpoint-level `auth:` does not apply to the MCP side of a dual REST+MCP file.
- **The plugin's MCP entry with `mcp.auth` on**: export `FLAPI_MCP_TOKEN` (a JWT as
  above for `type: bearer`, an access token for `oidc`); it goes in `Authorization:
  Bearer`, and the config token separately in `X-Config-Token`. Unset, every call is
  401. For `type: basic` register the server yourself with a Basic header (`flapi-mcp`).

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
`--validate-config`; they never stop startup. (An unknown `auth.type` is not a warning but a
load error, `AUTH_UNKNOWN_TYPE`, see above.)

| Code | Meaning |
|---|---|
| `AUTH_PLAINTEXT_PASSWORD` | a user password is not a hash; the message prints the passlib PBKDF2 one-liner |
| `AUTH_MD5_PASSWORD` | 32-hex MD5 password: unsalted, migrate to PBKDF2 |
| `AUTH_BCRYPT_UNSUPPORTED` | bcrypt hash: flAPI cannot verify it, every login as that user is refused |
| `AUTH_EMPTY_JWT_SECRET` | bearer endpoint, global block or `mcp.auth` with empty `jwt-secret`: fails closed, every token is refused |
| `AUTH_JWT_ISSUER_UNCHECKED` | bearer endpoint, global block or `mcp.auth` without `jwt-issuer`: any token signed with the secret is accepted, whoever issued it |
| `CORS_WILDCARD_WITH_AUTH` | `cors.allow-origins` empty or `"*"` while any auth is enabled |
| `MCP_UNAUTHENTICATED_TOOLS` | `mcp.enabled` with tools and `mcp.auth.enabled: false` |
| `CREDENTIAL_TOO_SHORT_TO_REDACT` | a credential connection property under 4 chars: error detail is withheld |

## Canonical auth setup

1. Pick the gate: REST endpoint `auth:`, `mcp.auth`, or both (they are separate).
2. Put secrets in env vars matching `template.environment-whitelist`; hash
   Basic passwords with the PBKDF2 one-liner.
3. Put one `auth:` block in `flapi.yaml` (every endpoint inherits it), opt
   public endpoints out with `auth: {enabled: false}`, add `roles:` where only
   some users may call; for MCP add `allowed-roles` to every tool.
4. `flapi -c flapi.yaml --validate-config` and read `SECURITY WARNINGS`.
5. Prove each path with curl: no credentials -> 401, wrong credentials -> 401,
   right credentials -> 200, a user without the role -> 403 (REST and MCP).
6. Lock down CORS, terminate TLS (`enforce-https` or a proxy), and run with a
   strong `FLAPI_CONFIG_SERVICE_TOKEN` or without `--config-service`.
