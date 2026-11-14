# Requiring authentication

**Goal:** stop anonymous callers reaching an endpoint, and restrict who can do
what once they are in.

flAPI supports Basic, Bearer (also spelled `jwt`) and OIDC. All are configured the same way:
an `auth:` block, either globally in `flapi.yaml` or per endpoint.

---

## The quickest thing that works

```yaml
# sqls/customers.yaml
auth:
  enabled: true
  type: basic
  users:
    - username: admin
      password: '{{env.ADMIN_PASSWORD}}'
      roles: [admin, read, write]
    - username: reader
      password: '{{env.READER_PASSWORD}}'
      roles: [read]
```

```bash
curl -u reader:$READER_PASSWORD http://localhost:8080/customers
```

**Never write a literal password.** `'{{env.VAR}}'` reads from the environment (the variable must match `template.environment-whitelist`); see
[reusing config and reading the environment](./yaml-includes.md) for the
whitelist rules. Better still, store a PBKDF2-SHA256 hash, which is what the
startup auditor asks for:

```bash
python3 -c "from passlib.hash import pbkdf2_sha256; import getpass; print(pbkdf2_sha256.using(rounds=600000).hash(getpass.getpass()))"
```

Paste the `$pbkdf2-sha256$600000$…` string as the password. **bcrypt hashes
(`$2b$…`) are not supported**: every login as such a user is refused, and the
auditor reports `AUTH_BCRYPT_UNSUPPORTED`.

## Choosing a method

| Method | `type:` | Use when |
|---|---|---|
| Basic | `basic` | Internal tools, a handful of known callers |
| Bearer / JWT | `bearer` (alias `jwt`) | You issue HS256-signed JWTs: the token must verify against `jwt-secret`, carry `iss` equal to `jwt-issuer`, a `sub` claim and a `roles` array (it may be empty). A token missing any of them, or a static API key, gets `401` |
| OIDC | `oidc` | An identity provider is the source of truth (Entra, Keycloak, Auth0, Okta) |

For OIDC, `provider-type` picks a preset issuer URL and you fill in its
placeholders: `tenant-id` for Microsoft (`provider-type: microsoft`, or its
aliases `azure`, `azure-ad`, `entra`), `domain` and `realm` for Keycloak
(`https://{domain}/realms/{realm}`). A placeholder you leave unset stops
startup with an error naming the key.

Each method's full parameter list is in
[CONFIG_REFERENCE § 7](../CONFIG_REFERENCE.md) — JWT in § 7.2, OIDC in § 7.4,
and AWS Secrets Manager for credential storage in § 7.5.

## Roles

Roles gate individual endpoints and MCP tools:

```yaml
auth:
  enabled: true
  type: jwt
  # ...

  roles: [analyst, admin]     # this endpoint needs one of these
```

A caller who authenticates successfully but holds none of the listed roles gets
**403** (`{"error":"Forbidden","message":"The authenticated principal does not hold any role this endpoint requires."}`),
not 401 — the distinction matters when you are reading the audit log. An empty
or missing `roles` list lets any authenticated caller in. Basic-auth users take
their roles from `users[].roles`; token callers from the token's roles claim.

## Global vs per-endpoint

A top-level `auth:` block with `enabled: true` in `flapi.yaml` applies to every
REST endpoint that has **no `auth:` block of its own** — including endpoints you
create later through the config service. An endpoint's own block that names a
`type` or credentials **replaces** the global one; the two are not merged, so
such a block must be complete: the `type`, the users or secret, and its `roles`.

A block that names **no scheme and no credentials** — `auth: {roles: [admin]}` —
uses the global block's type and credentials and applies its own `roles` on
top. Without an enabled global block that is a load error (`AUTH_NO_TYPE`), as
is a block with `users:` or a secret but no `type:`. One scheme applies per
block: keys the chosen type never reads (`users:` under `bearer`,
`jwt-secret:` under `basic`) are named in a load warning.

To leave one endpoint public while the rest are protected, opt it out
explicitly:

```yaml
auth:
  enabled: false
```

Only `enabled: false` opts out. An `auth:` block without `enabled` that sets
`type`, `roles`, `users`, `jwt-secret`, `jwt-issuer`, `oidc` or
`from-aws-secretmanager` is on; startup logs that it is.

Startup tells you what happened:
`Global auth (type 'basic') applies to 3 endpoint(s) without their own auth: block; 1 endpoint(s) opt out with auth: {enabled: false}`.

> **Upgrading:** earlier versions ignored the global block for endpoints
> without their own. If you had one enabled, those endpoints now answer `401`
> to anonymous callers.

The global block covers REST only. MCP tools are protected by `mcp.auth` and
per-tool `allowed-roles`; see [MCP tools](./mcp-tools.md).

## CORS with authentication

With no `cors:` block, or `allow-origins` empty or containing `"*"`, any web
page can call the server cross-origin and read the answer. Combined with
authentication that means a page can use a credential the browser holds, so
the startup auditor warns `CORS_WILDCARD_WITH_AUTH`. List the origins that may
call you:

```yaml
cors:
  allow-origins: ['https://app.example.com']
```

Browser preflights (`OPTIONS` with `Access-Control-Request-Method`) from an
allowed origin get `Access-Control-Allow-Origin`, `-Methods`, `-Headers`,
`-Max-Age` and `Vary: Origin`, on REST paths and `/mcp/jsonrpc`. The requested
headers are echoed unless you set `cors.allow-headers`, so `Authorization`
passes; a bare `*` would not cover it.

A `401` carries the endpoint's own challenge — `WWW-Authenticate: Basic
realm="flAPI"` or `Bearer realm="flAPI"` (with `error="invalid_token"` when a
token was refused) — and a JSON body `{"error":"Unauthorized","message":...}`.

## Checking it

```bash
flapii endpoints get /customers | jq '.auth'      # roles, and inherited: true when it came from flapi.yaml

curl -i http://localhost:8080/customers                    # expect 401
curl -i -u reader:$PW http://localhost:8080/customers      # expect 200
curl -i -u reader:$PW http://localhost:8080/admin-only     # expect 403 (roles: [admin])
```

Both outcomes are recorded. Every request produces an audit line — including the
401 — with the principal, the route and an `X-Request-Id` you can quote. See
[Observability](../OBSERVABILITY.md).

## Related

- **Rate limiting** — a separate `rate-limit:` block, keyed by principal once
  authenticated. [CONFIG_REFERENCE § 2.8](../CONFIG_REFERENCE.md)
- **MCP tools** — protected by `mcp.auth` and per-tool `allowed-roles`, not
  by the REST `auth:` block. [MCP tools](./mcp-tools.md)

## How it works

Authentication runs in a middleware, after rate limiting and before the handler.
It records the principal and auth kind on the request context but deliberately
does not complete a 401 itself — the unwind does, so exactly one audit line is
written.

- [spec/components/security.md](../spec/components/security.md) — the layered defences, and what never leaves the process
- [spec/REQUEST_LIFECYCLE.md](../spec/REQUEST_LIFECYCLE.md) — where auth sits in the chain
