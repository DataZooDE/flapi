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
`allowed-roles` (step 5). The REST `auth:` block does not protect them.

**Words used below**

| Term | Meaning |
|---|---|
| principal | the authenticated caller: a user name or a token's subject |
| challenge | the `WWW-Authenticate` header of a `401`, naming the scheme to use |
| realm | the name in a challenge; flAPI always sends `realm="flAPI"` |
| preflight | the `OPTIONS` request a browser sends before a cross-origin call |
| `sub`, `iss`, `aud`, `exp`, `nbf` | token claims: subject, issuer, audience, expiry, not-before |
| JWKS, `kid` | the identity provider's published public keys, and the id in a token naming the key that signed it |
| startup auditor | the check that prints `SECURITY WARNINGS` at startup and in `--validate-config`; it never stops startup |
| config service | flAPI's admin API (`--config-service`), used by `flapii` |
| MCP tool | an endpoint exposed to AI agents over `POST /mcp/jsonrpc` |

**Before you start:** you have flAPI installed ([Getting started](./getting-started.md)).
The guide builds three small projects, each in its own directory:

| Project | Steps |
|---|---|
| `shop` | 1-4 and 6: users, roles, row filters, CORS, rate limits |
| `shop-agents` | 5: MCP tools |
| `reports-jwt` | 7: Bearer tokens |

Start the first one with the sample customer table:

```bash
mkdir -p shop/data shop/sqls && cd shop
curl -L -o data/customers.parquet https://github.com/DataZooDE/flapi/raw/main/examples/data/customers.parquet
```

flAPI does not watch its files: after every change to a config or endpoint
file, **restart it** (Ctrl-C, then the same `flapi` command).

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

Start it with the password exported. `--host 127.0.0.1` keeps a password demo
off the network:

```bash
export READER_PASSWORD='s3cret-reader'
flapi -c flapi.yaml --host 127.0.0.1
```

In a second terminal, call it without credentials. The answer is `401`, with a
challenge naming the scheme:

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

With credentials, the rows:

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

Startup also prints two security warnings. Step 2 removes both:

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

flAPI has no hashing command. passlib writes the format flAPI reads; `uvx`
runs it without installing anything:

```bash
uvx --with passlib python -c "from passlib.hash import pbkdf2_sha256; import getpass; print(pbkdf2_sha256.using(rounds=600000).hash(getpass.getpass()))"
```

```text
Password:
$pbkdf2-sha256$600000$ozRmzJmTci5lTIkxptQ6Zw$MLAFJ2P.1Tf285q/gCCNFLbb/giQPB71ksNz79pRVVc
```

Paste that string as the password. Here three users, defined once in the
top-level `auth:` block so every endpoint shares them (step 3). Replace
`flapi.yaml`:

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
  allow-origins: ['https://app.example.com']   # step 6
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

Replace `sqls/customers.yaml` too. An endpoint's own `auth:` block would
replace the top-level one, and the old one still reads `READER_PASSWORD`,
which this config no longer whitelists:

<!-- flapi: endpoint group=shop file=customers.yaml -->
```yaml
# sqls/customers.yaml
url-path: /customers
template-source: customers.sql
connection: [customers]
```

<!-- flapi: template group=shop file=customers.sql -->
```sql
-- sqls/customers.sql (unchanged)
SELECT c_custkey, c_name, c_mktsegment
FROM read_parquet('{{{ conn.path }}}')
ORDER BY c_custkey
```
<!-- flapi: check group=shop GET /customers -> 401 -->
<!-- flapi: check group=shop GET /customers?limit=1 user=bob:builder -> 200 contains "Customer#000000001" -->

Restart flAPI. It starts without a security warning, and `/customers` now
takes any of the three users:

```bash
curl -s -u bob:builder 'http://localhost:8080/customers?limit=1'
```

A hash is not the password, but it can be attacked offline, so keep it out of
public repositories; it can come from `{{env.NAME}}` like a password.

A 600,000-round hash takes a noticeable fraction of a second to check. flAPI
remembers a successful check for the same user and password, so only the first
request pays it, and it caps how many checks run at once.

## 3. Decide who may call what

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

Restart flAPI, then:

```bash
curl -s -u alice:wonderland http://localhost:8080/admin/balances
curl -s -u bob:builder http://localhost:8080/admin/balances
```

```json
{"data":[{"total_balance":6681865.59}],"next":"","total_count":1}
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

Restart flAPI; no credentials needed:

```bash
curl -s http://localhost:8080/segments
```

The rules for an endpoint's `auth:` block:

| The endpoint has | Result |
|---|---|
| no `auth:` block | the top-level block applies, roles included (`inherited: true` in the config API) |
| `auth: {roles: [...]}` (no `type`, no credentials) | the top-level type and users, with these roles. Without an enabled top-level block: startup error `AUTH_NO_TYPE` |
| a `type:` or credentials (`users`, `jwt-secret`, `jwt-issuer`, `oidc`, `from-aws-secretmanager`) | **replaces** the top-level block; nothing is merged, so give it everything it needs. Credentials without `type:` are `AUTH_NO_TYPE` |
| `auth: {enabled: false}` | public, whatever the top level says |

A block without an `enabled:` key that sets `type`, `roles`, `users`,
`jwt-secret`, `jwt-issuer`, `oidc` or `from-aws-secretmanager` is **on**; only
`enabled: false` turns it off. At this point startup reports:

```text
Endpoint GET /admin/balances: `auth:` names no type or credentials, so it uses the top-level `auth:` block (type 'basic') with its own roles
Global auth (type 'basic') applies to 1 endpoint(s) without their own `auth:` block; 1 endpoint(s) opt out with `auth: {enabled: false}`
```

The top-level block covers endpoints created later through the config service
too. It covers REST only; MCP tools are step 5.

## 4. Filter rows by caller

After authentication the template sees who is calling:

| Variable | Holds | Filled for |
|---|---|---|
| `auth.username` | the user name, token `sub`, or OIDC `username-claim` | every authenticated request |
| `auth.roles` | the roles, **joined with commas**: `sales,building,furniture` | every authenticated request |
| `auth.email` | the OIDC `email-claim`; over MCP also a bearer token's `email` | OIDC, MCP bearer; empty otherwise |
| `auth.type` | `basic`, `bearer` or `oidc` | every authenticated request |
| `auth.authenticated` | `true` | every authenticated request |

No other claim reaches the template. On an anonymous request (an endpoint with
`enabled: false`, or an MCP call with `mcp.auth` off) every one of them is
**empty**. A caller cannot set them: a `__auth_roles=admin` query or tool
argument is dropped.

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

Restart flAPI, then:

```bash
curl -s -u bob:builder 'http://localhost:8080/my-customers?limit=2'
```

```json
{"data":[{"c_custkey":1,"c_name":"Customer#000000001","c_mktsegment":"BUILDING"},{"c_custkey":8,"c_name":"Customer#000000008","c_mktsegment":"BUILDING"}],"next":"/my-customers?offset=2&limit=2","total_count":616}
```

alice gets all 1500, bob the 616 BUILDING and FURNITURE customers, carol none.
Five rules make such a filter safe:

- **Split the roles, never `LIKE`.** `'{{{ auth.roles }}}' LIKE '%admin%'` is
  true for carol, whose only role is `sysadmin`, and for any role that merely
  contains the word (`admin-readonly`). `list_contains(string_split(...), 'admin')`
  matches whole role names only. Role names must not contain commas (an
  identity with such a role gets `401`).
- **Fail closed.** Write the condition so that empty values match nothing, as
  above: an empty `auth.roles` splits into `['']`, which contains no role. A
  filter that is only applied *when* a value is present
  (`{{#auth.username}}WHERE owner = ...{{/auth.username}}`) shows every row to
  an anonymous caller.
- **`auth.*` is text, not a bound parameter.** Unlike `{{ params.x }}` it is
  never a prepared-statement parameter. flAPI doubles every single quote in it,
  which is safe in exactly one place: a plain `'...'` literal of the outer
  query, `'{{{ auth.username }}}'`. Any other place (unquoted, a comment, an
  `E'...'` string, a `"..."` identifier, a dollar-quoted string, or SQL text that
  `query()` or a `*_query` function runs) stops startup and `--validate-config`,
  and is refused at request time. An identity holding a control character, or a
  role containing a comma, gets `401`. Use only values the caller cannot edit: your own `users:`, a token you signed, or an identity
  provider's immutable subject (`sub` or an object id). Do not filter on
  `preferred_username` or `email` from an identity provider where users can
  change their own profile.
- **Scope by user** with a column compare:
  `WHERE account_owner = '{{{ auth.username }}}'`.
- **The endpoint's `roles:` decides whether the query runs; the template
  decides which rows it returns.** Use both: a `roles:` list keeps strangers
  out with a `403` before any SQL runs.

## 5. Protect MCP tools

MCP requests (`POST /mcp/jsonrpc`) never pass the REST `auth:` check, whether
it is the endpoint's own block or the top-level one. A tool is protected only
by `mcp.auth` and its own `allowed-roles`.

### The leak

Make a new project next to `shop`:

```bash
cd .. && mkdir -p shop-agents/sqls && cp -r shop/data shop-agents/ && cd shop-agents
```

Start it with REST authentication only. alice is the one user:

<!-- flapi: config group=leak allow=MCP_UNAUTHENTICATED_TOOLS -->
```yaml
# flapi.yaml (first version)
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
auth:
  enabled: true
  type: basic
  users:
    - username: alice                # password: wonderland
      password: '$pbkdf2-sha256$600000$ozRmzJmTci5lTIkxptQ6Zw$MLAFJ2P.1Tf285q/gCCNFLbb/giQPB71ksNz79pRVVc'
      roles: [admin]
```

and one endpoint that is also a tool:

<!-- flapi: endpoint group=leak file=customer-count.yaml -->
```yaml
# sqls/customer-count.yaml (first version)
url-path: /customer-count
template-source: customer-count.sql
connection: [customers]
mcp-tool:
  name: customer_count
  description: Number of customers
```

<!-- flapi: template group=leak file=customer-count.sql -->
```sql
-- sqls/customer-count.sql
SELECT count(*) AS customers
FROM read_parquet('{{{ conn.path }}}')
```
<!-- flapi: check group=leak GET /customer-count -> 401 -->
<!-- flapi: check group=leak GET /customer-count user=alice:wonderland -> 200 contains ""customers":1500" -->
<!-- flapi: check-mcp group=leak customer_count -> ok contains ""customers":1500" -->

Startup warns `MCP_UNAUTHENTICATED_TOOLS`, and it means it. REST refuses an
anonymous caller, the tool does not:

```bash
curl -s http://localhost:8080/customer-count
curl -s http://localhost:8080/mcp/jsonrpc -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"customer_count","arguments":{}}}'
```

```text
{"message":"This endpoint requires authentication; no Authorization header was sent.","error":"Unauthorized"}
{"jsonrpc":"2.0","id":1,"result":{"content":[{"text":"[{\"customers\":1500}]","type":"text"}],"structuredContent":{"rows":[{"customers":1500}],"row_count":1}}}
```

### The fix: `mcp.auth` and `allowed-roles`

Add an `mcp:` block. `mcp.auth` has its own user list (or `jwt-secret`, or
`oidc` block); a YAML anchor (`&users`, `*users`) reuses the REST one. Replace
`flapi.yaml`:

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
auth:
  enabled: true
  type: basic
  users: &users
    - username: alice                # password: wonderland
      password: '$pbkdf2-sha256$600000$ozRmzJmTci5lTIkxptQ6Zw$MLAFJ2P.1Tf285q/gCCNFLbb/giQPB71ksNz79pRVVc'
      roles: [admin]
    - username: bob                  # password: builder
      password: '$pbkdf2-sha256$600000$oxSC0LrXOocQotTamxOC8A$P1Vx2VzRfDfyN0vMeYUQDJMGY9UkAXekveTOV9rmjKY'
      roles: [sales, building, furniture]
    - username: carol                # password: sunshine
      password: '$pbkdf2-sha256$600000$KSVEaA2h9N5ba60V4jxnzA$KFV4K50ijplOcayCt7xitjyTNAEWAdZaxu6wtQoVb7U'
      roles: [sysadmin]
mcp:
  auth:
    enabled: true
    type: basic                      # default: bearer
    users: *users
```

With `mcp.auth` enabled, **a tool without `allowed-roles` is refused to
everyone** (`403`, "has no allowed-roles configured"). Give the tool its roles:

<!-- flapi: endpoint group=agents file=customer-count.yaml -->
```yaml
# sqls/customer-count.yaml
url-path: /customer-count
template-source: customer-count.sql
connection: [customers]
mcp-tool:
  name: customer_count
  description: Number of customers
  allowed-roles: [admin]
```

<!-- flapi: template group=agents file=customer-count.sql -->
```sql
-- sqls/customer-count.sql (unchanged)
SELECT count(*) AS customers
FROM read_parquet('{{{ conn.path }}}')
```

and add a second tool whose template filters exactly like step 4; `auth.*`
holds the MCP caller:

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
<!-- flapi: check-mcp group=agents customer_count -> 401 -->
<!-- flapi: check-mcp group=agents customer_count user=alice:wonderland -> ok contains ""customers":1500" -->
<!-- flapi: check-mcp group=agents segment_balances -> 401 -->
<!-- flapi: check-mcp group=agents segment_balances user=alice:wonderland -> ok contains ""row_count":5" -->
<!-- flapi: check-mcp group=agents segment_balances user=bob:builder -> ok contains ""row_count":2" -->
<!-- flapi: check-mcp group=agents segment_balances user=carol:sunshine -> 403 contains "requires one of [admin, sales]" -->
<!-- flapi: check group=agents GET /segment-balances user=bob:builder -> 200 contains "FURNITURE" -->

Restart flAPI and call the tool as each user (drop `-u` for the anonymous
call):

```bash
curl -s -u bob:builder http://localhost:8080/mcp/jsonrpc -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"segment_balances","arguments":{}}}'
curl -s -u bob:builder http://localhost:8080/segment-balances
```

| Caller | `tools/call segment_balances` |
|---|---|
| no credentials | HTTP `401`, `"Authentication required for method: tools/call"` |
| alice (`admin`) | five segments |
| bob (`sales`) | BUILDING and FURNITURE only |
| carol (`sysadmin`) | HTTP `403`, `"Permission denied: Tool 'segment_balances' requires one of [admin, sales]; caller has [sysadmin]."` |

The same endpoint answers REST too, through the top-level `auth:`: bob gets the
same two segments from `GET /segment-balances`. Remove `url-path` if the tool
should be the only way in.

The `401` names the scheme `mcp.auth.type` accepts: `WWW-Authenticate: Basic realm="flAPI"`
for `basic`, `Bearer` for `bearer` and `oidc`. A `403` under `basic` has no
`WWW-Authenticate`; under `bearer` it says `error="insufficient_scope"`.

`mcp.auth.methods` relaxes single methods, e.g. `tools/list: {required: false}`;
every other method requires authentication, except `server/discover`, which is
always public. See [MCP tools](./mcp-tools.md) and
[MCP_REFERENCE § 8](../MCP_REFERENCE.md).

## 6. CORS, rate limits and the audit log

Back in the `shop` project.

**CORS.** Without a `cors:` block, every origin is allowed: a page on any site
can call the API from a visitor's browser with a credential it holds and read
the answer. The auditor warns `CORS_WILDCARD_WITH_AUTH` whenever that meets
any authentication. Step 2's config lists the one origin that may call:

```yaml
cors:
  allow-origins: ['https://app.example.com']
```

Preflights (`OPTIONS` with `Access-Control-Request-Method`) from that origin
get the `Access-Control-Allow-*` headers on REST paths and `/mcp/jsonrpc`.
While `cors.allow-headers` is unset (or contains `*`) the requested headers are
echoed, so `Authorization` passes; a list of your own must name it. Other
origins get no `Access-Control-Allow-Origin`. Details:
[CONFIG_REFERENCE § 2.9.1](../CONFIG_REFERENCE.md#291-cors).

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
  key: user                          # ip | user | user-or-ip
```
<!-- flapi: check group=shop GET /export?limit=1 user=bob:builder -> 200 -->
<!-- flapi: check group=shop GET /export?limit=1 user=bob:builder -> 200 -->
<!-- flapi: check group=shop GET /export?limit=1 user=bob:builder -> 429 contains "Rate limit exceeded" -->
<!-- flapi: check group=shop GET /export?limit=1 user=alice:wonderland -> 200 -->

Restart flAPI and call it three times as bob, then once as alice:

```bash
for i in 1 2 3; do curl -s -o /dev/null -w '%{http_code}\n' -u bob:builder 'http://localhost:8080/export?limit=1'; done
curl -s -o /dev/null -w '%{http_code}\n' -u alice:wonderland 'http://localhost:8080/export?limit=1'
```

```text
200
200
429
200
```

The `429` carries `Retry-After`. The limit runs **before** authentication, so
the bucket is the `Authorization` header as sent, and requests with wrong
credentials count too. Anonymous requests share one bucket under `user`;
`user-or-ip` gives them their address's bucket instead. Every request also
counts against its IP with a ceiling of ten times `max`, so rotating made-up
credentials does not escape the limit. `key` defaults to the global
`rate_limit.key`, else `ip`. The global block is `rate_limit:` (underscore) in
`flapi.yaml`; [CONFIG_REFERENCE § 2.8](../CONFIG_REFERENCE.md#28-rate-limiting-global).
REST limits do not apply to `/mcp/jsonrpc`; tools take `mcp-tool.rate-limit`.

**Audit log.** `audit: {enabled: true}` writes one JSON line per request (off
by default). `principal` is the user, `anonymous` for a request without
credentials, and a bearer token's `sub` even when it was then refused for
missing roles. `status` is `success`, `denied`, `rate_limited` or `error:NNN`;
a `429` line has `target` `<unmatched>`, because the limit fires before routing
is recorded. See [Observability](./observability.md).

## 7. Choose another method

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

A project of its own:

```bash
cd .. && mkdir -p reports-jwt/sqls && cp -r shop/data reports-jwt/ && cd reports-jwt
```

```yaml
# flapi.yaml
project-name: reports
project-description: Reports for token holders
template:
  path: ./sqls
  environment-whitelist:
    - '^JWT_SECRET$'
connections:
  customers:
    properties:
      path: ./data/customers.parquet
cors:
  allow-origins: ['https://app.example.com']
```

```yaml
# sqls/reports.yaml
url-path: /reports
template-source: reports.sql
connection: [customers]
auth:
  enabled: true
  type: bearer
  jwt-secret: '{{env.JWT_SECRET}}'
  jwt-issuer: https://auth.example.com
  roles: [analyst]
```

```sql
-- sqls/reports.sql
SELECT '{{{ auth.username }}}' AS caller, count(*) AS customers
FROM read_parquet('{{{ conn.path }}}')
```

The secret is exported **before** the server starts. Print it once, so the
second terminal can mint tokens with the same value:

```bash
export JWT_SECRET=$(openssl rand -hex 32)
echo "$JWT_SECRET"
flapi -c flapi.yaml --host 127.0.0.1
```

In the second terminal, paste that value and mint a one-hour token with PyJWT:

```bash
export JWT_SECRET='<the value printed above>'
TOKEN=$(uvx --with pyjwt python -c "import jwt, os, time; print(jwt.encode({'sub': 'dana', 'iss': 'https://auth.example.com', 'roles': ['analyst'], 'exp': int(time.time()) + 3600}, os.environ['JWT_SECRET'], algorithm='HS256'))")
curl -s -H "Authorization: Bearer $TOKEN" http://localhost:8080/reports
```

```json
{"data":[{"caller":"dana","customers":1500}],"next":"","total_count":1}
```

A token is accepted when all of this holds:

| Check | Fails with |
|---|---|
| signed with HS256 and `jwt-secret` | `401` |
| `iss` equals `jwt-issuer` exactly | `401` |
| `sub` is a string: it becomes `auth.username` | `401` |
| `roles` is an array of strings, possibly empty: it becomes `auth.roles` | `401` (a missing claim, or a single string) |
| `exp`, when the token has one, is in the future | `401` |
| `nbf` and `iat`, when present, are not in the future | `401` |
| one of the endpoint's `roles` is in the token | `403` |

`aud` is not checked, and a token without `exp` never expires, so mint tokens
with one. An empty `jwt-secret` (an unset variable) refuses every token and
the auditor reports `AUTH_EMPTY_JWT_SECRET`. A refused token gets
`WWW-Authenticate: Bearer realm="flAPI", error="invalid_token"`; no token at all
gets the bare `Bearer realm="flAPI"`.

There is one secret per block. Rotating it means changing it and restarting
flAPI, and every token signed with the old one stops working at once.

### OIDC

> In v26.11.25 OIDC rejected every token, valid ones included
> ([#273](https://github.com/DataZooDE/flapi/issues/273)); upgrade to a later release.

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
```

A token is accepted when:

- its header has a `kid` found in the JWKS and an `alg` of RS256, RS384 or
  RS512 (EC keys such as ES256 are refused). The JWKS is kept for
  `jwks-cache-hours` (default 24); a token with an unknown `kid` refetches it,
  up to three times per request and with no cooldown;
- `iss` equals `issuer-url` exactly, trailing slash included;
- `aud` holds one of `allowed-audiences`. **Without `allowed-audiences`, any
  token from that issuer is accepted**, including ones minted for other
  applications;
- it has an `exp` no older than `clock-skew-seconds` (default 300). A token
  without `exp` is refused unless `verify-expiration: false`;
- it has a `sub`.

Claims become template variables like this:

| Variable | Claim | Default |
|---|---|---|
| `auth.username` | `username-claim`, else `sub` | `sub`; the presets use `preferred_username` (Microsoft, Keycloak, Okta) or `email` (Google, Auth0) |
| `auth.email` | `email-claim` | `email` |
| `auth.roles` | `role-claim-path` if set, else `roles-claim` | `roles` |

A dot in `role-claim-path` or `roles-claim` means nesting (`realm_access.roles`),
so a claim name that itself contains a dot cannot be read. `groups-claim` is
read but grants nothing: only the roles claim counts for `roles:` and
`allowed-roles`.

`provider-type` fills in `issuer-url` and claim defaults:

| `provider-type` | `issuer-url` it fills | Placeholders |
|---|---|---|
| `microsoft` (aliases `azure`, `azure-ad`, `entra`) | `https://login.microsoftonline.com/{tenant}/v2.0` | `tenant-id`: the tenant GUID |
| `keycloak` | `https://{domain}/realms/{realm}` | `domain`: the full host; `realm` |
| `auth0` | `https://{domain}.auth0.com` | `domain`: the tenant subdomain. Auth0's `iss` ends in `/`, so set `issuer-url: https://<tenant>.auth0.com/` yourself |
| `okta` | `https://{domain}.okta.com/oauth2/default` | `domain`: the tenant subdomain |
| `google` | `https://accounts.google.com` | none |
| `generic` | none: set `issuer-url` | none |

A placeholder left unfilled stops startup with an error naming the key. The
presets replace `username-claim: sub` with their own default even when you
write it, and Keycloak's always sets `role-claim-path: realm_access.roles`, so
`roles-claim` has no effect there; set `role-claim-path` to change it.
`github` is accepted as a value but cannot validate tokens: GitHub publishes no
OIDC discovery document. flAPI only validates tokens: `client-secret`,
`scopes`, `enable-client-credentials` and `enable-refresh-tokens` do not take
part in it.

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

Create it from a file, and give flAPI's AWS identity read access to it:

```bash
aws secretsmanager create-secret --name prod/flapi/users --region eu-central-1 \
  --secret-string file://users.json
```

```json
{"Version": "2012-10-17",
 "Statement": [{"Effect": "Allow", "Action": "secretsmanager:GetSecretValue",
   "Resource": "arn:aws:secretsmanager:eu-central-1:123456789012:secret:prod/flapi/users-*"}]}
```

AWS appends a random six-character suffix to a secret's ARN, hence the `-*`.
A secret encrypted with a customer-managed KMS key also needs `kms:Decrypt` on
that key.

- **AWS credentials:** `secret-id` and `secret-key` in the block (both or
  neither, e.g. from `{{env.NAME}}`), else the AWS SDK's default chain
  (environment, `AWS_PROFILE`, SSO, web identity, container or instance role).
  The deprecated `init:` creates a DuckDB S3 secret named after `secret-name`
  (`prod/flapi/users` becomes `prod_flapi_users`) in a private DuckDB.
- **When it is read:** once at startup per `secret-name`, however many
  endpoints share it, for every `basic` block that names it. A failure stops
  startup.
- **Rotating users:** write the new list and restart flAPI; there is no
  periodic refresh. flAPI reads the version labelled `AWSCURRENT`, so the
  previous list stays available as `AWSPREVIOUS` should you need to roll back.
  Restart replicas one by one behind a load balancer to avoid downtime.

  ```bash
  aws secretsmanager update-secret --secret-id prod/flapi/users --region eu-central-1 \
    --secret-string file://users.json
  ```
- **Where it is kept:** in flAPI's memory. Neither the users nor the AWS
  credentials are in the DuckDB database templates run on, and neither is
  logged. `secret-table` from older configs is ignored with a warning (it
  has no effect), and tables an older version left in a file `db_path` are
  dropped at startup; if that file was in use, rotate those passwords.
- **Inline users win for logins:** a block with `users:` never checks a
  password against the secret, but the secret is still fetched at startup, and
  a failed fetch still stops it.
- **Endpoint override:** `AWS_ENDPOINT_URL_SECRETS_MANAGER` (LocalStack, a VPC
  endpoint) is honoured with a startup warning; plain `http://` only to a
  loopback host or with `FLAPI_ALLOW_INSECURE_SECRETS_ENDPOINT=1`.
  `AWS_ENDPOINT_URL` is not used for it.

The rest is in [CONFIG_REFERENCE § 7.5](../CONFIG_REFERENCE.md#75-aws-secrets-manager).

## 8. Test it

Restart `shop` with the config service, flAPI's admin API that `flapii` talks
to, and a token of your own:

```bash
export FLAPI_CONFIG_SERVICE_TOKEN=$(openssl rand -hex 16)
flapi -c flapi.yaml --host 127.0.0.1 --config-service --config-service-token "$FLAPI_CONFIG_SERVICE_TOKEN"
```

**With curl**, one request per outcome:

```bash
curl -i http://localhost:8080/customers                            # 401: no credentials
curl -i -u bob:wrong http://localhost:8080/customers               # 401: refused
curl -i -u bob:builder http://localhost:8080/admin/balances        # 403: no admin role
curl -s -u alice:wonderland http://localhost:8080/admin/balances   # 200
```

**With flapii**, in a terminal with the same token exported, see how an
endpoint ended up protected:

```bash
flapii endpoints get /customers
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
are never shown. `flapii metrics -o table` counts the four requests above:

```text
4 requests, 0 server errors, 0 cache-not-ready 503s · rejected: 2 × 401, 1 × 403, 0 × 429
```

`flapi --validate-config` prints the security warnings without starting the
server.

## Production checklist

- **TLS:** `enforce-https` with a certificate, or a TLS proxy in front. Basic
  passwords and tokens are readable on plain HTTP. Bind with `--host` to the
  interface you mean.
- **Passwords:** PBKDF2 hashes, from `{{env.NAME}}` or AWS Secrets Manager.
- **Bearer:** `jwt-issuer` set, every token carries `exp`, a secret of at
  least 32 random bytes. Rotation is a new secret and a restart, which ends all
  tokens.
- **MCP:** `mcp.auth` on and `allowed-roles` on every tool, or no `mcp-tool`
  at all.
- **Templates:** fail closed, with `list_contains(string_split(...))` and only
  immutable claims.
- **Rate limits:** `rate-limit` with `key: user-or-ip`, and
  `mcp-tool.rate-limit` for tools.
- **CORS:** `cors.allow-origins` lists your front ends.
- **Audit:** `audit.enabled: true`.
- **Config service:** off, or bound to loopback with a long token. It is an
  admin interface: it can create endpoints with authentication off.
- **CI:** `flapi --validate-config` with zero `SECURITY WARNINGS`.

## Limits you should know

- Bearer checks no `aud`, and a token without `exp` never expires.
- OIDC accepts RS256, RS384 and RS512 only, and exactly one `iss` per block,
  so it cannot accept tokens from several Entra ID tenants.
- An OIDC `groups` claim grants nothing.
- Only `sub`/user name, email and roles reach templates; custom claims such as
  `tid` do not.
- flAPI does not read `X-Forwarded-For`: behind a load balancer every caller
  has the balancer's address, so `key: ip` puts everyone in one bucket.
- Rate-limit buckets are keyed on the raw `Authorization` header per route: a
  new token is a new bucket. They live in memory, per replica.

## Common mistakes

- **`'{{auth.roles}}' LIKE '%admin%'`.** Matches `sysadmin`, `admin-readonly`
  and any role containing the word. Use
  `list_contains(string_split('{{{ auth.roles }}}', ','), 'admin')`.
- **A filter that disappears for anonymous callers.** `{{#auth.username}}`
  sections render nothing when the value is empty. Write conditions that match
  no row when `auth.*` is empty.
- **Expecting REST `auth:` to protect a tool.** It does not (step 5). Enable
  `mcp.auth` and give every tool `allowed-roles`.
- **An endpoint block with `type:` but no users.** It replaces the top-level
  block, users included, and every login fails. Name only `roles:` to keep the
  top-level users.
- **Editing a file and not restarting.** flAPI reads its config at startup.
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
