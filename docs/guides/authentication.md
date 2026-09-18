# Requiring authentication

**Goal:** stop anonymous callers reaching an endpoint, and restrict who can do
what once they are in.

flAPI supports Basic, JWT, Bearer and OIDC. All four are configured the same way:
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
      password: '${ADMIN_PASSWORD}'
      roles: [admin, read, write]
    - username: reader
      password: '${READER_PASSWORD}'
      roles: [read]
```

```bash
curl -u reader:$READER_PASSWORD http://localhost:8080/customers
```

**Never write a literal password.** `${VAR}` reads from the environment; see
[reusing config and reading the environment](./yaml-includes.md) for the
whitelist rules.

## Choosing a method

| Method | `type:` | Use when |
|---|---|---|
| Basic | `basic` | Internal tools, a handful of known callers |
| JWT | `jwt` | You already issue signed tokens |
| Bearer | `bearer` | Static API keys |
| OIDC | `oidc` | An identity provider is the source of truth (Entra, Keycloak, Auth0, Okta) |

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

A caller who authenticates successfully but lacks the role gets **403**, not 401
— the distinction matters when you are reading the audit log.

## Global vs per-endpoint

A global block in `flapi.yaml` applies everywhere; an endpoint's own block
overrides it. To leave one endpoint public while the rest are protected, set
`enabled: false` on it explicitly.

## Checking it

```bash
flapii endpoints get /customers | jq '.auth'

curl -i http://localhost:8080/customers                    # expect 401
curl -i -u reader:$PW http://localhost:8080/customers      # expect 200
```

Both outcomes are recorded. Every request produces an audit line — including the
401 — with the principal, the route and an `X-Request-Id` you can quote. See
[Observability](../OBSERVABILITY.md).

## Related

- **Rate limiting** — a separate `rate-limit:` block, keyed by principal once
  authenticated. [CONFIG_REFERENCE § 2.8](../CONFIG_REFERENCE.md)
- **MCP tools** — honour the same blocks, plus per-tool roles.
  [MCP tools](./mcp-tools.md)

## How it works

Authentication runs in a middleware, after rate limiting and before the handler.
It records the principal and auth kind on the request context but deliberately
does not complete a 401 itself — the unwind does, so exactly one audit line is
written.

- [spec/components/security.md](../spec/components/security.md) — the layered defences, and what never leaves the process
- [spec/REQUEST_LIFECYCLE.md](../spec/REQUEST_LIFECYCLE.md) — where auth sits in the chain
