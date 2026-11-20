# flAPI Config Service API Reference

**Version:** 1.0.0
**flAPI Version:** >= 1.0.0

This document provides a complete reference for the flAPI Config Service REST API and the `flapii` CLI client for runtime configuration management.

---

## Table of Contents

1. [Overview](#1-overview)
   - [Architecture](#architecture)
   - [Enabling Config Service](#enabling-config-service)
   - [Authentication](#authentication)
2. [REST API Reference](#2-rest-api-reference)
   - [2.1 Project Configuration](#21-project-configuration)
   - [2.2 Endpoint Management](#22-endpoint-management)
   - [2.3 Template Management](#23-template-management)
   - [2.4 Cache Management](#24-cache-management)
   - [2.5 Schema Introspection](#25-schema-introspection)
   - [2.6 Server Administration](#26-server-administration)
   - [2.7 Filesystem Operations](#27-filesystem-operations)
3. [CLI Client (flapii)](#3-cli-client-flapii)
   - [3.1 Installation](#31-installation)
   - [3.2 Global Options](#32-global-options)
   - [3.3 Environment Variables](#33-environment-variables)
   - [3.4 Commands Reference](#34-commands-reference)
   - [3.5 Output Formats](#35-output-formats)
4. [Common Patterns](#4-common-patterns)
   - [Error Handling](#error-handling)
   - [Authentication Flow](#authentication-flow)
- [Appendix A: Endpoint Slug Format](#appendix-a-endpoint-slug-format)
- [Appendix B: HTTP Status Codes](#appendix-b-http-status-codes)
- [Related Documentation](#related-documentation)

---

## 1. Overview

### Architecture

The Config Service provides runtime configuration management without server restart:

```
┌─────────────────────────────────────────────────────────┐
│                    flAPI Server                         │
├─────────────────────────────────────────────────────────┤
│  /api/v1/_config/*  ──→  Config Service REST API        │
│  /api/v1/*          ──→  User-defined REST endpoints    │
│  /mcp/*             ──→  MCP Protocol endpoints         │
└─────────────────────────────────────────────────────────┘
                          ↑
                          │ HTTP/HTTPS
                          ↓
┌─────────────────────────────────────────────────────────┐
│                   flapii CLI Client                     │
│  Commands: ping, config, project, endpoints,            │
│            templates, cache, schema, mcp                │
└─────────────────────────────────────────────────────────┘
```

### Enabling Config Service

Start the flAPI server with the `--config-service` flag:

```bash
# With auto-generated token (printed to stdout)
./flapi --config-service

# With custom token
./flapi --config-service --config-service-token "your-secret-token"

# Via environment variable
export FLAPI_CONFIG_SERVICE_TOKEN="your-secret-token"
./flapi --config-service
```

### Authentication

All Config Service endpoints require authentication via bearer token.

**Header Options:**

```http
Authorization: Bearer <token>
```

or:

```http
X-Config-Token: <token>
```

Any non-empty token configured with `--config-service-token` / `FLAPI_CONFIG_SERVICE_TOKEN` is
accepted; startup warns when it is shorter than 16 characters. The `flapi_*` MCP config tools take
the same token: `X-Config-Token` first, then `Authorization: Bearer`.

**Unauthenticated Response** (every config route, `/metrics` included):

```http
HTTP/1.1 401 Unauthorized
Content-Type: application/json

{"error": "Unauthorized: missing or invalid config-service token (send it as X-Config-Token or Authorization: Bearer)"}
```

A request that presents a token which does not match is also logged as a warning
(`Config service: rejected X-Config-Token token for /api/v1/_config/...`; the token itself is never
logged). A `flapi_*` MCP tool called without a token, or with a wrong one, answers JSON-RPC error
`-32001` naming `X-Config-Token` (it was `-32603 "Tool execution failed: ..."`).

**Invalidated database.** After an internal DuckDB error invalidates the database (nothing recovers it
but a restart), every config route except `/metrics` and `/log-level` answers, after the token check:

```http
HTTP/1.1 503 Service Unavailable
Retry-After: 30
Content-Type: application/json

{"error": "database_invalidated", "message": "The database has been invalidated by an earlier internal error and flapi must be restarted; GET /health shows the cause."}
```

`GET /api/v1/_config/health` answers `503` with `"status": "fatal"` and
`database: {"status": "invalidated", "reason": "..."}`.

---

## 2. REST API Reference

### 2.1 Project Configuration

#### GET /api/v1/_config/project

Retrieve the main project configuration.

**Request:**
```http
GET /api/v1/_config/project HTTP/1.1
Authorization: Bearer <token>
```

**Response (200)** (abridged):
```json
{
  "name": "my-api",
  "description": "Customer data API",
  "template-path": "/srv/api/sqls",
  "connections": {"customers": {"init": "...", "properties": {"path": "./data/customers.parquet"}}},
  "auth": {"enabled": false},
  "ducklake": {"enabled": true, "alias": "cache", "metadata-path": "postgres:dbname=lake host=db password=<redacted>",
               "warmup": "always", "warmup_effective": "always", "read-only": false},
  "bundled": false
}
```

`warmup_effective` is the warm-up that actually runs: `never` on a read-only replica, otherwise the
`--cache-warmup` / `FLAPI_CACHE_WARMUP` override if set, else `warmup`. `metadata-path` and `data-path`
are redacted: `password=`, `pwd=`, `*secret*=`, `*token*=`, `api_key=` values and the password of a
`scheme://user:password@host` URL show as `<redacted>`. The same applies to a resolved `{{env.X}}`.

`bundled` is `true` when the server runs from a packed binary (`flapi pack`): it serves the configuration
folded into it, so edits to files on disk cannot reach it and its config-service writes are refused. The
Claude Code hook uses it to say so instead of reloading.

The project configuration is read-only through this API: `PUT /api/v1/_config/project` answers
`405 Method Not Allowed` with `Allow: GET` (after the usual `401` for a missing token). Edit
`flapi.yaml` and restart. The same applies to `DELETE /config` and other non-GET methods on `/config`.

**CLI:** `flapii project get`

> **Implementation:** `src/config_service.cpp` | **Tests:** `test/cpp/config_service_test.cpp`

---

#### GET /api/v1/_config/environment-variables

List the environment variables that exist and match a `template.environment-whitelist` pattern, plus plain-name whitelist entries (reported `available: false` when unset). Credential-looking names and long values are redacted.

**Request:**
```http
GET /api/v1/_config/environment-variables HTTP/1.1
Authorization: Bearer <token>
```

**Response (200):**
```json
{
  "variables": [
    {
      "name": "DB_PASSWORD",
      "value": "***",
      "available": true
    },
    {
      "name": "API_KEY",
      "value": null,
      "available": false
    }
  ]
}
```

---

### 2.2 Endpoint Management

**Addressing an endpoint.** Every `/api/v1/_config/endpoints/{slug}/...` route takes the slug of the
url-path (`/customers` -> `-customers`, `/order-items` -> `-order~1items`) or an MCP-only endpoint's
name, and an optional `?method=GET|POST|...`. Several files may serve one url-path with different
methods (`GET /orders` and `POST /orders`): such a slug without `?method=` answers
`400 {"error": "Ambiguous: /orders is served by GET (<file>), POST (<file>); specify ?method=GET|POST"}`
instead of acting on an arbitrary one. A slug served by one endpoint needs no method. A `:` in the
slug (`/products/:id` -> `-products-:id`) may be sent literally or as `%3A`. Unknown slug:
`404 {"error": "Endpoint not found: ..."}`.

#### GET /api/v1/_config/endpoints

List all endpoint configurations.

**Request:**
```http
GET /api/v1/_config/endpoints HTTP/1.1
Authorization: Bearer <token>
```

**Response (200):** an object keyed by URL path (or MCP name). A url-path served by several endpoints
is keyed `"METHOD /path"` for each of them (`"GET /orders"`, `"POST /orders"`); a path served once
keeps its plain key. The list uses camelCase keys;
`GET /api/v1/_config/endpoints/{slug}` returns the same document with the YAML (hyphenated) keys.
MCP-only tools and resources carry their `templateSource` and `connection` too.
```json
{
  "/customers": {
    "urlPath": "/customers",
    "method": "GET",
    "templateSource": "/srv/api/sqls/customers.sql",
    "connection": ["customers-db"],
    "auth": {"enabled": true, "type": "basic", "roles": [], "inherited": true},
    "cache": {"enabled": true, "table": "customers_cache", "schema": "analytics", "schedule": "1h"}
  }
}
```

`auth.inherited` is `true` when the endpoint has no `auth:` block of its own and the top-level
`auth:` applies; `auth.roles` lists the roles the endpoint requires (empty: any authenticated caller).

**CLI:** `flapii endpoints list`

---

#### POST /api/v1/_config/endpoints

Create a new endpoint. The body goes through the same loader as a YAML file on disk, so
`mcp-tool`, validators, `rate-limit`, `auth` and `cache` all survive. The endpoint is written to
A file named after the url-path (`/orders` -> `orders.yaml`, `/a/b` -> `a_b.yaml`; an MCP-only endpoint after its name) in the templates directory
and registered at once, including in MCP `tools/list`. An endpoint without an `auth:` block inherits
the top-level `auth:`. `409` when that file exists, when a loaded endpoint has the same url-path and
method or the same MCP tool/resource/prompt name, or when any YAML file in the templates directory
already declares them (loaded or not). In that last case the error names the file and says to load
it with `POST /api/v1/_config/endpoints/reload-file` (`flapii endpoints reload --file <yaml>`)
instead of writing a second copy.

**Request:**
```http
POST /api/v1/_config/endpoints HTTP/1.1
Authorization: Bearer <token>
Content-Type: application/json

{
  "url-path": "/orders",
  "method": "GET",
  "template-source": "orders.sql",
  "connection": ["orders-db"],
  "request": [
    {
      "field-name": "customer_id",
      "field-in": "query",
      "required": false,
      "validators": [
        { "type": "int", "min": 1 }
      ]
    }
  ]
}
```

**Response (201):**
```json
{
  "status": "created",
  "endpoint": "REST endpoint: /orders",
  "slug": "-orders",
  "url-path": "/orders",
  "file": "/srv/api/sqls/orders.yaml"
}
```

| Status | When |
|--------|------|
| `201` | created and written to `file` |
| `400` | invalid JSON, or the configuration does not parse: `{"error": "..."}` |
| `409` | an endpoint with that path/name already exists (update it with PUT, or delete it first), or the server runs from a packed bundle |

**CLI:** `flapii endpoints create -f endpoint.yaml`

---

#### GET /api/v1/_config/endpoints/{slug}

Get a specific endpoint configuration.

**Path Parameters:**

| Parameter | Type | Description |
|-----------|------|-------------|
| `slug` | string | Endpoint identifier (see [Appendix A](#appendix-a-endpoint-slug-format)) |

**Request:**
```http
GET /api/v1/_config/endpoints/-customers HTTP/1.1
Authorization: Bearer <token>
```

**Response (200):**
```json
{
  "url-path": "/customers",
  "method": "GET",
  "template-source": "customers.sql",
  "connection": ["customers-db"],
  "request": [...],
  "cache": {...},
  "auth": {...}
}
```

**CLI:** `flapii endpoints get /customers`

---

#### PUT /api/v1/_config/endpoints/{slug}

Update an endpoint configuration. The body is deep-merged onto the endpoint's YAML file and the
result is re-parsed and written back, so you can send only the keys you change - `{"template-source":
"customers-v2.sql"}` is a complete request; the url-path comes from `{slug}`. The file is patched
as text: only the keys the request changes are rewritten, and comments, blank lines, key order and
flow-style values elsewhere in the file are kept (an inline `# comment` after a changed one-line
scalar is kept too). Comments *inside* a value that is replaced - a `request:` list whose
contents change, for example - are lost with it. A file the patcher cannot handle line by line
(document markers, quoted or complex keys) is re-emitted whole, which drops its comments; the
result is always re-parsed and verified before it is written.

**Request:**
```http
PUT /api/v1/_config/endpoints/-customers HTTP/1.1
Authorization: Bearer <token>
Content-Type: application/json

{
  "url-path": "/customers",
  "method": "GET",
  "template-source": "customers-v2.sql",
  "connection": ["customers-db"]
}
```

**Response (200):**
```json
{
  "status": "updated",
  "endpoint": "REST endpoint: /customers",
  "slug": "-customers",
  "url-path": "/customers",
  "file": "/srv/api/sqls/customers.yaml"
}
```

| Status | When |
|--------|------|
| `200` | updated and persisted |
| `200` with `"persisted": false` | the body used camelCase keys (`urlPath`, `templateSource`): applied in memory only, with a `warning`; send the YAML (hyphenated) keys to persist. Keys it leaves out come from the endpoint |
| `400` | the update changes `url-path`, `method` or the MCP name (delete and re-create instead), the result does not parse, or the body cannot be read as an endpoint configuration (`{"error": ...}`; this was a 500 for a partial camelCase body) |
| `404` | unknown endpoint |
| `409` | the endpoint's YAML uses `{{include}}` (edit the files by hand), or the server runs from a packed bundle |

**CLI:** `flapii endpoints update /customers -f endpoint.yaml`

---

#### DELETE /api/v1/_config/endpoints/{slug}

Delete an endpoint.

**Request:**
```http
DELETE /api/v1/_config/endpoints/-customers HTTP/1.1
Authorization: Bearer <token>
```

Unregisters the endpoint and removes its YAML file.

**Response (200):**
```json
{
  "status": "deleted",
  "endpoint": "REST endpoint: /customers"
}
```

Unknown endpoint: `404 {"error": "Endpoint not found"}`.

**CLI:** `flapii endpoints delete /customers --force`

---

#### POST /api/v1/_config/endpoints/{slug}/validate

Validate endpoint configuration with the same parser the server loads files with.

**Request:**
```http
POST /api/v1/_config/endpoints/-customers/validate HTTP/1.1
Authorization: Bearer <token>
Content-Type: text/plain

url-path: /customers
method: GET
template-source: customers.sql
connection: [customers-db]
```

**Response (200):**
```json
{"valid": true}
```

**Response (400):**
```json
{"valid": false, "errors": ["Parse error: Configuration error at template-source: Missing required key: template-source"]}
```

An empty body gives `400` with `"No YAML given: send the endpoint YAML as the request body"`.

**CLI:** `flapii endpoints validate /customers -f endpoint.yaml` (or pipe the YAML on stdin; with
neither, flapii stops with `No YAML given: pass --file <path> or pipe the YAML on stdin`)

---

#### POST /api/v1/_config/endpoints/{slug}/reload

Reload endpoint configuration from disk.

**Request:**
```http
POST /api/v1/_config/endpoints/-customers/reload HTTP/1.1
Authorization: Bearer <token>
```

**Response (200):**
```json
{
  "success": true,
  "message": "Endpoint configuration reloaded successfully"
}
```

A file that no longer loads gives `400` with the reason; the endpoint keeps its previous
configuration. `404` means only that the endpoint is unknown (a new file is not picked up by
reload; create it with POST or restart).
```json
{
  "success": false,
  "error": "Failed to reload endpoint configuration",
  "message": "Failed to reload endpoint configuration",
  "reason": "/srv/api/sqls/customers.yaml: Failed to parse YAML: ..."
}
```

Every reload error carries `error`, like create, update and delete (`404`: `"error": "Endpoint not
found: ..."`). A reload whose file now declares another endpoint's url-path and method, or another
file's MCP tool/resource/prompt name, fails with `"reason": "Duplicate endpoint GET /x: declared in
... and ..."` (or `Duplicate MCP tool name 'x': declared in ... and ...`).

The reload replaces the endpoint(s) loaded **from that endpoint's file**. It used to replace the first
endpoint with the same url-path, so reloading the `POST /orders` file overwrote `GET /orders` and
answered 200 with the POST change not applied. Use `?method=` when the path is shared.

Every endpoint change (create, update, delete, reload; over REST or the `flapi_*` MCP tools)
rebuilds the MCP `tools/list`, `resources/list` and `prompts/list` at once. Pagination cursors issued
before the change are rejected. No `notifications/list_changed` is sent (`listChanged` stays `false`),
so MCP clients see the change on their next list call.

**CLI:** `flapii endpoints reload /customers [--method POST]`

---

#### POST /api/v1/_config/endpoints/reload-file

Reload every endpoint a YAML file declares, addressed by the FILE - or load it when nothing is loaded
from it yet (a hand-written endpoint YAML added after startup goes live without a restart).

**Request:**
```http
POST /api/v1/_config/endpoints/reload-file HTTP/1.1
X-Config-Token: <token>
Content-Type: application/json

{"file": "/srv/api/sqls/orders_post.yaml"}
```

A relative `file` is resolved against the directory of `flapi.yaml`.

**Response:** `200` (reloaded) or `201` (newly loaded):
```json
{
  "success": true,
  "status": "loaded",
  "file": "/srv/api/sqls/orders_post.yaml",
  "endpoints": ["POST /orders"],
  "message": "Endpoint configuration loaded"
}
```

`endpoints` lists each endpoint by its key: `"METHOD /path"`, or the MCP name for an MCP-only file.
Errors are `{"success": false, "error": "..."}`:

| Status | When |
|--------|------|
| `400` | no `file`; the file is outside `template.path` or not YAML; it does not validate; or it is not an endpoint file (`... is not an endpoint file (no url-path, mcp-tool, mcp-resource or mcp-prompt)`) |
| `404` | the file does not exist and no loaded endpoint was declared in it |
| `409` | it declares a url-path + method or an MCP name another file already serves (both files named), or the server runs from a packed bundle |

**A deleted file unloads its endpoints.** When `file` no longer exists but endpoints loaded from it are
still served, they are unloaded and the answer is `200`:

```json
{"success": true, "status": "removed", "file": "/srv/api/sqls/orders_post.yaml",
 "removed": ["POST /orders"], "endpoints": [],
 "message": "Endpoint file no longer exists; its endpoints were unloaded"}
```

It used to answer `404` and leave the routes loaded. `POST .../endpoints/{slug}/reload` on an endpoint whose
YAML was deleted does the same (`"status": "removed"`).

**CLI:** `flapii endpoints reload --file sqls/orders_post.yaml`

---

#### GET /api/v1/_config/endpoints/{slug}/parameters

Get parameter definitions for an endpoint.

**Request:**
```http
GET /api/v1/_config/endpoints/-customers/parameters HTTP/1.1
Authorization: Bearer <token>
```

**Response (200):**
```json
{
  "endpoint": "/customers",
  "method": "GET",
  "parameters": [
    {
      "name": "id",
      "in": "query",
      "description": "Customer ID",
      "required": false,
      "default": null,
      "validators": [
        { "type": "int", "min": 1 }
      ]
    }
  ]
}
```

---

#### POST /api/v1/_config/endpoints/by-template

Find endpoints using a specific template file: as their `template-source`, or as their cache
`template-file` (the cache populate SQL).

**Request:**
```http
POST /api/v1/_config/endpoints/by-template HTTP/1.1
Authorization: Bearer <token>
Content-Type: application/json

{
  "template_path": "customers.sql"
}
```

**Response (200):**
```json
{
  "template_path": "customers.sql",
  "count": 2,
  "endpoints": [
    {
      "url_path": "/customers",
      "method": "GET",
      "slug": "-customers",
      "role": "template-source",
      "roles": ["template-source"],
      "config_file_path": "/srv/api/sqls/customers.yaml",
      "type": "REST"
    },
    {
      "mcp_name": "get_customers",
      "slug": "get_customers",
      "role": "cache.template-file",
      "roles": ["cache.template-file"],
      "type": "MCP_Tool"
    }
  ]
}
```

`role` says how the endpoint uses the file (`template-source` or `cache.template-file`); `roles` lists
both when one file is both.

> **Implementation:** `src/config_service.cpp`, `src/endpoint_repository.cpp` | **Tests:** `test/cpp/config_service_test.cpp`, `test/integration/test_config_service_endpoints.tavern.yaml`

---

### 2.3 Template Management

#### GET /api/v1/_config/endpoints/{slug}/template

Get the SQL template content.

**Request:**
```http
GET /api/v1/_config/endpoints/-customers/template HTTP/1.1
Authorization: Bearer <token>
```

**Response (200):**
```json
{
  "template": "SELECT * FROM customers\nWHERE 1=1\n{{#params.id}}\n  AND id = {{ params.id }}\n{{/params.id}}"
}
```

**CLI:** `flapii templates get /customers`

---

#### PUT /api/v1/_config/endpoints/{slug}/template

Update the SQL template.

**Request:**
```http
PUT /api/v1/_config/endpoints/-customers/template HTTP/1.1
Authorization: Bearer <token>
Content-Type: application/json

{
  "template": "SELECT * FROM customers WHERE status = 'active'"
}
```

**Response (200):**
```json
{
  "success": true,
  "message": "Template updated"
}
```

**Response (400):** the template is not valid Mustache (an unbalanced or crossed section, `{{{` not closed
by `}}}`, an unterminated `{{`, an empty tag, a partial or a set-delimiter tag). The file is not written:

```json
{"error": "Invalid template sqls/customers.sql:3:1: section '{{#params.id}}' is never closed (missing '{{/params.id}}')"}
```

`PUT .../cache/template` answers the same way for the cache template, and a reload of an endpoint whose
template on disk is invalid answers 400 with that message in `reason`.

**CLI:** `flapii templates update /customers -f template.sql`

---

#### POST /api/v1/_config/endpoints/{slug}/template/expand

Expand a Mustache template with parameters.

**Query Parameters:**

| Parameter | Type | Description |
|-----------|------|-------------|
| `include_variables` | boolean | Include variable metadata in response |
| `validate_only` | boolean | Check the rendered SQL without running it (see below) |
| `target` | `cache` | The cache populate template instead of the serving template |

**Request:**
```http
POST /api/v1/_config/endpoints/-customers/template/expand HTTP/1.1
Authorization: Bearer <token>
Content-Type: application/json

{
  "parameters": {
    "id": "123",
    "status": "active"
  }
}
```

**Response (200):**
```json
{
  "expanded": "SELECT * FROM customers\nWHERE 1=1\n  AND id = ?\n  AND status = 'active'",
  "bindings": [
    {"position": 1, "name": "id", "type": "integer", "value": 123}
  ]
}
```

`expanded` is the SQL the serving path runs: a `{{ params.x }}` whose field has a typed validator
(`int`, `number`, `boolean`, `date`, `time`, ...) is a `?` placeholder and its value is bound, listed in
`bindings` in placeholder order (`value` is `null` for an absent parameter: the placeholder binds NULL).
The value used to be pasted into `expanded`, which showed SQL that never runs (`ALFKI IS NULL OR ...`,
or `( IS NULL OR c <= )` for an absent value). `{{{ }}}` and untyped fields are still substituted.

`?target=cache` renders the cache populate template as the next refresh would (no bindings). When the
cursor watermark read from the cache table does not match `cursor.type` (it is then dropped and the
refresh loads the full source), the answer carries `watermark_dropped_reason`.

A template with a Mustache syntax error (an unclosed `{{#section}}`, a stray `{{/x}}`) is a **400**
whose `error` names the file, line and column (`Invalid template .../orders_cache.sql:1:1: ...`). It used
to be a 500.

**`?validate_only=1` (serving template) and `?target=cache&validate_only=1` (cache template).** The
template is rendered - the serving one as the serving path renders it (typed params as `?`
placeholders), the cache one with the live watermark exactly as the next refresh would, and a cache
without `template-file` as the `CREATE OR REPLACE TABLE ... AS <serving SELECT>` it is materialised as -
and then checked, **never run**:

1. split into statements (top-level `;`; string literals, quoted identifiers and comments respected), each
   parsed by DuckDB's parser: a syntax error is reported against its own statement;
2. for `target=cache`, the bare-SELECT rule of a refresh: a SELECT statement is an error;
3. every statement is prepared - bound and planned, which is what `EXPLAIN` checks: unknown tables and
   columns, type errors - inside a transaction that is always rolled back. No DML and no query runs. Only
   a definition without data (`CREATE TABLE` without AS, `CREATE SCHEMA`/`VIEW`) is executed in that
   transaction, and a `CREATE TABLE ... AS` as `... WITH NO DATA`, so a later statement that writes the
   table they create (`MERGE INTO` on a first build) is bound against its real columns.
4. for `target=cache`, an `ATTACH ... (READ_ONLY)` in the template is executed, so the statements that
   read the attached database are checked against it (they used to fail with `schema src does not exist`).
   A `DETACH` is executed only for a database this check attached; a database the server already has
   attached is used as it is. Everything the check attached is detached again afterwards. An `ATTACH`
   without `READ_ONLY` is not executed: it is a `not_checked` warning, and so is every statement that
   reads its alias. The check waits for a running cache refresh (they share the catalog lock), so it can
   never collide with a refresh attaching the same alias.

`template` in the body (an unsaved candidate) is validated instead of the file, for the serving template.

```json
{
  "valid": false,
  "target": "cache",
  "statements": 2,
  "expanded": "CREATE TABLE IF NOT EXISTS cache.main.orders_cache AS ...;\nMERGE INTOO ...",
  "errors": [
    {"statement": 2, "type": "sql_syntax", "kind": "parse",
     "message": "SQL validation failed: statement 2: Parser Error: syntax error at or near \"INTOO\""}
  ],
  "warnings": []
}
```

| Error `type` (`kind`) | Meaning |
|---|---|
| `sql_syntax` (`parse`) | The statement does not parse |
| `sql_syntax` (`bind`) | It parses but does not bind: unknown table/column, type mismatch |
| `bare_select` | `target=cache`: a SELECT builds nothing. Before the last statement the message reads "statement N of M is a bare SELECT; its result is discarded. Wrap a check in CREATE OR REPLACE TEMP TABLE _chk AS SELECT ..." |
| `template_syntax` | The Mustache template is unbalanced (`statement` 0) |
| `template_load`, `template_expansion`, `context_creation` | Serving template only: it could not be read or rendered |

| Warning `type` | Meaning |
|---|---|
| `cache_not_built` | The statement reads or writes the endpoint's cache table, which does not exist yet: run `flapii cache refresh <path>`. Not an error |
| `not_checked` | The statement uses a table an earlier statement would create and this check could not (e.g. `CREATE INDEX`), or its parameter types could not be determined |
| `watermark_dropped` | `target=cache`: the `watermark_dropped_reason` of the plain expansion |
| `undefined_variable` | Serving template: a `{{ name }}` the context may not define |

`valid` is `true` exactly when `errors` is empty. `bindings` is not returned.

**CLI:** `flapii templates expand /customers -p '{"id":"123"}' [--cache] [--method GET]`

---

#### POST /api/v1/_config/endpoints/{slug}/template/test

Execute a template with sample parameters (limited to 10 rows).

**Request:**
```http
POST /api/v1/_config/endpoints/-customers/template/test HTTP/1.1
Authorization: Bearer <token>
Content-Type: application/json

{
  "parameters": {
    "id": "123"
  }
}
```

**Response (200):**
```json
{
  "success": true,
  "columns": ["id", "name", "email"],
  "rows": [
    {"id": 123, "name": "John Doe", "email": "john@example.com"}
  ]
}
```

**CLI:** `flapii templates test /customers -p '{"id":"123"}'`

> **Implementation:** `src/config_service.cpp` | **Tests:** `test/cpp/config_service_template_lookup_test.cpp`

---

### 2.4 Cache Management

#### GET /api/v1/_config/endpoints/{slug}/cache

Get cache configuration.

**Request:**
```http
GET /api/v1/_config/endpoints/-customers/cache HTTP/1.1
Authorization: Bearer <token>
```

**Response (200):**
```json
{
  "enabled": true,
  "table": "customers_cache",
  "schema": "cache",
  "schedule": "5m",
  "primary-key": ["id"],
  "cursor": {
    "column": "updated_at",
    "type": "timestamp"
  },
  "retention": {
    "keep-last-snapshots": 5,
    "max-snapshot-age": "30d"
  }
}
```

**CLI:** `flapii cache get /customers`

---

#### PUT /api/v1/_config/endpoints/{slug}/cache

Update cache configuration.

**Request:**
```http
PUT /api/v1/_config/endpoints/-customers/cache HTTP/1.1
Authorization: Bearer <token>
Content-Type: application/json

{
  "enabled": true,
  "table": "customers_cache",
  "schedule": "10m"
}
```

**Response (200):**
```json
{
  "success": true,
  "message": "Cache configuration updated"
}
```

**CLI:** `flapii cache update /customers --schedule 10m`

---

#### GET /api/v1/_config/endpoints/{slug}/cache/template

Get the cache populate template.

**Request:**
```http
GET /api/v1/_config/endpoints/-customers/cache/template HTTP/1.1
Authorization: Bearer <token>
```

**Response (200):**
```json
{
  "template": "CREATE OR REPLACE TABLE {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS SELECT * FROM source"
}
```

**CLI:** `flapii cache template /customers`

---

#### PUT /api/v1/_config/endpoints/{slug}/cache/template

Update the cache populate template.

**Request:**
```http
PUT /api/v1/_config/endpoints/-customers/cache/template HTTP/1.1
Authorization: Bearer <token>
Content-Type: application/json

{
  "template": "CREATE OR REPLACE TABLE {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS SELECT * FROM customers"
}
```

**Response (200):**
```json
{
  "success": true,
  "message": "Cache template updated"
}
```

On a read-only replica (`ducklake.read-only: true`) this answers `409 read_only_cache` ("the populate
template belongs to the writer; edit it there"). A replica shares the populate SQL with the writer.

**CLI:** `flapii cache update-template /customers -f cache.sql`

---

#### POST /api/v1/_config/endpoints/{slug}/cache/refresh

Refresh a cache now. The call returns when the refresh has finished.

**Query Parameters:**

| Parameter | Description |
|-----------|-------------|
| `full` | `true` (or `1`): full rebuild. The incremental watermark is erased and the cache table is dropped (`DROP TABLE IF EXISTS`) in the same transaction as the populate template, so an append cache is rebuilt rather than re-appended. A template with its own `BEGIN`/`COMMIT` only gets the watermark cleared (a warning is logged). |

**Request:**
```http
POST /api/v1/_config/endpoints/-customers/cache/refresh?full=true HTTP/1.1
Authorization: Bearer <token>
```

**Response (200):** the stats this refresh recorded (the same values as the newest audit row):

```json
{
  "status": "refreshed",
  "endpoint": "/customers",
  "mode": "merge",
  "full": true,
  "duration_ms": 412,
  "rows_affected": 37,
  "snapshot_id": "12"
}
```

`mode` is the mode that ran (`full`, `append` or `merge`; `full` for a cache without `template-file`).
`warning` is present when the refresh ran but not as configured: the cursor watermark did not look like
`cursor.type` and was dropped, so the refresh loaded the full source (the same text is appended to the
audit row's `message`).
`duration_ms`, `rows_affected` and `snapshot_id` are left out when unknown. `snapshot_id` is the snapshot
this refresh committed; it is absent when the refresh committed none (for example a merge with nothing to
change). When a refresh of the same
table is already running (warm-up, the scheduler) nothing is started and the answer is
`{"status": "already_running", "endpoint": ..., "full": ...}`.

`full` is `true` when the refresh loaded the whole source: `?full=true`, a full-mode cache (no
`cursor`), or an incremental cache's first build (no watermark yet). It used to echo `?full=` only.
The audit row records the same value (column `full`).

Refreshes run **one at a time per DuckLake catalog** - populate script, retention and audit row - and so
do compaction, garbage collection, `drop-table` and `?target=cache&validate_only=1`. A refresh of another
table waits for the running one; a second refresh of the SAME table answers `already_running` at once.
Two populate templates that `DETACH DATABASE IF EXISTS src; ATTACH ... AS src` used to run in parallel
and collide on the alias ("Database name src was already used by a different database for this meta
transaction"), which invalidates the DuckDB database (see `503 database_invalidated` below).
Serialisation prevents that crash; still give each populate template its own alias (`src_orders`,
`src_events`), because an attach is process-wide and a shared one is detached under whatever else reads it.

Errors are JSON `{"error": ..., "reason": ...}`:

| Status | When |
|--------|------|
| `200` | refreshed, or already running |
| `400` | cache not enabled (`"error": "Cache is not enabled for this endpoint"`), or the refresh failed (`"error": "Cache refresh failed"`, `"reason"`: the error) |
| `404` | unknown endpoint (`"error": "Endpoint not found"`) |
| `409` | `ducklake.read-only: true`: `{"error":"read_only_cache","message":"... refresh it from the writer, e.g. `flapi cache refresh`"}` |
| `503` | `database_invalidated`: the DuckDB database was invalidated by an internal error; restart flapi |

The `/cache/*` routes (`cache`, `cache/template`, `cache/refresh`, `cache/gc`, `cache/audit`) count only
endpoints **that have a cache** when a path is shared: a cached `GET /orders` beside an uncached
`POST /orders` resolves to the GET without `?method=`. Several cached endpoints on one cache table are one
cache. `?method=` is needed only when cached endpoints on the path use different tables.

A refresh through this API counts as the cache's last refresh for the scheduler: the next scheduled
refresh is one `schedule` later.

**CLI:** `flapii cache refresh /customers [--full]`

---

#### POST /api/v1/_config/endpoints/{slug}/cache/gc

Apply the cache's retention policy now (`cache.retention`, else `ducklake.retention`): expire the
snapshots it allows, then delete the files no snapshot references any more. Only this table's snapshots
are expired, including those of earlier incarnations (each CTAS full refresh and `--full` rebuild gives
the table a new id). A file stays on disk while any retained snapshot can still see it, including another
table's. It is deleted when that snapshot expires.

**Request:**
```http
POST /api/v1/_config/endpoints/-customers/cache/gc HTTP/1.1
Authorization: Bearer <token>
```

**Response (200):**
```json
{
  "status": "expired",
  "expired_snapshots": 3,
  "files_deleted": 3,
  "bytes_freed": 1048576,
  "message": "Expired 3 snapshot(s), deleted 3 file(s), freed 1048576 bytes"
}
```

`status` is `expired`, `nothing_to_expire`, `no_policy` or `error`. `no_policy` has the message
`no retention policy configured; nothing expired (set cache.retention or ducklake.retention)`.
`bytes_freed` is the size of the data and delete files the expiry released.
`warning` is present when snapshots beyond the policy are shared with another table and were kept
(expiring them would discard that table's data too).

| Status | When |
|--------|------|
| `200` | `expired`, `nothing_to_expire`, `no_policy` |
| `404` | unknown endpoint |
| `409` | `ducklake.read-only: true` (`read_only_cache`: "snapshots are expired by the writer: ...") |
| `500` | `status: "error"`; the expiry failed (the audit row has the scrubbed error) |

**CLI:** `flapii cache gc /customers`

---

#### POST /api/v1/_config/cache/drop-table

Drop a cache table that no endpoint uses any more. A changed `schema:` or `table:` leaves the old one
behind, and no endpoint's retention matches it. The table is dropped. Every snapshot since its first one
that touched no live table is expired (its own history, the `DROP`, a `CREATE SCHEMA`). Then the files no
snapshot references are deleted. Snapshots of live tables are left to their own retention. Its last files
are deleted once those snapshots expire too. An audit row with `sync_type` `drop_table` is recorded.

```http
POST /api/v1/_config/cache/drop-table HTTP/1.1
Authorization: Bearer <token>
Content-Type: application/json

{"schema": "old", "table": "customers_cache"}
```

`schema` defaults to `main`.

**Response (200):**
```json
{"status": "dropped", "schema": "old", "table": "customers_cache", "expired_snapshots": 4,
 "files_deleted": 2, "bytes_freed": 524288, "message": "Dropped old.customers_cache; ..."}
```

| Status | When |
|--------|------|
| `400` | `invalid_request` (no `table`), or `ducklake_disabled` |
| `404` | `table_not_found` |
| `409` | `cache_table_in_use` (a configured endpoint caches into it), `protected_table` (schema `audit`), `read_only_cache` |

Snapshots in the dropped table's window that touched a live table belong to that table's history. Its
retention policy is applied right away (not at its next refresh), so the dropped table's files go as soon
as that policy allows. Whatever it still keeps is named in `pinned_by`, one entry per endpoint, e.g.
`"/orders (main.orders_cache: 2 snapshot(s) kept by retention keep-last-snapshots 2)"`, and in `message`.
An endpoint without a retention policy is named with `no retention policy`.

**CLI:** `flapii cache drop-table old.customers_cache`

---

#### POST /api/v1/_config/cache/compact

Run DuckLake compaction now: merge adjacent small files, rewrite delete-heavy files
(`compaction.delete-threshold`), and delete the files they replaced. The scheduled job
(`ducklake.compaction.schedule`) does the same for the whole catalog. This route runs whether or not
`compaction.enabled` is set.

```http
POST /api/v1/_config/cache/compact HTTP/1.1
Authorization: Bearer <token>
Content-Type: application/json

{"endpoint": "/orders", "method": "GET"}
```

No body (or `{}`) compacts the whole catalog. `endpoint` (with an optional `method`, picked like the
`/cache/*` routes) compacts only that endpoint's cache table.

**Response (200):**
```json
{"status": "compacted", "duration_ms": 84, "schema": "main", "table": "orders_cache",
 "message": "Compacted main.orders_cache"}
```

`files_rewritten` is present when DuckLake reports how many files it replaced. An audit row with
`sync_type` `compaction` is recorded: a catalog-wide run under an empty `cache_table` AND an empty
`cache_schema` (it used to be `main`), a table run under that table.

| Status | When |
|--------|------|
| `200` | compacted |
| `400` | `ducklake_disabled`, `invalid_request`, or `no_cache` (the endpoint has no cache) |
| `404` | `endpoint_not_found` |
| `409` | `read_only_cache` |
| `500` | `status: "error"`: a compaction step failed (`message` names it) |

**CLI:** `flapii cache compact [/orders] [--method GET]`

---

#### GET /api/v1/_config/endpoints/{slug}/cache/audit

Get cache synchronization audit log for an endpoint.

**Request:**
```http
GET /api/v1/_config/endpoints/-customers/cache/audit HTTP/1.1
Authorization: Bearer <token>
```

**Response (200):**
```json
[
  {
    "event_id": "d774841d-2cfe-4e55-9be3-c29313c8245b",
    "endpoint_path": "/customers",
    "cache_schema": "analytics",
    "cache_table": "customers_cache",
    "sync_type": "merge",
    "status": "success",
    "message": "Cache refreshed successfully",
    "snapshot_id": "9",
    "rows_affected": 42,
    "sync_started_at": "2025-01-10T14:30:00Z",
    "sync_completed_at": "2025-01-10T14:30:05Z",
    "duration_ms": 5234,
    "full": false
  }
]
```

`full` is `true` for a refresh that loaded the whole source (a forced `--full`, a full-mode cache, or an
incremental cache's first build). The column is added to an existing audit table (`ALTER TABLE ... ADD
COLUMN IF NOT EXISTS "full"`) when a writer starts; rows written before read `false`.

---

#### GET /api/v1/_config/cache/audit

Get all cache synchronization audit logs.

**Request:**
```http
GET /api/v1/_config/cache/audit HTTP/1.1
Authorization: Bearer <token>
```

| Query parameter | Description |
|---|---|
| `table` | Optional. One cache table, `schema.table` or `table`: its events whichever endpoint sharing the table triggered them. |
| `sync_type` | Optional. Only events of this kind: `full`, `append`, `merge`, `garbage_collection`, `compaction`, `drop_table`. Combinable with `table`. Compaction is catalog-wide and filed under no endpoint, so `?sync_type=compaction` is where its history is read. |

**Response (200):** Array of audit log entries for all endpoints (or for `table`).

`event_id` is a UUIDv4. `sync_type` is the refresh mode (`full`, `append`, `merge`),
`garbage_collection`, `compaction` or `drop_table`. `rows_affected` is the sum over the template's statements. The log is bounded: the
newest 1000 rows per cache table are kept.

> **Implementation:** `src/config_service.cpp`, `src/cache_manager.cpp` | **Tests:** `test/integration/test_ducklake_cache.tavern.yaml`, `test/integration/test_ducklake_comprehensive.tavern.yaml`

---

### 2.5 Schema Introspection

#### GET /api/v1/_config/schema

Retrieve database schema information.

**Query Parameters:**

| Parameter | Type | Description |
|-----------|------|-------------|
| `tables` | boolean | Return only the table list (tables, not views) |
| `connections` | boolean | Return the connections `flapi.yaml` declares |
| `format` | string | `completion` for flattened format |
| `connection` | string | Filter by database name (an attached database named like the connection) |

**Request:**
```http
GET /api/v1/_config/schema HTTP/1.1
Authorization: Bearer <token>
```

**Response (200):**
```json
{
  "main": {
    "tables": {
      "customers": {
        "is_view": false,
        "columns": {
          "id": { "type": "INTEGER", "nullable": false },
          "name": { "type": "VARCHAR", "nullable": false },
          "email": { "type": "VARCHAR", "nullable": true }
        }
      }
    }
  }
}
```

**Response with `?tables=true` (200):**
```json
{
  "tables": [
    {"name": "ev_cache", "schema": "analytics", "database": "cache", "type": "table",
     "connection": "", "qualified_name": "cache.analytics.ev_cache"}
  ]
}
```

`connection` is the connection name when the table's database is attached under that name
(`ATTACH ... AS <connection>`), else empty.

**Response with `?connections=true` (200):** names and a type hint only; connection properties
(which can hold credentials) are never returned.
```json
{"connections": [{"name": "customers", "type": "parquet", "status": "configured"}]}
```

A failing introspection query answers `500` with the reason.

**CLI:** `flapii schema get [--connection <name>] [--table <name>]`, `flapii schema tables`,
`flapii schema connections`

---

#### POST /api/v1/_config/schema/refresh

Refresh the cached schema information.

**Request:**
```http
POST /api/v1/_config/schema/refresh HTTP/1.1
Authorization: Bearer <token>
```

**Response (200):**
```json
{
  "success": true,
  "message": "Schema refreshed"
}
```

**CLI:** `flapii schema refresh`

> **Implementation:** `src/config_service.cpp`, `src/database_manager.cpp` | **Tests:** `test/cpp/config_service_schema_test.cpp`, `test/integration/test_config_service_schema.py`

---

### 2.6 Server Administration

#### GET /api/v1/_config/metrics

Get runtime metrics: per-endpoint and per-MCP-tool request counts, errors and
latency; 401/403/429 rejections; per-cache refresh duration, rows and data age;
span export and Arrow counters.

**Request:**
```http
GET /api/v1/_config/metrics HTTP/1.1
Authorization: Bearer <token>
```

| Query parameter | Description |
|---|---|
| `endpoint` | Optional. An endpoint path (`/customers`) or MCP tool name. Narrows `requests.series` and `caches` to it. |

**Response (200):**
```json
{
  "tracing": {
    "enabled": true,
    "spans_exported": 10482,
    "spans_dropped": 0
  },
  "arrow": {
    "total_requests": 12,
    "successful_requests": 12,
    "failed_requests": 0,
    "total_rows": 48210,
    "active_streams": 0
  },
  "endpoints": {
    "count": 18
  },
  "requests": {
    "since_unix_ms": 1763112000000,
    "total": 1530,
    "server_errors": 2,
    "cache_unavailable": 3,
    "rejections": { "unauthorized": 4, "forbidden": 0, "rate_limited": 11 },
    "series": [
      {
        "protocol": "rest",
        "method": "GET",
        "route": "/customers",
        "requests": 1210,
        "status": { "2xx": 1193, "3xx": 0, "4xx": 12, "5xx": 2, "503_cache": 3 },
        "latency_ms": { "avg": 3.1, "p50": 2.5, "p95": 10.0, "p99": 25.0, "max": 41.7 },
        "last_request_unix_ms": 1763115600000
      },
      {
        "protocol": "mcp",
        "tool": "customer_lookup",
        "requests": 320,
        "failures": 0,
        "latency_ms": { "avg": 4.0, "p50": 5.0, "p95": 10.0, "p99": 10.0, "max": 9.2 },
        "last_request_unix_ms": 1763115590000
      }
    ]
  },
  "caches": [
    {
      "cache": "cache.analytics.customers_cache",
      "state": "ready",
      "endpoint": "/customers",
      "refreshes": 24,
      "failures": 0,
      "last": {
        "mode": "merge",
        "status": "success",
        "full": false,
        "duration_ms": 812,
        "rows_affected": 37,
        "snapshot_id": "118",
        "completed_at": "2026-11-14 10:00:00.812"
      },
      "duration_ms": { "avg": 790, "max": 1404 },
      "data_age_seconds": 412
    }
  ]
}
```

`requests` series are keyed by **route template** (never a filled path), so their
number is bounded by your endpoints. Unmatched paths share the route `<unmatched>`.
REST series count status classes; a 401/403 an endpoint's auth answered is counted
in its route's `status.4xx` and in `rejected`. MCP tool series count `failures`
(calls whose tool result reported an error) and `rejected` (role denials, 403),
which are not failures and not in `server_errors`. Latency is measured server-side
from the start of the request (the first middleware) to the response, for every
outcome, so authentication (a ~165 ms PBKDF2 check on a Basic-auth endpoint's first
request with a password), rate limiting and rejections are inside it; it used to
start in the handler and miss them. MCP tool series include the MCP authentication
too (a task runs on a worker and is timed from its start, so its tool series holds the
execution time only). An async MCP task additionally has its own series with
`protocol: "mcp_task"` and the same `tool` name: its latency is **end to end**, from the moment
the task was submitted (including any wait in the queue behind busy workers) to its terminal
state; `requests` counts finished tasks and `failures` the ones that failed. These series are
left out of `total` and `server_errors`, because the call is already counted by the tool
series. Values are milliseconds
rounded to three decimals. Percentiles are **estimates** from a fixed histogram with ten
buckets per decade (bounds 1, 1.25, 1.6, 2, 2.5, 3.2, 4, 5, 6.3, 8 x 10^n, from
0.1 ms to 60 s): the quantile's bucket is found and the value interpolated linearly
inside it, never above the observed `max`. A percentile is therefore accurate to
about one bucket width (~25%), not an exact sample.
A `503` whose body has an `"error"` starting with `cache_` (`cache_warming`,
`cache_build_failed`, `cache_not_built`) is a cache that is not ready yet, not a
server fault: it is counted under `status.503_cache` per series and in
`requests.cache_unavailable`, and excluded from `5xx` and `server_errors`.
`rejections` counts every 401, 403 and 429 the server returned, including those
from middleware before an endpoint ran.

`caches` lists every cache the server knows about, refreshed or not. Each row has an `endpoint`: the
endpoint that last refreshed it, else the first configured endpoint caching into that table. So
`?endpoint=/x` also matches before the first refresh in this process. `state` is
`starting`, `ready`, `stale` (the last refresh failed and the previous snapshot is
still served), `failed` or `unbuilt`. A cache that has never been refreshed in
this process has no `last` or `duration_ms`; its `data_age_seconds` is the age of the
newest snapshot in the catalog (looked up once), so an old snapshot served after a
restart whose refresh failed still shows its age. `duration_ms`
times the populate statement only. `rows_affected` is the sum of the rows written
by every statement in the cache template. The same values are written to the cache
audit log, `GET /api/v1/_config/endpoints/{path}/cache/audit`.

**Response (401):** the JSON every config route answers (see [Authentication](#authentication)).

All counters are cumulative since process start (`requests.since_unix_ms`) and
are not persisted across restarts. `endpoints.count` is present only
when a configuration is loaded.

`spans_dropped` counts failed export batches. A rising value means the collector
is slow or unreachable. Note its scope: spans discarded because the batch queue
was already full are dropped before export is attempted and are **not** counted
here.

> **Implementation:** `src/config_service.cpp`, `src/runtime_metrics.cpp`, `src/cache_manager.cpp` | **Tests:** `test/integration/test_config_service_metrics.py`, `test/integration/test_runtime_metrics_and_cache_audit.py`, `test/cpp/runtime_metrics_test.cpp`

---

#### GET /api/v1/_config/health

Server health with details: database, endpoint count, Arrow IPC, storage backends and credential
status, and the server's build version (`server.version`, the same as `flapi --version`).

```json
{"status": "healthy", "server": {"name": "flapi", "version": "26.10.31"}, "database": {"status": "connected"}, "endpoints": {"count": 7}}
```

**CLI:** `flapii health`

---

#### GET /api/v1/_config/log-level

Get the current log level.

**Request:**
```http
GET /api/v1/_config/log-level HTTP/1.1
Authorization: Bearer <token>
```

**Response (200):**
```json
{
  "level": "info"
}
```

**CLI:** `flapii config log-level get`

---

#### PUT /api/v1/_config/log-level

Set the server log level.

**Request:**
```http
PUT /api/v1/_config/log-level HTTP/1.1
Authorization: Bearer <token>
Content-Type: application/json

{
  "level": "debug"
}
```

**Valid Levels:** `debug`, `info`, `warning`, `error`, case-insensitive; `warn` is accepted as
`warning`, as with `--log-level`, `FLAPI_LOG_LEVEL` and `flapi.yaml`. The answer names the canonical level.

**Response (200):**
```json
{
  "level": "debug",
  "message": "Log level updated successfully"
}
```

**CLI:** `flapii config log-level set debug`

> **Implementation:** `src/config_service.cpp` | **Tests:** `test/integration/test_mcp_methods.py` (GET only), PUT: `test/integration/test_config_service_followups.py`

---

### 2.7 Filesystem Operations

#### GET /api/v1/_config/filesystem

Get the project filesystem structure.

**Request:**
```http
GET /api/v1/_config/filesystem HTTP/1.1
Authorization: Bearer <token>
```

**Response (200):**
```json
{
  "base_path": "/home/user/project",
  "config_file": "flapi.yaml",
  "config_file_exists": true,
  "template_path": "/home/user/project/sqls",
  "tree": [
    {
      "name": "customers.yaml",
      "type": "file",
      "path": "customers.yaml",
      "extension": ".yaml",
      "yaml_type": "endpoint",
      "url_path": "/customers"
    },
    {
      "name": "customers.sql",
      "type": "file",
      "path": "customers.sql",
      "extension": ".sql"
    }
  ]
}
```

---

#### GET /api/v1/doc.yaml

Get OpenAPI/Swagger documentation.

**Request:**
```http
GET /api/v1/doc.yaml HTTP/1.1
```

**Response (200):** OpenAPI YAML specification

**Note:** This endpoint does not require authentication.

> **Implementation:** `src/config_service.cpp` | **Tests:** `test/cpp/config_service_filesystem_test.cpp`, *OpenAPI endpoint not tested*

---

## 3. CLI Client (flapii)

### 3.1 Installation

```bash
# From the cli directory
cd cli
npm install
npm link

# Verify installation
flapii --version
```

### 3.2 Global Options

| Option | Short | Type | Default | Description |
|--------|-------|------|---------|-------------|
| `--config` | `-c` | string | - | Path to flapi.yaml configuration file |
| `--base-url` | `-u` | string | - | Base URL of the flapi server |
| `--auth-token` | - | string | - | Bearer token for authentication |
| `--config-service-token` | `-t` | string | - | Config service authentication token |
| `--timeout` | - | number | - | Request timeout in seconds |
| `--retries` | - | number | - | Number of retries for failed requests |
| `--insecure` | - | boolean | `false` | Disable TLS certificate verification |
| `--output` | `-o` | string | `json` | Output format: `json` or `table` |
| `--json-style` | - | string | `camel` | JSON key casing of config documents: `camel` or `hyphen` (see 3.5) |
| `--debug-http` | - | boolean | `false` | Enable HTTP debugging |
| `--quiet` | `-q` | boolean | `false` | Suppress non-error output |
| `--yes` | `-y` | boolean | `false` | Assume yes for prompts |

### 3.3 Environment Variables

| Variable | Description |
|----------|-------------|
| `FLAPI_BASE_URL` | Base URL of the flapi server |
| `FLAPI_URL` | Base URL, read when `FLAPI_BASE_URL` is unset |
| `FLAPI_TOKEN` | Bearer authentication token |
| `FLAPI_CONFIG_SERVICE_TOKEN` | Config service authentication token |
| `FLAPI_TIMEOUT` | Request timeout in seconds |
| `FLAPI_RETRIES` | Number of retries |
| `FLAPI_GEMINI_KEY` | Google Gemini API key for AI-assisted endpoint creation |

### 3.4 Commands Reference

#### ping

Check server connectivity.

```bash
flapii ping
```

**API:** `GET /api/v1/_config/project`

---

#### config show

Display current CLI configuration.

```bash
flapii config show
```

---

#### config validate

Validate endpoint YAML files locally.

```bash
flapii config validate                     # template.path of ./flapi.yaml
flapii -c my/flapi.yaml config validate    # template.path of that file
flapii config validate -f endpoint.yaml
flapii config validate -d ./sqls
```

| Option | Short | Description |
|--------|-------|-------------|
| `--file` | `-f` | Specific YAML file to validate |
| `--dir` | `-d` | Directory containing YAML files (default: `template.path` of the global `--config`, else of `./flapi.yaml`, resolved against that file's directory) |

Exits `1` when validation fails, when no YAML file is found, or when the `--config` file cannot be read
or has no `template.path`.

---

#### config log-level

Manage server log level.

```bash
flapii config log-level get
flapii config log-level set <level>
flapii config log-level list
```

**Levels:** `debug`, `info`, `warning` (alias `warn`), `error`; `list` shows the alias.

---

#### project get

Get project configuration from server.

```bash
flapii project get
flapii project get --output table
```

---

#### project init

Initialize a new flapi project.

```bash
flapii project init
flapii project init my-api
flapii project init --force
```

| Option | Description |
|--------|-------------|
| `--force` | Overwrite existing files |
| `--skip-validation` | Skip config validation after setup |
| `--no-examples` | Create minimal structure |
| `--advanced` | Include additional templates |
| `--ai` | Use AI assistance (requires Gemini API key) |

**Creates:**
```
project/
├── flapi.yaml
├── sqls/
│   ├── sample.yaml
│   └── sample.sql
├── data/
│   └── sample.csv      # the data the sample endpoint reads
├── common/
└── .gitignore
```

---

#### endpoints list

List all endpoints.

```bash
flapii endpoints list
flapii endpoints list --output table
```

---

#### endpoints get

Get specific endpoint configuration.

```bash
flapii endpoints get /customers
flapii endpoints get /users/:id
flapii endpoints get /orders --method POST   # when several endpoints share the path
```

Every command that names an endpoint by path (`endpoints get|update|delete|parameters|validate|reload`,
`templates get|update|expand|test`, `cache get|update|template|update-template|refresh|gc|audit`) takes
`--method <METHOD>`, sent as `?method=`. Without it, a path served by several endpoints is an error that
lists the methods. `endpoints list -o table` shows `REST+Tool` (or `REST+Resource`) for a file that is
both a REST route and an MCP entity.

---

#### endpoints create

Create a new endpoint.

```bash
flapii endpoints create -f endpoint.yaml
cat endpoint.json | flapii endpoints create --stdin
```

| Option | Short | Description |
|--------|-------|-------------|
| `--file` | `-f` | JSON/YAML file with endpoint config |
| `--stdin` | - | Read config from stdin |

---

#### endpoints update

Update an existing endpoint.

```bash
flapii endpoints update /customers -f endpoint.yaml
```

---

#### endpoints delete

Delete an endpoint.

```bash
flapii endpoints delete /customers
flapii endpoints delete /customers --force
```

---

#### endpoints validate

Validate endpoint configuration.

```bash
flapii endpoints validate /customers -f endpoint.yaml
```

---

#### endpoints reload

Reload endpoint from disk.

```bash
flapii endpoints reload /customers
flapii endpoints reload /orders --method POST
flapii endpoints reload --file sqls/new_endpoint.yaml    # reload by file, or LOAD a new file
```

`--file` calls `POST /api/v1/_config/endpoints/reload-file`: it reloads every endpoint the YAML
declares, and loads a hand-written file the server has not loaded yet (`endpoints create -f` of such a
file answers 409 and points here).

---

#### endpoints wizard

Interactive endpoint creation with optional AI assistance.

```bash
flapii endpoints wizard
flapii endpoints wizard --ai
flapii endpoints wizard --output-file endpoint.yaml
flapii endpoints wizard --dry-run
```

| Option | Description |
|--------|-------------|
| `--ai` | Use AI generation (requires Gemini API key) |
| `--output-file` | Save to file instead of creating |
| `--skip-validation` | Skip validation before save |
| `--dry-run` | Preview without creating |

---

#### templates list

List all templates.

```bash
flapii templates list
```

---

#### templates get

Get template content.

```bash
flapii templates get /customers
```

---

#### templates update

Update template content.

```bash
flapii templates update /customers -f template.sql
cat template.sql | flapii templates update /customers --stdin
```

---

#### templates expand

Expand template with parameters.

```bash
flapii templates expand /customers -p '{"id":"123"}'
flapii templates expand /customers --params-file params.json
```

| Option | Short | Description |
|--------|-------|-------------|
| `--params` | `-p` | JSON parameters inline |
| `--params-file` | - | Read parameters from file |

---

#### templates test

Test template with parameters.

```bash
flapii templates test /customers -p '{"id":"123"}'
```

---

#### cache list

List all cache configurations.

```bash
flapii cache list
```

---

#### cache get

Get cache configuration.

```bash
flapii cache get /customers
```

---

#### cache update

Update cache configuration.

```bash
flapii cache update /customers --schedule 1h
flapii cache update /customers -f cache.json
```

| Option | Short | Description |
|--------|-------|-------------|
| `--enabled` | `-e` | Enable/disable caching |
| `--schedule` | `-s` | Refresh schedule, e.g. `5m`, `6h` |
| `--ttl` | - | Not supported: flAPI has no TTL; use `--schedule` |
| `--table` | - | Cache table name |
| `--schema` | - | Cache schema name |
| `--file` | `-f` | JSON config file |
| `--stdin` | - | Read from stdin |

---

#### cache template

Get cache populate template.

```bash
flapii cache template /customers
```

---

#### cache update-template

Update cache populate template.

```bash
flapii cache update-template /customers -f cache.sql
```

---

#### cache refresh

Refresh a cache now (`POST .../cache/refresh`).

```bash
flapii cache refresh /customers
flapii cache refresh /customers --full    # drop the table and rebuild it from scratch (?full=true)
```

flapii prints the outcome the server returns: mode, status, rows, duration and snapshot (and
`Full rebuild: yes` with `--full`); `-o json` prints the response body. The history is in
`flapii cache audit /customers -o table`. `-f/--force` is
deprecated and hidden: it is a normal refresh and prints a warning.

---

#### cache audit

Show the refresh history of one cache, or of all caches without a path.

```bash
flapii cache audit /customers -o table
flapii cache audit --table analytics.orders_cache -o table   # one cache table, any endpoint
flapii cache audit                         # all caches, JSON
```

`-o table` includes a `message` column (truncated to 60 characters): the reason a refresh failed, or a
warning it ran with (a dropped cursor watermark).

---

#### cache compact

Run DuckLake compaction now (`POST /api/v1/_config/cache/compact`), for the whole catalog or for one
endpoint's cache table.

```bash
flapii cache compact                  # the whole catalog
flapii cache compact /orders          # only /orders' cache table
flapii cache compact /orders --method GET
```

Prints the server's answer (`status`, `duration_ms`, `files_rewritten` when DuckLake reports it).

---

#### schema get

Get database schema.

```bash
flapii schema get
flapii schema get --connection postgres-db
flapii schema get --table customers
```

| Option | Short | Description |
|--------|-------|-------------|
| `--connection` | - | Specific connection |
| `--table` | - | Specific table |
| `--database` | `-d` | Specific database |

`--connection` and `--table` have no short form (`-c` is the global `--config`, `-t` the global
`--config-service-token`).

---

#### schema refresh

Refresh schema cache.

```bash
flapii schema refresh
flapii schema refresh --connection postgres-db --force
```

---

#### schema connections

List database connections.

```bash
flapii schema connections
```

---

#### schema tables

List tables.

```bash
flapii schema tables
flapii schema tables --connection postgres-db
```

---

#### mcp tools list

List MCP tools.

```bash
flapii mcp tools list
```

---

#### mcp tools get

Get MCP tool configuration.

```bash
flapii mcp tools get get_customers
```

---

#### mcp resources list

List MCP resources.

```bash
flapii mcp resources list
```

---

#### mcp resources get

Get MCP resource configuration.

```bash
flapii mcp resources get customer_schema
```

---

#### mcp prompts list

List MCP prompts.

```bash
flapii mcp prompts list
```

---

#### mcp prompts get

Get MCP prompt configuration.

```bash
flapii mcp prompts get analyze_customer
```

---

### 3.5 Output Formats

#### JSON Output (Default)

```bash
flapii endpoints list --output json
```

**Key Casing:** `--json-style` re-cases **configuration documents** only: `endpoints list/get`,
`mcp tools/resources/prompts`, `cache list/get`, `project get`, `templates list`.
- `--json-style camel` (default): `urlPath`, `templateSource`
- `--json-style hyphen`: `url-path`, `template-source`

**Data output** is printed exactly as the server sent it, in snake_case, whatever `--json-style`
says: `metrics`, `cache audit`, `schema`, `health`, `templates expand/test`, `endpoints validate`,
parameters, `cache gc`, filesystem, environment. Scripts read e.g. `rows_affected`, `duration_ms`,
`sync_type` from `flapii cache audit`.

#### Table Output

```bash
flapii endpoints list --output table
```

Features colored columns and formatted borders.

> **Implementation:** `cli/src/` | **Tests:** `cli/test/`

---

## 4. Common Patterns

### Error Handling

**HTTP Status Codes:**

| Code | Description | CLI Behavior |
|------|-------------|--------------|
| 200 | Success | Display result |
| 201 | Created | Display success message |
| 400 | Bad Request | Display validation error |
| 401 | Unauthorized | Display auth error, exit 1 |
| 404 | Not Found | Display not found error |
| 409 | Conflict | Display the server's reason |
| 429, 500-504 | Server Error | Auto-retry with backoff |

flapii prints errors as `[status] statusText: <server reason>`, followed by the JSON body when there
is one, e.g. `[404] Not Found: Endpoint not found`.

**Error Response Format** (endpoint create/update/delete):

```json
{
  "error": "Endpoint already exists: REST endpoint: /orders (update it with PUT, or delete it first)"
}
```

### Authentication Flow

```bash
# Option 1: Environment variable
export FLAPI_CONFIG_SERVICE_TOKEN="your-token-of-16-plus-chars"
flapii endpoints list

# Option 2: Command-line flag (-t is short for --config-service-token)
flapii -t "your-token-of-16-plus-chars" endpoints list

# Option 3: Config service token flag
flapii --config-service-token "your-token-of-16-plus-chars" endpoints list
```

---

## Appendix A: Endpoint Slug Format

Slugs identify endpoints in API paths. The format depends on endpoint type:

| Type | Format | Example |
|------|--------|---------|
| REST | the encoded `url-path` (method is not part of it) | `-customers`, `-orders` |
| MCP tool (no `url-path`) | the bare tool name | `get_customers` |
| MCP resource (no `url-path`) | the bare resource name | `schema` |
| MCP prompt (no `url-path`) | the bare prompt name | `analysis` |

**Path Conversion Rules:**

| In the path | In the slug | Why |
|---|---|---|
| `/` | `-` | keeps the common case readable |
| `-` | `~1` | so a `-` in a slug always means `/` |
| `~` | `~0` | the escape character escapes itself |
| *(empty path)* | `empty` | every non-empty path starts with `/`, so it encodes to a leading `-` |

The encoding is **injective** — distinct paths always produce distinct slugs,
and `slugToPath(pathToSlug(p)) == p` for every path. An earlier scheme was not:
it mapped every `/` and every special character to `-`, so `/order-items` and
`/order/items` shared one slug, and a `GET` → `PUT` round trip through this API
silently rewrote the first into the second.

The escape alphabet deliberately contains **no `%`**. Clients percent-encode
the slug for transport, and RFC 3986 §6.2.2.2 permits any normaliser to decode
an escaped unreserved character — so a `%`-based escape could be undone in
transit by an ordinary proxy and reintroduce that collision.

**Examples:**

| URL Path | Slug |
|----------|------|
| `/customers` | `-customers` |
| `/customers/` | `-customers-` |
| `/api/v1/users` | `-api-v1-users` |
| `/order-items` | `-order~1items` |
| `/` | `-` |

An endpoint that has a `url-path` is addressed **only** by its path slug, even
when it also declares an `mcp-tool`: `/customers` with tool `get_customers` is
`-customers`, and `GET /api/v1/_config/endpoints/get_customers` returns 404. The
bare-name form applies only to MCP entries without a `url-path`.

**One accepted form.** The slug is the only name the endpoint routes accept. A
percent-encoded url-path (`customers%2F`, `%2Fcustomers`) returns 404: the
routes match a single path segment and Crow percent-decodes before matching, so
the decoded `/` never reaches the slug lookup (`src/path_utils.cpp`,
`PathUtils::identifierToPath`).

**Clients must agree byte-for-byte.** `flapii` implements the same codec in
`cli/src/lib/url.ts`, and both it and the server are tested against the shared
fixture `test/fixtures/slug_codec.json`. That fixture exists because the two
drifted once: the CLI shipped `-slash-` while the server used `-slash`.

---

## Appendix B: HTTP Status Codes

| Code | Description |
|------|-------------|
| 200 | OK - Request successful |
| 201 | Created - Resource created |
| 400 | Bad Request - Invalid input |
| 401 | Unauthorized - Invalid or missing token |
| 404 | Not Found - Resource doesn't exist (for reload: only an unknown endpoint) |
| 405 | Method Not Allowed - read-only resource (`PUT /api/v1/_config/project`, non-GET `/config`); `Allow: GET` |
| 409 | Conflict - endpoint exists, `{{include}}` in the YAML, packed bundle, or a read-only cache |
| 500 | Internal Server Error |
| 501 | Not Implemented |
| 503 | Service Unavailable |

---

## Related Documentation

- **[Documentation index](./README.md)** - Start here; organised by what you are trying to do
- **[Configuration Reference](./CONFIG_REFERENCE.md)** - Configuration file options and format
- **[CLI Reference](./CLI_REFERENCE.md)** - Server executable command-line options
- **[MCP Reference](./MCP_REFERENCE.md)** - Model Context Protocol specification
- **[MCP Configuration Integration Guide](./MCP_CONFIG_INTEGRATION.md)** - How config tools integrate with MCP protocol
