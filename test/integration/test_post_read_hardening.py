"""Reads over POST (`operation.type: read`) and the edges of the body path.

* `?limit=` / `?offset=` must never reach a WRITE template, and wherever they
  are accepted they must be plain non-negative integers: a write template
  `SELECT '{{{ params.limit }}}'` was injectable with `?limit=1'||version()||'`.
* A body that is not a JSON object is a 400, on reads and writes alike; it was
  read as empty (a read then returned every row).
* A POST read rejects unknown body keys like a write does (`validate-before-write`,
  default on) and like MCP does; a typo widened the result.
* A duplicated query parameter is a 400 on a POST read, as on a GET.
* A bad limit/offset is a JSON 400 in the validators' `{"errors": [...]}` shape.
* Arrow on a POST read pages with the body's limit/offset.
* /doc.yaml documents a POST read's body.
* MCP `tools/call` with a bad limit is an isError with a clear message.
* An unknown `operation.type` stops `--validate-config`.
"""
import json
import os
import signal
import socket
import subprocess
import tempfile
import time
from types import SimpleNamespace

import pytest
import requests
import yaml

from otel_helpers import flapi_binary

pytestmark = pytest.mark.standalone_server

CONFIG = """\
project-name: post-read-hardening
template:
  path: ./sqls
connections:
  app:
    init: |
      ATTACH IF NOT EXISTS './app.duckdb' AS app;
      CREATE OR REPLACE TABLE app.items (id INTEGER, name VARCHAR);
      INSERT INTO app.items VALUES (1, 'apple'), (2, 'apricot'), (3, 'banana');
    properties:
      unused: x
rate_limit:
  enabled: false
mcp:
  enabled: true
"""

NAME_FIELD = """\
request:
  - field-name: name
    field-in: body
    required: true
    validators:
      - type: string
        pattern: "^[a-z]+$"
"""

ENDPOINTS = {
    "find.yaml": "url-path: /find\nmethod: POST\ntemplate-source: find.sql\nconnection: [app]\n"
                 "operation:\n  type: read\n"
                 "mcp-tool:\n  name: find_items\n  description: Find items by prefix\n" + NAME_FIELD,
    "find.sql": "SELECT id, name FROM app.items WHERE name LIKE {{ params.name }} || '%' ORDER BY id",
    # A write that echoes params.limit: nothing but the server default may reach it.
    "echo.yaml": "url-path: /echo\nmethod: POST\ntemplate-source: echo.sql\nconnection: [app]\n"
                 "operation:\n  returns-data: true\n  validate-before-write: false\n",
    "echo.sql": "SELECT '{{{ params.limit }}}' AS l",
    "list.yaml": "url-path: /list\nmethod: GET\ntemplate-source: list.sql\nconnection: [app]\n",
    "list.sql": "SELECT id FROM app.items ORDER BY id",
    "create.yaml": "url-path: /create\nmethod: POST\ntemplate-source: create.sql\nconnection: [app]\n"
                   "request:\n  - field-name: id\n    field-in: body\n    required: true\n"
                   "    validators:\n      - type: int\n",
    "create.sql": "INSERT INTO app.items VALUES ({{ params.id }}, 'new')",
}


def _port_in_range() -> int:
    for port in range(31000, 31100):
        s = socket.socket()
        try:
            s.bind(("127.0.0.1", port))
            return port
        except OSError:
            continue
        finally:
            s.close()
    pytest.skip("no free port in 31000-31099")


def _project(endpoints, config=CONFIG):
    root = tempfile.mkdtemp(prefix="flapi_post_read_")
    os.makedirs(os.path.join(root, "sqls"))
    with open(os.path.join(root, "flapi.yaml"), "w") as f:
        f.write(config)
    for name, body in endpoints.items():
        with open(os.path.join(root, "sqls", name), "w") as f:
            f.write(body)
    return root


@pytest.fixture(scope="module")
def server():
    root = _project(ENDPOINTS)
    port = _port_in_range()
    log_path = os.path.join(root, "server.log")
    log = open(log_path, "w")
    proc = subprocess.Popen(
        [flapi_binary(), "-c", "flapi.yaml", "-p", str(port), "--log-level", "info"],
        cwd=root, stdout=log, stderr=subprocess.STDOUT, preexec_fn=os.setsid,
        env={**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"},
    )
    base = f"http://127.0.0.1:{port}"
    deadline = time.time() + 60
    while time.time() < deadline:
        if proc.poll() is not None:
            pytest.fail("server exited:\n" + open(log_path).read())
        try:
            if requests.get(f"{base}/health/live", timeout=1).status_code == 200:
                break
        except requests.RequestException:
            pass
        time.sleep(0.3)
    else:
        pytest.fail("server never became live")
    try:
        yield SimpleNamespace(base=base, root=root, log_path=log_path)
    finally:
        os.killpg(proc.pid, signal.SIGTERM)
        try:
            proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            os.killpg(proc.pid, signal.SIGKILL)
        log.close()


PAYLOAD = "1'||version()||'"


class TestPaginationNeverReachesWrites:
    def test_query_limit_does_not_reach_a_write_template(self, server):
        r = requests.post(f"{server.base}/echo", params={"limit": PAYLOAD}, json={}, timeout=10)
        assert r.status_code == 201, r.text
        assert r.json()["data"] == [{"l": "100"}], r.text

    def test_body_limit_on_a_write_must_be_a_plain_int(self, server):
        r = requests.post(f"{server.base}/echo", json={"limit": PAYLOAD}, timeout=10)
        assert r.status_code == 400, r.text
        assert r.json()["errors"][0]["field"] == "limit", r.text

    def test_query_limit_on_a_read_must_be_a_plain_int(self, server):
        r = requests.post(f"{server.base}/find", params={"limit": PAYLOAD}, json={"name": "ap"}, timeout=10)
        assert r.status_code == 400, r.text


class TestBodies:
    @pytest.mark.parametrize("body", ["{not json", "[1, 2]", "\"text\"", "42"])
    def test_a_read_refuses_a_body_that_is_not_an_object(self, server, body):
        r = requests.post(f"{server.base}/find", data=body,
                          headers={"Content-Type": "application/json"}, timeout=10)
        assert r.status_code == 400, r.text
        assert "request body is not a JSON object" in r.text

    def test_a_write_refuses_a_body_that_is_not_an_object(self, server):
        r = requests.post(f"{server.base}/create", data="{\"id\": 9",
                          headers={"Content-Type": "application/json"}, timeout=10)
        assert r.status_code == 400, r.text
        assert "request body is not a JSON object" in r.text

    def test_unknown_body_key_on_a_read_is_refused(self, server):
        r = requests.post(f"{server.base}/find", json={"name": "ap", "nmae": "x"}, timeout=10)
        assert r.status_code == 400, r.text
        assert "nmae" in r.text

    def test_duplicate_query_parameter_on_a_read_is_refused(self, server):
        r = requests.post(f"{server.base}/find?limit=1&limit=2", json={"name": "ap"}, timeout=10)
        assert r.status_code == 400, r.text


class TestPaginationErrors:
    @pytest.mark.parametrize("where", ["query", "body"])
    def test_bad_limit_is_a_json_400(self, server, where):
        if where == "query":
            r = requests.post(f"{server.base}/find?limit=-1", json={"name": "ap"}, timeout=10)
        else:
            r = requests.post(f"{server.base}/find", json={"name": "ap", "limit": "abc"}, timeout=10)
        assert r.status_code == 400, r.text
        assert r.headers["Content-Type"].startswith("application/json"), r.headers
        errors = r.json()["errors"]
        assert errors[0]["field"] == "limit", r.text

    def test_a_string_limit_in_the_body_is_accepted(self, server):
        r = requests.post(f"{server.base}/find", json={"name": "ap", "limit": "1"}, timeout=10)
        assert r.status_code == 200, r.text
        assert len(r.json()["data"]) == 1

    def test_get_bad_limit_is_json_too(self, server):
        r = requests.get(f"{server.base}/list?offset=1x", timeout=10)
        assert r.status_code == 400, r.text
        assert r.headers["Content-Type"].startswith("application/json"), r.headers
        assert r.json()["errors"][0]["field"] == "offset", r.text


class TestArrow:
    def test_arrow_pages_with_the_body_window(self, server):
        r = requests.post(f"{server.base}/find?format=arrow", json={"name": "ap", "limit": 1, "offset": 1},
                          timeout=10)
        assert r.status_code == 200, r.text
        assert r.headers["X-Arrow-Rows"] == "1", r.headers


class TestOpenApi:
    def test_a_post_read_documents_its_body(self, server):
        doc = yaml.safe_load(requests.get(f"{server.base}/doc.yaml", timeout=10).text)
        op = doc["paths"]["/find"]["post"]
        schema = op["requestBody"]["content"]["application/json"]["schema"]
        assert "name" in schema["properties"], op
        assert schema["required"] == ["name"]


def mcp_call(s, tool, arguments):
    r = requests.post(f"{s.base}/mcp/jsonrpc", json={
        "jsonrpc": "2.0", "id": 1, "method": "initialize",
        "params": {"protocolVersion": "2024-11-05", "capabilities": {},
                   "clientInfo": {"name": "t", "version": "0"}}}, timeout=10)
    headers = {"Mcp-Session-Id": r.headers.get("Mcp-Session-Id", "")}
    r = requests.post(f"{s.base}/mcp/jsonrpc", headers=headers, json={
        "jsonrpc": "2.0", "id": 2, "method": "tools/call",
        "params": {"name": tool, "arguments": arguments}}, timeout=30)
    assert r.status_code == 200, r.text
    return r.json()


class TestMcpPagination:
    @pytest.mark.parametrize("limit", ["abc", -1, "1;SELECT 1"])
    def test_bad_limit_is_a_clear_tool_error(self, server, limit):
        body = mcp_call(server, "find_items", {"name": "ap", "limit": limit})
        text = json.dumps(body)
        if "error" in body:
            assert "limit" in body["error"]["message"], text
        else:
            assert body["result"].get("isError") is True, text
            assert "limit must be a non-negative integer" in body["result"]["content"][0]["text"], text
        assert "stoll" not in text

    def test_good_limit_pages(self, server):
        body = mcp_call(server, "find_items", {"name": "ap", "limit": 1})
        assert not body["result"].get("isError"), body
        assert len(json.loads(body["result"]["content"][0]["text"])) == 1


class TestOperationType:
    @pytest.mark.parametrize("value", ["bogus", "raed"])
    def test_unknown_type_stops_validate_config(self, value):
        root = _project({
            "x.yaml": f"url-path: /x\nmethod: POST\ntemplate-source: x.sql\nconnection: [app]\n"
                      f"operation:\n  type: {value}\n",
            "x.sql": "SELECT 1",
        })
        r = subprocess.run([flapi_binary(), "-c", "flapi.yaml", "--validate-config", "--log-level", "error"],
                           cwd=root, capture_output=True, text=True, timeout=120,
                           env={**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"})
        out = r.stdout + r.stderr
        assert r.returncode != 0, out
        assert value in out and "read" in out and "write" in out, out

    @pytest.mark.parametrize("value", ["Read", "WRITE"])
    def test_known_type_any_case_is_valid(self, value):
        root = _project({
            "x.yaml": f"url-path: /x\nmethod: POST\ntemplate-source: x.sql\nconnection: [app]\n"
                      f"operation:\n  type: {value}\n",
            "x.sql": "SELECT 1",
        })
        r = subprocess.run([flapi_binary(), "-c", "flapi.yaml", "--validate-config", "--log-level", "error"],
                           cwd=root, capture_output=True, text=True, timeout=120,
                           env={**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"})
        assert r.returncode == 0, r.stdout + r.stderr
