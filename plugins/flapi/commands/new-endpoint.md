---
description: Create a REST endpoint and/or MCP tool from SQL - YAML, template, validators, a test - then validate and try it
argument-hint: "[what the endpoint should return]"
---

Create a new flAPI endpoint. Request: $ARGUMENTS

Use the `flapi-rest` skill (and `flapi-mcp` if it should also be an MCP tool or
resource, `flapi-testing` for the test, `flapi-auth` if it needs auth).

1. Find the project (`flapi.yaml`) and its connections; inspect the source schema
   (e.g. `flapii schema get` on a running config-service server, or `DESCRIBE` the
   table with DuckDB) so column names and types are real.
2. Agree the contract with the user in one short message: URL path, method,
   parameters (name, location, type, required), response shape, and whether it is
   REST only, MCP only, or both. Default to both REST and an MCP tool for reads.
3. Write `sqls/<name>.yaml` and `sqls/<name>.sql`:
   - a validator on EVERY parameter (`int`/`string` with `regex`/`enum` with
     `allowedValues`, ...) - validators are the injection defence, Mustache does
     no SQL escaping; `field-type` does nothing (load warning)
   - `field-in: header` reads the request header (case-insensitive) - a query
     parameter of the same name does not satisfy it
   - `WHERE 1=1` + `{{#params.x}} AND ... {{/params.x}}` for optional filters
   - triple braces inside quoted string literals, double braces only for validated
     numbers; no `LIMIT` in the SQL (pagination wraps it): bound rows with a
     `limit` request field (`int`, default, max) or `mcp-tool.response.max-rows`
   - for MCP: a description an agent can act on (what it returns, when to use it,
     parameter meanings and units)
4. Validate with `flapi -c flapi.yaml --validate-config`; render the SQL with
   `flapii templates expand <path> --params '{...}'` when a server is running.
5. Call it (curl the REST path; `tools/call` for MCP) with valid and invalid input
   and show both results.
6. Add a Tavern test under the project's tests (or `test/integration/` in the flAPI
   repo) covering a success and a 400.
