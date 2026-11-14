"""Templates, request parsing, validation and serialisation (#211, cluster E1).

* An unbalanced Mustache section crashed (stray `{{/x}}`: SIGSEGV) or hung the
  server (unclosed `{{#x}}` with x absent: endless loop, unbounded memory). It
  is now refused at load, reload, config-service template update and
  `--validate-config` with file:line, and a template edited on disk after load
  answers 500 instead of taking the process down.
* JSON body numbers were truncated (9.99 -> 9; 301.7 passed an `int`).
* `number` validators had no min/max, and tools/list said minimum=maximum=0.
* A DELETE endpoint used as an MCP tool ran as a read.
* limit=0 made `next` point at itself; `?format=csv` was "unknown" under
  request-fields-validation; `?x=1&x=2` silently took the first value.
* /doc.yaml typed every parameter as a string, had no limit/offset/400/X-*
  headers, a fixed "Description not available" and `properties: ~`.
* Row keys came out in hash order, doubles with 21 digits, naive TIMESTAMPs
  with a `Z`; a 0-row RETURNING omitted `data`; a missing required field was
  listed twice; MCP initialize reported version "0.3.0".
"""
import os
import shutil
import threading
import signal
import subprocess
import tempfile
import time
from types import SimpleNamespace

import pytest
import requests
import yaml

from otel_helpers import flapi_binary, free_port

pytestmark = pytest.mark.standalone_server

TOKEN = "e1-templates-token"
CS = {"Authorization": f"Bearer {TOKEN}"}

CONFIG = """\
project-name: e1-templates
template:
  path: ./sqls
connections:
  app:
    init: |
      ATTACH IF NOT EXISTS './app.duckdb' AS app;
      CREATE TABLE IF NOT EXISTS app.items (id INTEGER PRIMARY KEY, name VARCHAR, price DOUBLE);
  locked:
    serialize-access: true
    init: |
      ATTACH IF NOT EXISTS './locked.duckdb' AS locked;
      CREATE TABLE IF NOT EXISTS locked.items AS SELECT range AS id FROM range(100);
ducklake:
  enabled: true
  alias: cache
  metadata-path: ./cache.ducklake
  data-path: ./cache_data
  warmup: never
rate_limit:
  enabled: false
mcp:
  enabled: true
"""

ENDPOINTS = {
    "rows.yaml": """\
url-path: /rows
method: GET
description: Rows in a fixed column order
template-source: rows.sql
connection: [app]
request-fields-validation: true
request:
  - field-name: x
    field-in: query
    validators:
      - type: int
        min: 1
        max: 1000
  - field-name: ratio
    field-in: query
    validators:
      - type: number
        min: 0.5
        max: 2.5
  - field-name: loose
    field-in: query
    validators:
      - type: number
mcp-tool:
  name: rows
  description: Rows in a fixed column order
""",
    "rows.sql": "SELECT 'z' AS zeta, 711.56::DOUBLE AS price, 1 AS alpha, "
                "'2024-05-06 07:08:09.123456'::TIMESTAMP AS ts, "
                "'2024-05-06 07:08:09.123456+00'::TIMESTAMPTZ AS ts_tz "
                "FROM range(3)",
    "create.yaml": """\
url-path: /items
method: POST
template-source: create.sql
connection: [app]
operation:
  type: write
  validate-before-write: true
  returns-data: true
request:
  - field-name: id
    field-in: body
    required: true
    validators:
      - type: int
  - field-name: price
    field-in: body
    required: true
    validators:
      - type: number
        min: 0
  - field-name: qty
    field-in: body
    validators:
      - type: int
        min: 1
        max: 1000
""",
    "create.sql": "INSERT INTO app.items VALUES ({{ params.id }}, 'x', {{ params.price }}) RETURNING id, price",
    "delete.yaml": """\
url-path: /items/:id
method: DELETE
template-source: delete.sql
connection: [app]
operation:
  returns-data: true
request:
  - field-name: id
    field-in: path
    required: true
    validators:
      - type: int
mcp-tool:
  name: delete_item
  description: Delete one item
""",
    "delete.sql": "DELETE FROM app.items WHERE id = {{ params.id }} RETURNING id",
    "list.yaml": "url-path: /items\nmethod: GET\ntemplate-source: list.sql\nconnection: [app]\n",
    "list.sql": "SELECT id, price FROM app.items ORDER BY id",
    "editable.yaml": "url-path: /editable\nmethod: GET\ntemplate-source: editable.sql\nconnection: [app]\n",
    "editable.sql": "SELECT 1 AS ok {{#params.x}}, {{ params.x }} AS x{{/params.x}}",
    "cached.yaml": """\
url-path: /cached
method: GET
template-source: cached_serve.sql
connection: [app]
cache:
  enabled: true
  table: cached_items
  schema: main
  template-file: cached_populate.sql
""",
    "cached_serve.sql": "SELECT * FROM {{{cache.catalog}}}.{{{cache.schema}}}.{{{cache.table}}}",
    "cached_populate.sql": "CREATE OR REPLACE TABLE {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS "
                           "SELECT * FROM app.items",
    # A serialize-access connection: an uncached read holds its lock while it
    # runs; a read served from the cache must not wait for it.
    "heavy.yaml": """\
url-path: /heavy
method: GET
template-source: heavy.sql
connection: [locked]
request:
  - field-name: n
    field-in: query
    required: true
    validators:
      - type: int
        min: 1
        max: 2000000000
""",
    "heavy.sql": "SELECT sum(hash(range)) AS h FROM range({{ params.n }})",
    "locked_cached.yaml": """\
url-path: /locked_cached
method: GET
template-source: locked_serve.sql
connection: [locked]
cache:
  enabled: true
  table: locked_items
  schema: main
  template-file: locked_populate.sql
""",
    "locked_serve.sql": "SELECT count(*) AS n FROM {{{cache.catalog}}}.{{{cache.schema}}}.{{{cache.table}}}",
    "locked_populate.sql": "CREATE OR REPLACE TABLE {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS "
                           "SELECT * FROM locked.items",
}


def start_server(root, extra_args=()):
    port = free_port()
    log_path = os.path.join(root, "server.log")
    log = open(log_path, "w")
    proc = subprocess.Popen(
        [flapi_binary(), "-c", "flapi.yaml", "-p", str(port), "--log-level", "info",
         "--config-service", "--config-service-token", TOKEN, *extra_args],
        cwd=root, stdout=log, stderr=subprocess.STDOUT, preexec_fn=os.setsid,
        env={**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"},
    )
    return proc, port, log, log_path


def write_project(root, endpoints):
    os.makedirs(os.path.join(root, "sqls"), exist_ok=True)
    with open(os.path.join(root, "flapi.yaml"), "w") as f:
        f.write(CONFIG)
    for name, body in endpoints.items():
        with open(os.path.join(root, "sqls", name), "w") as f:
            f.write(body)


@pytest.fixture(scope="module")
def server():
    root = tempfile.mkdtemp(prefix="flapi_e1_")
    write_project(root, ENDPOINTS)
    proc, port, log, log_path = start_server(root)
    base = f"http://127.0.0.1:{port}"
    deadline = time.time() + 60
    while time.time() < deadline:
        if proc.poll() is not None:
            pytest.fail("server exited:\n" + open(log_path).read())
        try:
            if requests.get(f"{base}/health/live", timeout=1).status_code == 200:
                break
        except requests.RequestException:
            time.sleep(0.3)
    else:
        pytest.fail("server never became live")
    s = SimpleNamespace(base=base, root=root, log_path=log_path, proc=proc)
    try:
        yield s
    finally:
        os.killpg(proc.pid, signal.SIGTERM)
        try:
            proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            os.killpg(proc.pid, signal.SIGKILL)
        log.close()
        shutil.rmtree(root, ignore_errors=True)


def mcp(server, method, params=None):
    r = requests.post(f"{server.base}/mcp/jsonrpc", json={
        "jsonrpc": "2.0", "id": 1, "method": "initialize",
        "params": {"protocolVersion": "2024-11-05", "capabilities": {},
                   "clientInfo": {"name": "t", "version": "0"}}}, timeout=10)
    assert r.status_code == 200, r.text
    if method == "initialize":
        return r.json()
    headers = {"Mcp-Session-Id": r.headers.get("Mcp-Session-Id", "")}
    r = requests.post(f"{server.base}/mcp/jsonrpc", headers=headers, json={
        "jsonrpc": "2.0", "id": 2, "method": method, "params": params or {}}, timeout=30)
    assert r.status_code == 200, r.text
    return r.json()


def sqls(server, name):
    return os.path.join(server.root, "sqls", name)


# ---------------------------------------------------------------------------
# P0: unbalanced templates


class TestTemplateBalance:
    @pytest.mark.parametrize("bad", ["SELECT 1 {{/x}}", "SELECT {{ params.y }} {{#params.x}} AND 1"])
    def test_template_edited_on_disk_answers_500_not_a_crash(self, server, bad):
        path = sqls(server, "editable.sql")
        original = open(path).read()
        try:
            with open(path, "w") as f:
                f.write(bad)
            r = requests.get(f"{server.base}/editable", timeout=10)
            assert r.status_code == 500, r.text
            assert server.proc.poll() is None, "server died"
            assert requests.get(f"{server.base}/health/live", timeout=5).status_code == 200
            # And the reload refuses it, naming the file and line.
            r = requests.post(f"{server.base}/api/v1/_config/endpoints/-editable/reload", headers=CS, timeout=10)
            assert r.status_code == 400, r.text
            assert "editable.sql:1:" in r.json()["reason"], r.text
        finally:
            with open(path, "w") as f:
                f.write(original)
        assert requests.get(f"{server.base}/editable", params={"x": 2}, timeout=10).json()["data"] == [
            {"ok": 1, "x": 2}]

    def test_config_service_template_update_refuses_unbalanced(self, server):
        before = open(sqls(server, "editable.sql")).read()
        r = requests.put(f"{server.base}/api/v1/_config/endpoints/-editable/template", headers=CS,
                         json={"template": "SELECT 1\n{{#params.x}} x"}, timeout=10)
        assert r.status_code == 400, r.text
        assert "editable.sql:2:1" in r.json()["error"], r.text
        assert "never closed" in r.json()["error"]
        assert open(sqls(server, "editable.sql")).read() == before

    def test_config_service_cache_template_update_refuses_unbalanced(self, server):
        before = open(sqls(server, "cached_populate.sql")).read()
        r = requests.put(f"{server.base}/api/v1/_config/endpoints/-cached/cache/template", headers=CS,
                         json={"template": "CREATE TABLE t AS SELECT 1 {{/x}}"}, timeout=10)
        assert r.status_code == 400, r.text
        assert "cached_populate.sql:1:" in r.json()["error"], r.text
        assert open(sqls(server, "cached_populate.sql")).read() == before

    def test_balanced_template_update_still_works(self, server):
        path = sqls(server, "editable.sql")
        before = open(path).read()
        try:
            r = requests.put(f"{server.base}/api/v1/_config/endpoints/-editable/template", headers=CS,
                             json={"template": "SELECT 7 AS ok"}, timeout=10)
            assert r.status_code == 200, r.text
            assert requests.get(f"{server.base}/editable", timeout=10).json()["data"] == [{"ok": 7}]
        finally:
            with open(path, "w") as f:
                f.write(before)


@pytest.mark.parametrize("broken_file,broken,where", [
    ("editable.sql", "SELECT 1\n\n  {{/x}}", "editable.sql:3:3"),
    ("cached_populate.sql", "CREATE OR REPLACE TABLE t AS\nSELECT 1 {{#x}}", "cached_populate.sql:2:10"),
])
def test_startup_and_validate_config_refuse_unbalanced(broken_file, broken, where):
    root = tempfile.mkdtemp(prefix="flapi_e1_bad_")
    try:
        write_project(root, {**ENDPOINTS, broken_file: broken})
        out = subprocess.run([flapi_binary(), "-c", "flapi.yaml", "--validate-config"], cwd=root,
                             capture_output=True, text=True, timeout=60,
                             env={**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"})
        text = out.stdout + out.stderr
        assert out.returncode == 1, text
        assert where in text, text

        proc, _port, log, log_path = start_server(root)
        try:
            assert proc.wait(timeout=60) == 1   # an error exit, not a signal
        finally:
            log.close()
        assert where in open(log_path).read()
    finally:
        shutil.rmtree(root, ignore_errors=True)


# ---------------------------------------------------------------------------
# Request parsing and validation


class TestBodyNumbers:
    def test_fraction_is_kept(self, server):
        r = requests.post(f"{server.base}/items", json={"id": 1, "price": 9.99}, timeout=10)
        assert r.status_code == 201, r.text
        assert r.json()["data"] == [{"id": 1, "price": 9.99}]
        rows = requests.get(f"{server.base}/items", timeout=10).json()["data"]
        assert {"id": 1, "price": 9.99} in rows

    def test_int_validator_rejects_a_fraction(self, server):
        r = requests.post(f"{server.base}/items", json={"id": 2, "price": 1, "qty": 301.7}, timeout=10)
        assert r.status_code == 400, r.text
        assert r.json()["errors"] == [{"field": "qty", "message": "Invalid integer value"}]

    def test_missing_required_field_is_listed_once(self, server):
        r = requests.post(f"{server.base}/items", json={"price": 1}, timeout=10)
        assert r.status_code == 400, r.text
        assert [e["field"] for e in r.json()["errors"]] == ["id"]

    def test_range_message_names_the_limit(self, server):
        r = requests.post(f"{server.base}/items", json={"id": 3, "price": 1, "qty": 5000}, timeout=10)
        assert r.status_code == 400, r.text
        assert r.json()["errors"][0]["message"] == "Integer is greater than the maximum allowed value (max 1000)"


class TestNumberValidator:
    def test_bounds_are_enforced(self, server):
        assert requests.get(f"{server.base}/rows", params={"ratio": "1.5"}, timeout=10).status_code == 200
        r = requests.get(f"{server.base}/rows", params={"ratio": "0.4"}, timeout=10)
        assert r.status_code == 400, r.text
        assert r.json()["errors"][0]["message"] == "Number is less than the minimum allowed value (min 0.5)"
        assert requests.get(f"{server.base}/rows", params={"ratio": "2.6"}, timeout=10).status_code == 400

    def test_tools_list_emits_bounds_only_when_set(self, server):
        tools = mcp(server, "tools/list")["result"]["tools"]
        props = next(t for t in tools if t["name"] == "rows")["inputSchema"]["properties"]
        assert props["ratio"]["minimum"] == 0.5 and props["ratio"]["maximum"] == 2.5
        assert "minimum" not in props["loose"] and "maximum" not in props["loose"]
        assert props["x"]["minimum"] == 1 and props["x"]["maximum"] == 1000


class TestQueryParameters:
    def test_limit_zero_has_no_next(self, server):
        r = requests.get(f"{server.base}/rows", params={"limit": 0}, timeout=10)
        assert r.status_code == 200, r.text
        assert r.json()["data"] == []
        assert r.json()["next"] == ""
        assert r.json()["total_count"] == 3
        assert r.headers["X-Next"] == ""

    def test_format_is_allowed_under_request_fields_validation(self, server):
        r = requests.get(f"{server.base}/rows", params={"format": "csv"}, timeout=10)
        assert r.status_code == 200, r.text
        assert r.headers["Content-Type"].startswith("text/csv")
        assert r.text.splitlines()[0] == "zeta,price,alpha,ts,ts_tz"
        assert r.text.splitlines()[1].startswith('"z",711.56,1,')
        assert requests.get(f"{server.base}/rows", params={"nope": "1"}, timeout=10).status_code == 400

    def test_duplicate_query_parameter_is_rejected(self, server):
        r = requests.get(f"{server.base}/rows?x=1&x=2", timeout=10)
        assert r.status_code == 400, r.text
        assert "Duplicate query parameter: x" in r.json()["error"]


# ---------------------------------------------------------------------------
# Writes as MCP tools


class TestDeleteTool:
    def test_delete_endpoint_is_a_write_tool(self, server):
        r = requests.post(f"{server.base}/items", json={"id": 50, "price": 2.5}, timeout=10)
        assert r.status_code == 201, r.text
        result = mcp(server, "tools/call", {"name": "delete_item", "arguments": {"id": 50}})
        assert "error" not in result, result
        assert not result["result"].get("isError"), result
        ids = [row["id"] for row in requests.get(f"{server.base}/items", timeout=10).json()["data"]]
        assert 50 not in ids

    def test_returning_no_row_still_has_data(self, server):
        r = requests.delete(f"{server.base}/items/987654", timeout=10)
        assert r.status_code == 200, r.text
        assert r.json() == {"rows_affected": 0, "data": []}


# ---------------------------------------------------------------------------
# Serialisation


class TestSerialisation:
    def test_columns_in_select_order_and_short_doubles(self, server):
        r = requests.get(f"{server.base}/rows", params={"limit": 1}, timeout=10)
        assert r.status_code == 200, r.text
        assert r.text.startswith('{"data":[{"zeta":"z","price":711.56,"alpha":1,'), r.text
        assert "711.5599" not in r.text

    def test_timestamp_has_no_zone_timestamptz_keeps_z(self, server):
        row = requests.get(f"{server.base}/rows", params={"limit": 1}, timeout=10).json()["data"][0]
        assert row["ts"] == "2024-05-06T07:08:09.123456"
        assert row["ts_tz"] == "2024-05-06T07:08:09.123456Z"

    def test_tools_call_uses_the_same_serialisation(self, server):
        result = mcp(server, "tools/call", {"name": "rows", "arguments": {}})
        text = result["result"]["content"][0]["text"]
        assert '"zeta":"z","price":711.56,"alpha":1' in text, text


class TestServerInfo:
    def test_initialize_reports_the_build_version(self, server):
        info = mcp(server, "initialize")["result"]["serverInfo"]
        out = subprocess.run([flapi_binary(), "--version"], capture_output=True, text=True, timeout=30)
        assert info["version"] != "0.3.0"
        assert info["version"] in out.stdout, (info, out.stdout)


class TestOpenApi:
    def doc(self, server):
        r = requests.get(f"{server.base}/doc.yaml", timeout=30)
        assert r.status_code == 200, r.text
        assert "properties: ~" not in r.text
        return yaml.safe_load(r.text)

    def test_parameters_are_typed_with_bounds(self, server):
        op = self.doc(server)["paths"]["/rows"]["get"]
        params = {p["name"]: p for p in op["parameters"]}
        assert params["x"]["schema"] == {"type": "integer", "minimum": 1, "maximum": 1000}
        assert params["ratio"]["schema"]["type"] == "number"
        assert params["ratio"]["schema"]["minimum"] == 0.5
        assert params["limit"]["schema"]["type"] == "integer"
        assert params["limit"]["schema"]["default"] == 100
        assert params["offset"]["in"] == "query"
        assert params["format"]["schema"]["enum"] == ["json", "csv", "arrow"]

    def test_description_responses_and_headers(self, server):
        op = self.doc(server)["paths"]["/rows"]["get"]
        assert op["description"] == "Rows in a fixed column order"
        # yaml-cpp emits status codes as plain scalars, which YAML reads as ints.
        responses = {str(k): v for k, v in op["responses"].items()}
        assert "400" in responses
        assert set(responses["200"]["headers"]) == {"X-Total-Count", "X-Offset", "X-Limit", "X-Next"}

    def test_body_fields_are_in_the_request_body_typed(self, server):
        op = self.doc(server)["paths"]["/items"]["post"]
        assert "parameters" not in op or all(p["in"] != "body" for p in op["parameters"])
        props = op["requestBody"]["content"]["application/json"]["schema"]["properties"]
        assert props["id"]["type"] == "integer"
        assert props["price"]["type"] == "number"
        assert props["price"]["minimum"] == 0
        assert "description" not in self.doc(server)["paths"]["/items"]["get"]


class TestCachedReadsSkipTheSourceLock:
    def test_cached_read_does_not_wait_for_a_locked_source(self, server):
        r = requests.post(f"{server.base}/api/v1/_config/endpoints/-locked_cached/cache/refresh",
                          headers=CS, timeout=60)
        assert r.status_code == 200, r.text
        assert requests.get(f"{server.base}/locked_cached", timeout=10).json()["data"] == [{"n": 100}]

        # An uncached read on the same serialize-access connection that runs
        # for a while (grown until it takes at least 1.5 s on this machine).
        n = 50_000_000
        while True:
            t0 = time.time()
            assert requests.get(f"{server.base}/heavy", params={"n": n}, timeout=120).status_code == 200
            if time.time() - t0 >= 1.5 or n >= 1_600_000_000:
                break
            n *= 2

        done = threading.Event()

        def heavy():
            requests.get(f"{server.base}/heavy", params={"n": n}, timeout=120)
            done.set()

        t = threading.Thread(target=heavy)
        t.start()
        time.sleep(0.3)   # the heavy read now holds the connection's lock
        t0 = time.time()
        r = requests.get(f"{server.base}/locked_cached", timeout=60)
        cached_took = time.time() - t0
        still_running = not done.is_set()
        t.join()
        assert r.status_code == 200, r.text
        # It answered while the source lock was held, instead of queueing behind it.
        assert still_running, f"the cached read waited for the source lock ({cached_took:.2f}s)"
