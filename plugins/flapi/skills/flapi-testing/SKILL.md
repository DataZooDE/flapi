---
name: flapi-testing
description: >
  Testing a flAPI project: `flapi --validate-config` (what it catches and what it
  misses), `flapii templates expand` / `templates test` / `endpoints validate`,
  Tavern REST tests (pagination envelope, 400 bodies, writes, auth), MCP tests
  with pytest + requests against /mcp/jsonrpc, a uv-managed test environment,
  and a GitHub Actions CI job that installs flapi-io, starts the server and runs
  the suite. Use when a user wants to check an endpoint before deploying, write
  API or MCP tests, or set up CI for a flAPI repository.
version: 26.11.26
user-invocable: false
---

# flAPI Testing Cheat Sheet

**flAPI:** v26.11.26 | **Skill:** Develop | **See also:** `flapi-rest`, `flapi-mcp`, `flapi-auth`, `flapi-live-config`, `flapi-deploy`

Three layers, cheapest first: static validation (no database), template checks
against a running server, then HTTP/MCP tests against a running server.

## Critical gotchas

- **`--validate-config` never opens the database or renders a template.** SQL
  errors, missing tables, misspelt variable names, failing connection `init` and
  wrong column names all pass; they surface as 500s at request time. Always run
  at least one real request per endpoint. It does check Mustache *syntax* of every
  serving and cache template (unbalanced/crossed sections, `{{{` without `}}}`,
  partials, `{{= =}}`) and exits 1 with `<file>:<line>:<col>`.
- **A missing template file is only a warning; exit code stays 0.** Errors
  (bad `url-path`, unknown connection) exit 1. CI that must fail on warnings has
  to grep the output.
- **Unknown keys inside endpoint files are silently ignored** (`methd:`,
  `requird:`); only unknown TOP-level keys of `flapi.yaml` and a few known-inert
  keys (`field-type`, `server-name`, connection `log-queries`) log a warning. A
  typo'd optional key validates fine and does nothing.
- **`POST .../template/expand?validate_only=1` checks SQL without running it**
  (add `target=cache` for the populate template, rendered with the live watermark):
  each statement is parsed, then bound in a rolled-back transaction - unknown
  columns/tables, `MERGE INTOO`, a bare SELECT in a populate template - and the
  answer is `{valid, errors[{statement, type, kind, message}], warnings, expanded}`.
  SQL inside `{{#params.x}}` is only checked when `x` is in `parameters`; the
  plugin hook sends a sample value per typed field. A cache not built yet is a
  `cache_not_built` warning, not an error.
- **`flapii templates expand` skips validators and pastes values in**; the real
  request binds `{{ params.x }}` as `?`. Use `templates test` (validates, then
  runs the SQL) or a real HTTP call to see true behaviour.
- **`flapii endpoints validate` runs the server's real endpoint parser**, so a
  broken validator (`Missing required key: values (or allowedValues)`) fails
  there too. Its path argument is the endpoint, the YAML comes from `--file` or
  stdin (neither: `No YAML given: pass --file <path> or pipe the YAML on stdin`).
- **Every `flapii` command needs the server started with `--config-service`**
  and the same token in `FLAPI_CONFIG_SERVICE_TOKEN` (or `-t`).
- **Endpoint YAML is read at startup.** Restart (or `flapii endpoints reload
  <path>`) before re-running tests after a YAML change; `.sql` edits apply live.
- **Write tests are not idempotent** against a persistent database: point the
  test server at a throwaway file (fresh CI checkout, or delete it first).
- **Security warnings go to stderr under `SECURITY WARNINGS (n)`** and never fail
  validation; read them (see `flapi-auth`).

## Layer 1: static validation

```bash
flapi -c flapi.yaml --validate-config
echo "exit=$?"
```

| Caught (exit 1 or startup error) | Reported as warning (exit 0) | Not checked at all |
|---|---|---|
| YAML syntax in `flapi.yaml` or any endpoint file | template file does not exist | SQL syntax, tables, columns |
| missing `project-name` | `No database connection specified` | Mustache variable names (`{{ params.typo }}` renders empty) |
| `{{env.X}}` not in `template.environment-whitelist` | unknown top-level key (log line) | connection `init` SQL, extensions |
| `url-path` not starting with `/` or containing `//`, no `url-path`/`mcp-*` | security warnings (stderr) | unknown keys inside endpoint files |
| `connection:` naming an undefined connection | `field-type`, other known-inert keys | |
| missing required validator keys (`allowedValues`/`values`) | | DuckLake / cache SQL |
| bad `log-level`, `compaction.schedule`, `scan-interval`, OIDC placeholder | | |
| empty Basic-auth password from an unset env var | | |
| Mustache syntax in a template (unclosed `{{#x}}`: `<file>:<line>:<col>`) | | |
| two files with one route + method (`Duplicate endpoint ...`; `get` = `GET`) | overlapping routes, same auth (info) or allowed by `allow-shadowing` (WARNING) | |
| overlapping routes whose effective auth differs in any way, either direction, incl. other users/secrets (`Overlapping routes with different auth`) | | |
| `url-path` with characters outside `[A-Za-z0-9._~-]` or `:name` segments (`{id}` -> use `:id`), a `.`/`..` segment or a repeated `:name`; a method other than GET/POST/PUT/PATCH/DELETE | | |

Fail CI on warnings too:

```bash
rc=0; out=$(flapi -c flapi.yaml --validate-config 2>&1) || rc=$?
echo "$out"
[ $rc -eq 0 ] && ! grep -qE '\[WARNING|WARNING:' <<<"$out"
```

That matches log warnings (`[WARNING ]`) and endpoint warnings (`WARNING:`),
not the `SECURITY WARNINGS` banner; add `|SECURITY WARNINGS` to fail on those.

## Layer 2: template checks (server running)

```bash
export FLAPI_CONFIG_SERVICE_TOKEN=dev-token-0123456789
flapi -c flapi.yaml --config-service &        # same token picked up from the env

flapii templates expand /customers --params '{"nation":"5"}'          # JSON {"expanded": "..."}
flapii templates expand /customers --params '{"nation":"5"}' -o table # raw SQL
flapii templates test /customers --params '{"nation":"5"}'            # validates, runs: {columns, rows}
flapii endpoints validate /customers --file sqls/customers.yaml       # {valid, errors, warnings}
flapii endpoints list
flapii endpoints parameters /customers
```

`--params-file params.json` works instead of `--params`. `templates test`
answers HTTP 400 for a validator failure or a SQL error; `flapii` prints
`[400] Bad Request: <reason>` and the JSON body (`errors`, `error`).

## The endpoint under test (runnable)

```yaml
# flapi: endpoint group=suite allow=MCP_UNAUTHENTICATED_TOOLS file=customers.yaml
url-path: /customers
method: GET
mcp-tool:
  name: list_customers
  description: List customers, optionally filtered by nation key (0-24).
request:
  - field-name: nation
    field-in: query
    description: Nation key 0-24
    validators:
      - type: int
        min: 0
        max: 24
template-source: customers.sql
connection: [customers]
# flapi: check GET /customers?limit=2 -> 200
# flapi: check GET /customers?nation=99 -> 400
# flapi: check POST /mcp/jsonrpc body={"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"list_customers","arguments":{"nation":5}}} -> 200
```

```sql
-- flapi: template group=suite file=customers.sql
SELECT c_custkey AS id, c_name AS name, c_nationkey AS nation
FROM read_parquet('{{{ conn.path }}}')
WHERE ({{ params.nation }} IS NULL OR c_nationkey = {{ params.nation }})
ORDER BY id
```

(MCP has no auth here, so startup prints `MCP_UNAUTHENTICATED_TOOLS`; fine for
a local test server.)

## Layer 3a: Tavern REST tests

Layout used below (tests live next to the project, in their own uv project):

```
flapi.yaml
sqls/customers.yaml, sqls/customers.sql
tests/pyproject.toml
tests/common.yaml
tests/test_customers.tavern.yaml
tests/test_mcp.py
```

```toml
# tests/pyproject.toml
[project]
name = "my-api-tests"
version = "0.1.0"
requires-python = ">=3.11"
dependencies = ["tavern>=3", "requests>=2.31"]
```

```text
# tests/common.yaml
name: common
description: Shared variables for every Tavern test
variables:
  base_url: "{tavern.env_vars.FLAPI_URL}"
```

```text
# tests/test_customers.tavern.yaml
test_name: customers endpoint

includes:
  - !include common.yaml

stages:
  - name: first page has the envelope
    request:
      url: "{base_url}/customers"
      method: GET
      params:
        limit: "2"
    response:
      status_code: 200
      headers:
        content-type: application/json
      json:
        total_count: 1500
        next: "/customers?offset=2&limit=2"
        data:
          - id: 1
            name: "Customer#000000001"
            nation: !anyint
          - id: 2
            name: !anystr
            nation: !anyint

  - name: filter narrows the total
    request:
      url: "{base_url}/customers"
      method: GET
      params:
        nation: "5"
        limit: "1"
    response:
      status_code: 200
      json:
        total_count: 57
        next: !anystr
        data: !anylist

  - name: out-of-range value is rejected before SQL
    request:
      url: "{base_url}/customers"
      method: GET
      params:
        nation: "99"
    response:
      status_code: 400
      json:
        errors:
          - field: nation
            message: Integer is greater than the maximum allowed value (max 24)
```

Tavern's `json:` must match the whole top-level structure (extra keys in a
nested object are allowed, a missing list item is not). Write, auth and
error-body patterns: [references/tavern-patterns.md](references/tavern-patterns.md).

## Layer 3b: MCP tests with pytest

```python
# tests/test_mcp.py
import json
import os

import requests

BASE = os.environ.get("FLAPI_URL", "http://127.0.0.1:8080")


def rpc(method, params=None, auth=None):
    resp = requests.post(
        f"{BASE}/mcp/jsonrpc",
        json={"jsonrpc": "2.0", "id": 1, "method": method, "params": params or {}},
        headers={"Accept": "application/json, text/event-stream"},
        auth=auth,          # ("agent", os.environ["AGENT_PASSWORD"]) when mcp.auth is basic
        timeout=30,
    )
    return resp.status_code, resp.json()


def test_tool_is_listed():
    status, body = rpc("tools/list")
    assert status == 200
    tools = {t["name"]: t for t in body["result"]["tools"]}
    assert "nation" in tools["list_customers"]["inputSchema"]["properties"]


def test_tool_call_returns_rows():
    status, body = rpc("tools/call", {"name": "list_customers", "arguments": {"nation": 5}})
    assert status == 200, body
    result = body["result"]
    assert not result.get("isError"), result
    rows = result["structuredContent"]["rows"]
    assert rows and all(r["nation"] == 5 for r in rows)
    assert json.loads(result["content"][0]["text"])[0]["nation"] == 5


def test_invalid_argument_is_a_tool_error():
    status, body = rpc("tools/call", {"name": "list_customers", "arguments": {"nation": 99}})
    assert status == 200
    assert body["result"]["isError"] is True
    assert "Integer is greater than the maximum" in body["result"]["content"][0]["text"]


def test_unknown_tool_is_a_protocol_error():
    _, body = rpc("tools/call", {"name": "nope", "arguments": {}})
    assert body["error"]["code"] == -32602
```

Shapes to assert on: a tool result is `{content:[{type:"text",text:<JSON rows>}],
structuredContent:{rows, row_count}}`; argument validation failures are
`result.isError: true` (HTTP 200); protocol errors are `error.code`; with
`mcp.auth` on, a missing login is HTTP 401 and a role miss HTTP 403 (see
`flapi-auth`). No `initialize` call is needed per test.

## Run the suite locally

```bash
flapi -c flapi.yaml -p 8080 > flapi.log 2>&1 &
until curl -fsS http://127.0.0.1:8080/health/live >/dev/null; do sleep 0.5; done
FLAPI_URL=http://127.0.0.1:8080 uv run --project tests pytest tests -v
kill %1
```

`uv run --project tests` creates `tests/.venv` from `tests/pyproject.toml` on
first use (add `tests/.venv/` to `.gitignore`). Run one file with
`... pytest tests/test_mcp.py -v`, one function with `::test_tool_is_listed`.

## GitHub Actions CI

```text
# .github/workflows/api-tests.yml
name: api-tests
on: [push, pull_request]

jobs:
  test:
    runs-on: ubuntu-latest
    env:
      FLAPI_URL: http://127.0.0.1:8080
      DATAZOO_DISABLE_TELEMETRY: "1"
    steps:
      - uses: actions/checkout@v4
      - uses: astral-sh/setup-uv@v6

      - name: Install flapi (server + flapii CLI)
        run: uv tool install flapi-io==26.10.31

      - name: Validate configuration
        run: |
          rc=0; out=$(flapi -c flapi.yaml --validate-config 2>&1) || rc=$?
          echo "$out"
          [ $rc -eq 0 ] && ! grep -qE '\[WARNING|WARNING:' <<<"$out"

      - name: Start flapi
        run: |
          flapi -c flapi.yaml -p 8080 --no-telemetry > flapi.log 2>&1 &
          for i in $(seq 1 60); do
            curl -fsS "$FLAPI_URL/health/live" >/dev/null && exit 0
            sleep 1
          done
          cat flapi.log; exit 1

      - name: Tests
        run: uv run --project tests pytest tests -v

      - name: Server log
        if: always()
        run: cat flapi.log
```

- Pin `flapi-io` to the version you develop against; the wheel carries both
  `flapi` and `flapii`. Alternatives: the `ghcr.io/datazoode/flapi` image or the
  `flapi-linux-amd64` release asset.
- Secrets the config reads through `{{env.X}}` come from repository secrets:
  `env: FLAPI_DB_PASSWORD: ${{ secrets.FLAPI_DB_PASSWORD }}` on the start step.
- The validate step ignores the `SECURITY WARNINGS` banner on purpose; add
  `|SECURITY WARNINGS` to the grep once the config has none (PBKDF2 users
  currently always trigger one, see `flapi-auth`).
- `/health/live` answers as soon as the HTTP server is up; caches may still be
  warming (503 `cache_warming`). Poll `/health` or retry if tests hit cached
  endpoints (see `flapi-cache-design`).
- For `flapii` steps start with `--config-service` and export
  `FLAPI_CONFIG_SERVICE_TOKEN` in `env:`.

## Canonical testing workflow

1. `flapi -c flapi.yaml --validate-config` until it passes with no warnings.
2. Start `flapi -c flapi.yaml --config-service`; `flapii templates test
   /<path> --params '{...}'` for each endpoint (happy path + one bad value).
3. Capture real responses with curl and turn them into Tavern stages: status,
   envelope (`data`, `next`, `total_count`), one 400 per validator.
4. For every `mcp-tool`, add `tools/list` + `tools/call` + one `isError` case.
5. For protected endpoints, one stage each for 401, wrong credentials and 200.
6. Run locally with `uv run --project tests pytest tests -v`, then add the
   workflow above.
