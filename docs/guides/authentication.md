# Authentication and authorization

**Goal:** decide who may call which endpoint or MCP tool, and which rows each
caller gets back.

flAPI checks a caller in three places, and each has its own configuration:

| Question | Where you answer it |
|---|---|
| Who is calling? (authentication) | an `auth:` block: `basic`, `bearer` (alias `jwt`) or `oidc` |
| May they call this endpoint? (authorization) | `roles:` in that block: `401` without valid credentials, `403` without a listed role |
| Which rows may they see? | the SQL template, using `auth.username` and `auth.roles` |

MCP tools are a separate door with a separate lock: `mcp.auth` and per-tool
`allowed-roles` (step 6). The REST `auth:` block does not protect them.

**Before you start:** you have a project with a working endpoint
([Getting started](./getting-started.md)). The examples use the sample
customer table:

```bash
mkdir -p data
curl -L -o data/customers.parquet https://github.com/DataZooDE/flapi/raw/main/examples/data/customers.parquet
```

---

<a id="the-quickest-thing-that-works"></a>

## 1. The quickest thing that works

Basic authentication with one user whose password comes from the environment.
`flapi.yaml` lets the endpoint read that one variable:

<!-- flapi: config group=quick env="READER_PASSWORD=s3cret-reader" allow=AUTH_PLAINTEXT_PASSWORD,CORS_WILDCARD_WITH_AUTH -->
```yaml
# flapi.yaml
project-name: shop
project-description: Customer data behind a login
template:
  path: ./sqls
  environment-whitelist:
    - '^READER_PASSWORD$'          # the only variable {{env.*}} may read
connections:
  customers:
    properties:
      path: ./data/customers.parquet
```

The endpoint gets an `auth:` block:

<!-- flapi: endpoint group=quick file=customers.yaml -->
```yaml
# sqls/customers.yaml
url-path: /customers
template-source: customers.sql
connection: [customers]
auth:
  enabled: true
  type: basic
  users:
    - username: reader
      password: '{{env.READER_PASSWORD}}'
      roles: [read]
```

<!-- flapi: template group=quick file=customers.sql -->
```sql
-- sqls/customers.sql
SELECT c_custkey, c_name, c_mktsegment
FROM read_parquet('{{{ conn.path }}}')
ORDER BY c_custkey
```
<!-- flapi: check group=quick GET /customers -> 401 contains "no Authorization header was sent" -->
<!-- flapi: check group=quick GET /customers?limit=2 user=reader:s3cret-reader -> 200 contains "Customer#000000002" -->
<!-- flapi: check group=quick GET /customers user=reader:wrong -> 401 contains "not accepted" -->

Start it with the password exported:

```bash
export READER_PASSWORD='s3cret-reader'
flapi -c flapi.yaml
```

Without credentials the answer is `401`, with a challenge naming the scheme:

```bash
curl -i http://localhost:8080/customers
```

```text
HTTP/1.1 401 Unauthorized
Content-Type: application/json
WWW-Authenticate: Basic realm="flAPI"
X-Request-Id: req-6088c2b6fe989082

{"message":"This endpoint requires authentication; no Authorization header was sent.","error":"Unauthorized"}
```

With them, the rows:

```bash
curl -u reader:s3cret-reader 'http://localhost:8080/customers?limit=2'
```

```json
{"data":[{"c_custkey":1,"c_name":"Customer#000000001","c_mktsegment":"BUILDING"},{"c_custkey":2,"c_name":"Customer#000000002","c_mktsegment":"AUTOMOBILE"}],"next":"/customers?offset=2&limit=2","total_count":1500}
```

A wrong password gets `401` with `"The credentials sent were not accepted."`.

`{{env.READER_PASSWORD}}` only resolves because the variable matches
`template.environment-whitelist`. An unlisted variable stops startup; a listed
one that is unset or empty stops it too (`auth.users: a user has an empty
username or password`). See [reusing config and reading the environment](./yaml-includes.md).

Startup also prints two security warnings. Steps 2 and 7 remove them:

```text
[AUTH_PLAINTEXT_PASSWORD] User 'reader' has a plaintext password. Store a PBKDF2-SHA256 hash instead, ... (at: endpoint GET /customers)
[CORS_WILDCARD_WITH_AUTH] CORS allows every origin (no cors.allow-origins set, which defaults to "*") while authentication is enabled. ... (at: cors)
```

## 2. Store password hashes, not passwords

A `password:` value is one of four things, recognised by its shape:

| Stored value | flAPI | Startup auditor |
|---|---|---|
| `$pbkdf2-sha256$<rounds>$<salt>$<hash>` | verifies it | silent: this is the format to use |
| 32 hex characters | treats it as an unsalted MD5 digest | `AUTH_MD5_PASSWORD` |
| `$2a$`, `$2b$`, `$2y$` (bcrypt) | **refuses every login** as that user | `AUTH_BCRYPT_UNSUPPORTED` |
| anything else | compares it as plaintext | `AUTH_PLAINTEXT_PASSWORD` |

Plaintext is readable by anyone who can read the config, the environment or a
process listing. MD5 has no salt and costs nothing to compute, so a leaked
digest is cracked in seconds. Both still log in, so an upgrade does not lock
anyone out, but both are deprecated. bcrypt is recognised and refused rather
than accepted unchecked.

flAPI has no hashing command. Use passlib, which writes the format flAPI
reads (the auditor's warning prints the same command):

```bash
pip install passlib
python3 -c "from passlib.hash import pbkdf2_sha256; import getpass; print(pbkdf2_sha256.using(rounds=600000).hash(getpass.getpass()))"
```

```text
Password:
$pbkdf2-sha256$600000$ozRmzJmTci5lTIkxptQ6Zw$MLAFJ2P.1Tf285q/gCCNFLbb/giQPB71ksNz79pRVVc
```

Paste that string as the password. Here three users, defined once in the
top-level `auth:` block of `flapi.yaml` so every endpoint shares them (step 4).
The `cors:` block is explained in step 7.

<!-- flapi: config group=shop -->
```yaml
# flapi.yaml
project-name: shop
project-description: Customer data behind a login
template:
  path: ./sqls
connections:
  customers:
    properties:
      path: ./data/customers.parquet
cors:
  allow-origins: ['https://app.example.com']
auth:
  enabled: true
  type: basic
  users:
    - username: alice                # password: wonderland
      password: '$pbkdf2-sha256$600000$ozRmzJmTci5lTIkxptQ6Zw$MLAFJ2P.1Tf285q/gCCNFLbb/giQPB71ksNz79pRVVc'
      roles: [admin]
    - username: bob                  # password: builder
      password: '$pbkdf2-sha256$600000$oxSC0LrXOocQotTamxOC8A$P1Vx2VzRfDfyN0vMeYUQDJMGY9UkAXekveTOV9rmjKY'
      roles: [sales, building, furniture]
    - username: carol                # password: sunshine
      password: '$pbkdf2-sha256$600000$KSVEaA2h9N5ba60V4jxnzA$KFV4K50ijplOcayCt7xitjyTNAEWAdZaxu6wtQoVb7U'
      roles: [sysadmin]
```

This config starts without a security warning. A hash is not the password,
but it can be attacked offline, so keep it out of public repositories; it can
come from `{{env.NAME}}` like a password.

A 600,000-round hash takes a noticeable fraction of a second to check. flAPI
remembers a successful check for the same user and password, so only the first
request pays it, and it caps how many checks run at once.

## 3. Choose a method

| Method | `type:` | Use it when | The caller sends |
|---|---|---|---|
| Basic | `basic` | a handful of known callers; users in YAML or in AWS Secrets Manager | `Authorization: Basic <base64 user:password>` |
| Bearer | `bearer` (alias `jwt`) | you mint HS256 tokens yourself with a shared secret | `Authorization: Bearer <jwt>` |
| OIDC | `oidc` | an identity provider (Entra ID, Keycloak, Auth0, Okta, Google) signs the tokens | `Authorization: Bearer <jwt>` |

One scheme applies per block. Keys the chosen type never reads (`users:` under
`bearer`, `jwt-secret:` under `basic`) are named in a startup warning. A
`type:` flAPI does not know (`basci`) stops startup with `AUTH_UNKNOWN_TYPE`.

There is no API-key header and no opaque shared-secret token: a bearer token
must be a JWT that verifies.

### Bearer (HS256 JWT)

```yaml
# sqls/reports.yaml (the auth: block)
auth:
  enabled: true
  type: bearer
  jwt-secret: '{{env.JWT_SECRET}}'           # whitelist JWT_SECRET
  jwt-issuer: https://auth.example.com
  roles: [analyst]
```

A token is accepted when all of this holds:

| Check | Fails with |
|---|---|
| signed with HS256 and `jwt-secret` | `401` |
| `iss` equals `jwt-issuer` exactly | `401` |
| `sub` is a string: it becomes `auth.username` | `401` |
| `roles` is an array of strings, possibly empty: it becomes `auth.roles` | `401` (a missing claim, or a single string) |
| `exp`, when the token has one, is in the future | `401` |
| one of the endpoint's `roles` is in the token | `403` |

`aud` is not checked, and a token without `exp` never expires, so mint tokens
with one. An empty `jwt-secret` (an unset variable) refuses every token and
the auditor reports `AUTH_EMPTY_JWT_SECRET`.

To try it, mint a token with PyJWT:

```bash
pip install pyjwt
export JWT_SECRET=$(openssl rand -hex 32)      # the same value the server reads
TOKEN=$(python3 -c "import jwt, os, time; print(jwt.encode({'sub': 'dana', 'iss': 'https://auth.example.com', 'roles': ['analyst'], 'exp': int(time.time()) + 3600}, os.environ['JWT_SECRET'], algorithm='HS256'))")
curl -H "Authorization: Bearer $TOKEN" http://localhost:8080/reports
```

A refused token gets `WWW-Authenticate: Bearer realm="flAPI", error="invalid_token"`;
no token at all gets the bare `Bearer realm="flAPI"`.

### OIDC

> **Known issue in v26.11.25: OIDC rejects every token, valid ones included**
> ([#273](https://github.com/DataZooDE/flapi/issues/273)). The server logs `JWT header missing 'alg' field`.
> Until a release fixes it, use Bearer (`jwt-secret`) or Basic. The configuration below is what a fixed
> release will accept.

flAPI validates tokens your identity provider issues. It fetches
`<issuer-url>/.well-known/openid-configuration` on the first request, then the
JWKS it names, and checks each token against them.

```yaml
# flapi.yaml (the top-level auth: block)
auth:
  enabled: true
  type: oidc
  oidc:
    provider-type: keycloak          # fills issuer-url, username and roles claims
    domain: kc.example.com
    realm: acme                      # issuer: https://kc.example.com/realms/acme
    client-id: flapi-api
    allowed-audiences: [flapi-api]
    email-claim: email
```

A token is accepted when:

- its header has a `kid` found in the JWKS and an `alg` of RS256, RS384 or
  RS512 (an unknown `kid` refetches the JWKS once; otherwise it is kept for
  `jwks-cache-hours`, default 24);
- `iss` equals `issuer-url` exactly, trailing slash included (Auth0 issuers end
  in `/`);
- `aud` holds one of `allowed-audiences`. **Without `allowed-audiences`, any
  token from that issuer is accepted**, including ones minted for other
  applications;
- `exp` is not older than `clock-skew-seconds` (default 300), unless
  `verify-expiration: false`;
- it has a `sub`.

Claims become template variables like this:

| Variable | Claim | Default |
|---|---|---|
| `auth.username` | `username-claim`, else `sub` | `sub`; the presets use `preferred_username` (Microsoft, Keycloak, Okta) or `email` (Google, Auth0) |
| `auth.email` | `email-claim` | `email` |
| `auth.roles` | `role-claim-path` if set, else `roles-claim` | `roles`; Keycloak's preset reads `realm_access.roles` |

A dot in `role-claim-path` or `roles-claim` means nesting (`realm_access.roles`).
`groups-claim` is read but does not grant roles: only the roles claim counts
for `roles:` and `allowed-roles`.

`provider-type` (`google`, `microsoft` with aliases `azure`, `azure-ad`,
`entra`, `keycloak`, `auth0`, `okta`, `github`, `generic`) fills in
`issuer-url` with placeholders: `{tenant}` from `tenant-id` (Microsoft),
`{domain}` and `{realm}` (Keycloak, Auth0, Okta). A placeholder left unfilled
stops startup with an error naming the key. flAPI only validates tokens:
`client-secret`, `scopes`, `enable-client-credentials` and
`enable-refresh-tokens` do not take part in it.

The full key list is in [CONFIG_REFERENCE § 7.4](../CONFIG_REFERENCE.md#74-oidc-authentication).

### Users from AWS Secrets Manager

The user list can live in a Secrets Manager secret instead of YAML. Only a
`type: basic` block reads it:

```yaml
# flapi.yaml (the top-level auth: block), or an endpoint's own block
auth:
  enabled: true
  type: basic
  from-aws-secretmanager:
    secret-name: prod/flapi/users
    region: eu-central-1               # else AWS_REGION, AWS_DEFAULT_REGION, the profile's region
```

The secret's value is a JSON object with an `auth` array. `username` and
`password` are required strings; `password` takes the formats of step 2:

```json
{"auth": [
  {"username": "alice", "password": "$pbkdf2-sha256$600000$...", "roles": ["admin"]},
  {"username": "bob",   "password": "$pbkdf2-sha256$600000$...", "roles": ["sales"]}
]}
```

- **AWS credentials:** `secret-id` and `secret-key` in the block (both or
  neither, e.g. from `{{env.NAME}}`), else the AWS SDK's default chain
  (environment, `AWS_PROFILE`, SSO, web identity, container or instance role).
  The deprecated `init:` creates a DuckDB S3 secret named after `secret-name`
  (`prod/flapi/users` becomes `prod_flapi_users`) in a private DuckDB.
- **When it is read:** once at startup per `secret-name`, however many
  endpoints share it. A failure stops startup. To rotate users, update the
  secret and restart flAPI.
- **Where it is kept:** in flAPI's memory. Neither the users nor the AWS
  credentials are in the DuckDB database templates run on, and neither is
  logged. `secret-table` from older configs is ignored with a warning (it
  has no effect), and tables an older version left in a file `db_path` are
  dropped at startup; if that file was in use, rotate those passwords.
- **Inline users win:** a block with `users:` never consults the secret.
- **Endpoint override:** `AWS_ENDPOINT_URL_SECRETS_MANAGER` (LocalStack, a VPC
  endpoint) is honoured with a startup warning; plain `http://` only to a
  loopback host or with `FLAPI_ALLOW_INSECURE_SECRETS_ENDPOINT=1`.
  `AWS_ENDPOINT_URL` is not used for it.

The rest is in [CONFIG_REFERENCE § 7.5](../CONFIG_REFERENCE.md#75-aws-secrets-manager).

## 4. Decide who may call what

With the top-level block of step 2, every endpoint **without an `auth:` block
of its own** requires one of those users. This endpoint has none:

<!-- flapi: endpoint group=shop file=customers.yaml -->
```yaml
# sqls/customers.yaml
url-path: /customers
template-source: customers.sql
connection: [customers]
```

<!-- flapi: template group=shop file=customers.sql -->
```sql
-- sqls/customers.sql
SELECT c_custkey, c_name, c_mktsegment
FROM read_parquet('{{{ conn.path }}}')
ORDER BY c_custkey
```
<!-- flapi: check group=shop GET /customers -> 401 -->
<!-- flapi: check group=shop GET /customers?limit=1 user=bob:builder -> 200 contains "Customer#000000001" -->

<a id="roles"></a>

**Roles.** An `auth:` block that names only `roles:` keeps the top-level users
and adds the requirement:

<!-- flapi: endpoint group=shop file=balances.yaml -->
```yaml
# sqls/balances.yaml
url-path: /admin/balances
template-source: balances.sql
connection: [customers]
auth:
  roles: [admin]                     # one of these, or 403
```

<!-- flapi: template group=shop file=balances.sql -->
```sql
-- sqls/balances.sql
SELECT round(sum(c_acctbal), 2) AS total_balance
FROM read_parquet('{{{ conn.path }}}')
```
<!-- flapi: check group=shop GET /admin/balances user=alice:wonderland -> 200 contains "total_balance" -->
<!-- flapi: check group=shop GET /admin/balances user=bob:builder -> 403 contains "does not hold any role" -->
<!-- flapi: check group=shop GET /admin/balances -> 401 -->

```bash
curl -s -u bob:builder http://localhost:8080/admin/balances
```

```json
{"message":"The authenticated principal does not hold any role this endpoint requires.","error":"Forbidden"}
```

`401` means "who are you?": no credentials, or wrong ones; retrying with
other credentials can help. `403` means "I know who you are, and no": the
caller authenticated but holds none of the listed roles. Any one listed role
is enough. An empty or missing `roles:` admits every authenticated caller.
Basic users get their roles from `users[].roles`, token callers from the token.

**Opting out.** One endpoint can stay public:

<!-- flapi: endpoint group=shop file=segments.yaml -->
```yaml
# sqls/segments.yaml
url-path: /segments
template-source: segments.sql
connection: [customers]
auth:
  enabled: false
```

<!-- flapi: template group=shop file=segments.sql -->
```sql
-- sqls/segments.sql
SELECT DISTINCT c_mktsegment AS segment
FROM read_parquet('{{{ conn.path }}}')
ORDER BY segment
```
<!-- flapi: check group=shop GET /segments -> 200 contains "MACHINERY" -->

The rules for an endpoint's `auth:` block:

| The endpoint has | Result |
|---|---|
| no `auth:` block | the top-level block applies, roles included (`inherited: true` in the config API) |
| `auth: {roles: [...]}` (no `type`, no credentials) | the top-level type and users, with these roles. Without an enabled top-level block: startup error `AUTH_NO_TYPE` |
| a `type:` or credentials (`users`, `jwt-secret`, `oidc`, `from-aws-secretmanager`) | **replaces** the top-level block; nothing is merged, so give it everything it needs. Credentials without `type:` are `AUTH_NO_TYPE` |
| `auth: {enabled: false}` | public, whatever the top level says |

A block without an `enabled:` key that sets `type`, `roles`, `users`,
`jwt-secret`, `jwt-issuer`, `oidc` or `from-aws-secretmanager` is **on**; only
`enabled: false` turns it off. Startup reports the outcome:

```text
Endpoint GET /admin/balances: `auth:` names no type or credentials, so it uses the top-level `auth:` block (type 'basic') with its own roles
Global auth (type 'basic') applies to 3 endpoint(s) without their own `auth:` block; 1 endpoint(s) opt out with `auth: {enabled: false}`
```

The top-level block covers endpoints created later through the config service
too. It covers REST only; MCP tools are step 6.

## 5. Filter rows by caller

After authentication the template sees who is calling:

| Variable | Holds | Filled for |
|---|---|---|
| `auth.username` | the user name, token `sub`, or OIDC `username-claim` | every authenticated request |
| `auth.roles` | the roles, **joined with commas**: `sales,building,furniture` | every authenticated request |
| `auth.email` | the OIDC `email-claim`; over MCP also a bearer token's `email` | OIDC, MCP bearer; empty otherwise |
| `auth.type` | `basic`, `bearer` or `oidc` | every authenticated request |
| `auth.authenticated` | `true` | every authenticated request |

On an anonymous request (an endpoint with `enabled: false`, or an MCP call with
`mcp.auth` off) every one of them is **empty**. A caller cannot set them: a
`__auth_roles=admin` query or tool argument is dropped.

Here admins see every customer and everyone else the segments named in their
roles:

<!-- flapi: endpoint group=shop file=my-customers.yaml -->
```yaml
# sqls/my-customers.yaml
url-path: /my-customers
template-source: my-customers.sql
connection: [customers]
```

<!-- flapi: template group=shop file=my-customers.sql -->
```sql
-- sqls/my-customers.sql
SELECT c_custkey, c_name, c_mktsegment
FROM read_parquet('{{{ conn.path }}}')
WHERE list_contains(string_split('{{{ auth.roles }}}', ','), 'admin')
   OR list_contains(string_split('{{{ auth.roles }}}', ','), lower(c_mktsegment))
ORDER BY c_custkey
```
<!-- flapi: check group=shop GET /my-customers?limit=1 user=alice:wonderland -> 200 contains ""total_count":1500" -->
<!-- flapi: check group=shop GET /my-customers?limit=1 user=bob:builder -> 200 contains ""total_count":616" -->
<!-- flapi: check group=shop GET /my-customers user=carol:sunshine -> 200 contains ""total_count":0" -->

```bash
curl -s -u bob:builder 'http://localhost:8080/my-customers?limit=2'
```

```json
{"data":[{"c_custkey":1,"c_name":"Customer#000000001","c_mktsegment":"BUILDING"},{"c_custkey":8,"c_name":"Customer#000000008","c_mktsegment":"BUILDING"}],"next":"/my-customers?offset=2&limit=2","total_count":616}
```

alice gets all 1500, bob the 616 BUILDING and FURNITURE customers, carol none.
Four rules make such a filter safe:

- **Split the roles, never `LIKE`.** `'{{{ auth.roles }}}' LIKE '%admin%'` is
  true for carol, whose only role is `sysadmin`, and for any role that merely
  contains the word (`admin-readonly`). `list_contains(string_split(...), 'admin')`
  matches whole role names only.
- **Fail closed.** Write the condition so that empty values match nothing, as
  above: an empty `auth.roles` splits into `['']`, which contains no role. A
  filter that is only applied *when* a value is present
  (`{{#auth.username}}WHERE owner = ...{{/auth.username}}`) shows every row to
  an anonymous caller.
- **`auth.*` is text in the SQL.** Unlike `{{ params.x }}` it is never bound
  as a prepared-statement parameter; the value is pasted into the query. Put
  it inside single quotes with triple braces, `'{{{ auth.username }}}'`, and
  trust it as far as you trust its source: your own `users:`, the holder of
  `jwt-secret`, or your identity provider.
- **The endpoint's `roles:` decides whether the query runs; the template
  decides which rows it returns.** Use both: a `roles:` list keeps strangers
  out with a `403` before any SQL runs.

To scope by user instead of role, compare a column with the user name:

```sql
WHERE account_owner = '{{{ auth.username }}}'
```

## 6. Protect MCP tools

MCP requests (`POST /mcp/jsonrpc`) never pass the REST `auth:` check, whether
it is the endpoint's own block or the top-level one. A tool is protected only by
`mcp.auth` and its own `allowed-roles`. If `mcp.auth` is off, every tool runs
for anyone who reaches the server, with empty `auth.*` (the startup auditor
says so: `MCP_UNAUTHENTICATED_TOOLS`).

`mcp.auth` has its own users (or `jwt-secret`, or `oidc` block), separate from
the REST ones:

<!-- flapi: config group=agents -->
```yaml
# flapi.yaml
project-name: shop-agents
project-description: Customer tools for agents
template:
  path: ./sqls
connections:
  customers:
    properties:
      path: ./data/customers.parquet
cors:
  allow-origins: ['https://app.example.com']
mcp:
  auth:
    enabled: true
    type: basic                      # default: bearer
    users:
      - username: alice
        password: '$pbkdf2-sha256$600000$ozRmzJmTci5lTIkxptQ6Zw$MLAFJ2P.1Tf285q/gCCNFLbb/giQPB71ksNz79pRVVc'
        roles: [admin]
      - username: bob
        password: '$pbkdf2-sha256$600000$oxSC0LrXOocQotTamxOC8A$P1Vx2VzRfDfyN0vMeYUQDJMGY9UkAXekveTOV9rmjKY'
        roles: [sales, building, furniture]
      - username: carol
        password: '$pbkdf2-sha256$600000$KSVEaA2h9N5ba60V4jxnzA$KFV4K50ijplOcayCt7xitjyTNAEWAdZaxu6wtQoVb7U'
        roles: [sysadmin]
```

With `mcp.auth` enabled, **a tool without `allowed-roles` is refused to
everyone**. List the roles that may call it:

<!-- flapi: endpoint group=agents file=segment-balances.yaml -->
```yaml
# sqls/segment-balances.yaml
url-path: /segment-balances
template-source: segment-balances.sql
connection: [customers]
mcp-tool:
  name: segment_balances
  description: Customer count and balance per market segment the caller may see
  allowed-roles: [admin, sales]
```

The template filters exactly like step 5; `auth.*` holds the MCP caller:

<!-- flapi: template group=agents file=segment-balances.sql -->
```sql
-- sqls/segment-balances.sql
SELECT c_mktsegment AS segment,
       count(*) AS customers,
       round(sum(c_acctbal), 2) AS balance
FROM read_parquet('{{{ conn.path }}}')
WHERE list_contains(string_split('{{{ auth.roles }}}', ','), 'admin')
   OR list_contains(string_split('{{{ auth.roles }}}', ','), lower(c_mktsegment))
GROUP BY c_mktsegment
ORDER BY segment
```
<!-- flapi: check-mcp group=agents segment_balances -> 401 -->
<!-- flapi: check-mcp group=agents segment_balances user=alice:wonderland -> ok contains ""row_count":5" -->
<!-- flapi: check-mcp group=agents segment_balances user=bob:builder -> ok contains ""row_count":2" -->
<!-- flapi: check-mcp group=agents segment_balances user=carol:sunshine -> 403 contains "requires one of [admin, sales]" -->
<!-- flapi: check group=agents GET /segment-balances -> 200 contains ""total_count":0" -->

| Caller | `tools/call segment_balances` |
|---|---|
| no credentials | HTTP `401`, `"Authentication required for method: tools/call"` |
| alice (`admin`) | five segments |
| bob (`sales`) | BUILDING and FURNITURE only |
| carol (`sysadmin`) | HTTP `403`, `"Permission denied: Tool 'segment_balances' requires one of [admin, sales]; caller has [sysadmin]."` |

The same file is also a REST endpoint, and this config has no REST `auth:`:
`GET /segment-balances` is public and returns `"total_count":0`, because its
anonymous caller has no roles and the template fails closed. Add a top-level
`auth:` block (step 4) or remove `url-path` if the tool should be the only way
in.

`mcp.auth.methods` relaxes single methods, e.g. `tools/list: {required: false}`;
every other method requires authentication. See
[MCP tools](./mcp-tools.md) and [MCP_REFERENCE § 8](../MCP_REFERENCE.md).

## 7. CORS, rate limits and the audit log

**CORS.** Without a `cors:` block, every origin is allowed: a page on any site
can call the API from a visitor's browser with a credential it holds and read
the answer. The auditor warns `CORS_WILDCARD_WITH_AUTH` whenever that meets
any authentication. Step 2's config lists the one origin that may call:

```yaml
cors:
  allow-origins: ['https://app.example.com']
```

Preflights (`OPTIONS` with `Access-Control-Request-Method`) from that origin
get the `Access-Control-Allow-*` headers on REST paths and `/mcp/jsonrpc`, and
the requested headers are echoed, so `Authorization` passes. Other origins get
no `Access-Control-Allow-Origin`. Details: [CONFIG_REFERENCE § 2.9.1](../CONFIG_REFERENCE.md#291-cors).

**Rate limits per caller.** `key: user` gives every caller a bucket of its own:

<!-- flapi: endpoint group=shop file=export.yaml -->
```yaml
# sqls/export.yaml
url-path: /export
template-source: my-customers.sql
connection: [customers]
rate-limit:
  enabled: true
  max: 2                             # requests per caller ...
  interval: 60                       # ... per 60 seconds
  key: user                          # ip (default) | user | user-or-ip
```
<!-- flapi: check group=shop GET /export?limit=1 user=bob:builder -> 200 -->
<!-- flapi: check group=shop GET /export?limit=1 user=bob:builder -> 200 -->
<!-- flapi: check group=shop GET /export?limit=1 user=bob:builder -> 429 contains "Rate limit exceeded" -->
<!-- flapi: check group=shop GET /export?limit=1 user=alice:wonderland -> 200 -->

bob's third request within a minute gets `429` with `Retry-After`; alice still
gets `200`. The limit runs **before** authentication, so the bucket is the
`Authorization` header as sent, and requests with wrong credentials count too.
Anonymous requests share one bucket under `user`; `user-or-ip` gives them
their address's bucket instead. Every request also counts against its IP with a
ceiling of ten times `max`, so rotating made-up credentials does not escape the
limit. The global block is `rate_limit:` (underscore) in `flapi.yaml`;
[CONFIG_REFERENCE § 2.8](../CONFIG_REFERENCE.md#28-rate-limiting-global).

**Audit log.** `audit: {enabled: true}` writes one JSON line per request, with
the principal (`anonymous` for a `401`), route, status and `request_id`. It is
off by default. See [Observability](./observability.md).

## 8. Test it

**With curl**, one request per outcome:

```bash
curl -i http://localhost:8080/customers                            # 401: no credentials
curl -i -u bob:wrong http://localhost:8080/customers               # 401: refused
curl -i -u bob:builder http://localhost:8080/admin/balances        # 403: no admin role
curl -s -u alice:wonderland http://localhost:8080/admin/balances   # 200
```

For an MCP tool, send the same `Authorization` header with the JSON-RPC body:

```bash
curl -s -u alice:wonderland http://localhost:8080/mcp/jsonrpc \
  -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"segment_balances","arguments":{}}}'
```

**With flapii.** `flapii` talks to the config service, which has its own
token (`flapi --config-service --config-service-token <token>`), not to your
endpoints. It shows how each endpoint ended up protected:

```bash
flapii -t "$FLAPI_CONFIG_SERVICE_TOKEN" endpoints get /customers
```

```json
"auth": {
  "inherited": true,
  "roles": [],
  "type": "basic",
  "enabled": true
}
```

`inherited: true` means the top-level block applies. `/segments` shows
`"enabled": false`, `/admin/balances` `"roles": ["admin"]`. Users and secrets
are never shown. `flapii metrics -o table` counts the rejections per
endpoint:

```text
2 requests, 0 server errors, 0 cache-not-ready 503s · rejected: 1 × 401, 1 × 403, 0 × 429
```

`flapi --validate-config` prints the security warnings without starting the
server.

## Common mistakes

- **`'{{auth.roles}}' LIKE '%admin%'`.** Matches `sysadmin`, `admin-readonly`
  and any role containing the word. Use
  `list_contains(string_split('{{{ auth.roles }}}', ','), 'admin')`.
- **A filter that disappears for anonymous callers.** `{{#auth.username}}`
  sections render nothing when the value is empty. Write conditions that match
  no row when `auth.*` is empty.
- **Expecting REST `auth:` to protect a tool.** It does not. Enable `mcp.auth`
  and give every tool `allowed-roles`.
- **An endpoint block with `type:` but no users.** It replaces the top-level
  block, users included, and every login fails. Name only `roles:` to keep the
  top-level users.
- **Bearer without `jwt-issuer`.** The token's `iss` must then be the empty
  string, so ordinary tokens get `401`. Always set `jwt-issuer`.
- **A token without a `roles` array.** REST bearer refuses it with `401`; send
  `"roles": []` when the caller has none.
- **OIDC without `allowed-audiences`.** Any token from the issuer passes,
  including tokens for other applications.
- **An OIDC `issuer-url` that differs from `iss` by a trailing slash.** The
  comparison is exact.
- **A 32-character hex password.** It is taken for an MD5 digest, so the
  password itself no longer logs in. Hash it.
- **A bcrypt hash.** Every login as that user is refused. Re-hash with PBKDF2.
- **`${VAR}` in a password.** Only `{{env.VAR}}` is expanded, and only for
  whitelisted names; `${VAR}` is taken literally (and as a plaintext password).
- **`rate-limit:` without `enabled: true`.** A per-endpoint block is off unless
  it says `enabled: true`.

## Next

- [MCP tools](./mcp-tools.md): exposing endpoints to agents
- [Observability](./observability.md): audit log, request IDs, metrics
- [CONFIG_REFERENCE § 7](../CONFIG_REFERENCE.md#7-authentication): every `auth:` key

## How it works

Every REST request passes the middleware chain: CORS, then rate limiting, then
authentication, then the handler. The auth middleware looks up the endpoint by
path and method; if its `auth` is enabled it verifies the `Authorization`
header against the endpoint's scheme, then checks `roles`. On failure it writes
the `401` or `403` and ends the request: no SQL runs. On success it stores user
name, roles, email and scheme on the request, and the handler injects them as
`auth.*` into the template context after removing any `__auth_*` value the
caller sent.

`POST /mcp/jsonrpc` is a route of its own: the REST auth middleware finds no
endpoint for it and lets it through. The MCP handler authenticates against
`mcp.auth` on every call (a session id is never a credential), applies
`allowed-roles` per tool, resource and prompt, and injects the MCP caller as
`auth.*`.

The startup auditor (`src/security_auditor.cpp`) runs on the loaded config and
prints the `SECURITY WARNINGS` banner; it never stops startup.

- [spec/components/security.md](../spec/components/security.md): the layered defences
- [spec/REQUEST_LIFECYCLE.md](../spec/REQUEST_LIFECYCLE.md): where auth sits in the chain
- Source: `src/auth_middleware.cpp`, `src/oidc_auth_handler.cpp`,
  `src/password_hasher.cpp`, `src/mcp_auth_handler.cpp`,
  `src/mcp_authorization_policy.cpp`
