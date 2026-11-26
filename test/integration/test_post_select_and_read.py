"""POST endpoints that answer with rows, against a real server.

#271: a POST whose template is one SELECT answered `data: []` even with
`returns-data: true` - only a SELECT that was the second statement or later,
and that began with the word SELECT, became `data`. And a POST with an explicit
`operation.type: read` still ran the write path over REST, though MCP and the
OpenAPI document treated it as the read it says it is.

#277 (RETURNING part): RETURNING was found by substring, so a column
`returning_note` or a 'returning' literal made a plain UPDATE report one row
affected and return DuckDB's `Count` row as data.

Both REST and MCP tools/call are covered: they share the write executor.
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

from otel_helpers import flapi_binary

pytestmark = pytest.mark.standalone_server

CONFIG = """\
project-name: post-select-and-read
template:
  path: ./sqls
connections:
  app:
    init: |
      ATTACH IF NOT EXISTS './app.duckdb' AS app;
      CREATE OR REPLACE TABLE app.items (id INTEGER PRIMARY KEY, name VARCHAR, returning_note VARCHAR);
      INSERT INTO app.items VALUES (1, 'apple', NULL), (2, 'apricot', NULL), (3, 'banana', NULL);
    properties:
      unused: x
ducklake:
  enabled: true
  alias: cache
  metadata-path: ./cache.ducklake
  data-path: ./cache_data
rate_limit:
  enabled: false
mcp:
  enabled: true
"""

Q_FIELD = """\
request:
  - field-name: q
    field-in: body
    required: true
    validators:
      - type: string
        pattern: "^[a-z]+$"
        max-length: 20
"""

ENDPOINTS = {
    # A search that takes its input as a JSON body: one SELECT.
    "search.yaml": "url-path: /search\nmethod: POST\ntemplate-source: search.sql\nconnection: [app]\n"
                   "operation:\n  returns-data: true\n"
                   "mcp-tool:\n  name: search_items\n  description: Search items by name\n" + Q_FIELD,
    "search.sql": "SELECT id, name FROM app.items WHERE name LIKE {{ params.q }} || '%' ORDER BY id",
    # The same, as a CTE behind a comment.
    "search_cte.yaml": "url-path: /search_cte\nmethod: POST\ntemplate-source: search_cte.sql\nconnection: [app]\n"
                       "operation:\n  returns-data: true\n" + Q_FIELD,
    "search_cte.sql": "-- matching items\nWITH m AS (SELECT * FROM app.items WHERE name LIKE {{ params.q }} || '%')\n"
                      "SELECT id, name FROM m ORDER BY id",
    # A write whose text says "returning" without a RETURNING clause, then a SELECT.
    "touch.yaml": "url-path: /touch\nmethod: POST\ntemplate-source: touch.sql\nconnection: [app]\n"
                  "operation:\n  returns-data: true\n"
                  "mcp-tool:\n  name: touch_items\n  description: Mark every item\n",
    "touch.sql": "UPDATE app.items SET returning_note = 'returning';\n"
                 "SELECT count(*) AS marked FROM app.items WHERE returning_note = 'returning'",
    # An explicit read over POST: validators, pagination envelope, body params.
    "find.yaml": "url-path: /find\nmethod: POST\ntemplate-source: find.sql\nconnection: [app]\n"
                 "operation:\n  type: read\n"
                 "mcp-tool:\n  name: find_items\n  description: Find items by name prefix\n" + Q_FIELD,
    "find.sql": "SELECT id, name FROM app.items WHERE name LIKE {{ params.q }} || '%' ORDER BY id",
    # An explicit read over POST served from a cache.
    "find_cached.yaml": "url-path: /find_cached\nmethod: POST\ntemplate-source: find_cached.sql\n"
                        "connection: [app]\noperation:\n  type: read\n"
                        "cache:\n  enabled: true\n  table: items_cache\n  schema: main\n"
                        "  template-file: items_cache.sql\n" + Q_FIELD,
    "find_cached.sql": "SELECT id, name, source FROM {{{cache.catalog}}}.{{{cache.schema}}}.{{{cache.table}}}\n"
                       "WHERE name LIKE {{ params.q }} || '%' ORDER BY id",
    "items_cache.sql": "CREATE OR REPLACE TABLE {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS\n"
                       "SELECT id, name, 'cache' AS source FROM app.items",
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


@pytest.fixture(scope="module")
def server():
    root = tempfile.mkdtemp(prefix="flapi_post_select_")
    os.makedirs(os.path.join(root, "sqls"))
    with open(os.path.join(root, "flapi.yaml"), "w") as f:
        f.write(CONFIG)
    for name, body in ENDPOINTS.items():
        with open(os.path.join(root, "sqls", name), "w") as f:
            f.write(body)

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

    s = SimpleNamespace(base=base, root=root, log_path=log_path)
    try:
        yield s
    finally:
        os.killpg(proc.pid, signal.SIGTERM)
        try:
            proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            os.killpg(proc.pid, signal.SIGKILL)
        log.close()


def log_tail(s, n=3000):
    with open(s.log_path) as f:
        return f.read()[-n:]


def mcp_call(s, tool, arguments):
    r = requests.post(f"{s.base}/mcp/jsonrpc", json={
        "jsonrpc": "2.0", "id": 1, "method": "initialize",
        "params": {"protocolVersion": "2024-11-05", "capabilities": {},
                   "clientInfo": {"name": "t", "version": "0"}}}, timeout=10)
    assert r.status_code == 200, r.text
    headers = {"Mcp-Session-Id": r.headers.get("Mcp-Session-Id", "")}
    r = requests.post(f"{s.base}/mcp/jsonrpc", headers=headers, json={
        "jsonrpc": "2.0", "id": 2, "method": "tools/call",
        "params": {"name": tool, "arguments": arguments}}, timeout=30)
    assert r.status_code == 200, r.text
    body = r.json()
    assert "error" not in body, r.text
    result = body["result"]
    assert not result.get("isError"), r.text
    return json.loads(result["content"][0]["text"])


def ids(rows):
    return [row["id"] for row in rows]


class TestSingleSelectPost:
    def test_single_select_returns_its_rows(self, server):
        r = requests.post(f"{server.base}/search", json={"q": "ap"}, timeout=10)
        assert r.status_code == 201, r.text
        body = r.json()
        assert ids(body["data"]) == [1, 2], r.text
        assert body["rows_affected"] == 0

    def test_cte_after_a_comment_returns_its_rows(self, server):
        r = requests.post(f"{server.base}/search_cte", json={"q": "ban"}, timeout=10)
        assert r.status_code == 201, r.text
        assert r.json()["data"] == [{"id": 3, "name": "banana"}], r.text

    def test_validators_still_apply(self, server):
        r = requests.post(f"{server.base}/search", json={"q": "x' OR 1=1 --"}, timeout=10)
        assert r.status_code == 400, r.text

    def test_mcp_tool_returns_the_rows(self, server):
        payload = mcp_call(server, "search_items", {"q": "ap"})
        assert ids(payload["data"]) == [1, 2], payload


class TestReturningIsTheClauseNotTheWord:
    def test_update_mentioning_returning_counts_and_selects(self, server):
        r = requests.post(f"{server.base}/touch", json={}, timeout=10)
        assert r.status_code == 201, r.text
        body = r.json()
        assert body["rows_affected"] == 3, r.text
        assert body["data"] == [{"marked": 3}], r.text

    def test_mcp_tool_counts_and_selects(self, server):
        payload = mcp_call(server, "touch_items", {})
        assert payload["rows_affected"] == 3, payload
        assert payload["data"] == [{"marked": 3}], payload


class TestPostRead:
    def test_runs_the_read_path_with_body_params(self, server):
        r = requests.post(f"{server.base}/find", json={"q": "ap"}, timeout=10)
        assert r.status_code == 200, r.text
        body = r.json()
        assert ids(body["data"]) == [1, 2], r.text
        assert body["total_count"] == 2
        assert "rows_affected" not in body
        assert r.headers.get("X-Total-Count") == "2"

    def test_pages_with_limit_and_offset_in_the_body(self, server):
        r = requests.post(f"{server.base}/find", json={"q": "ap", "limit": 1, "offset": 1}, timeout=10)
        assert r.status_code == 200, r.text
        body = r.json()
        assert ids(body["data"]) == [2], r.text
        assert body["total_count"] == 2

    def test_validates_body_params(self, server):
        r = requests.post(f"{server.base}/find", json={"q": "AP!"}, timeout=10)
        assert r.status_code == 400, r.text
        r = requests.post(f"{server.base}/find", json={}, timeout=10)
        assert r.status_code == 400, r.text

    def test_body_field_is_not_taken_from_the_query_string(self, server):
        # `q` is `field-in: body`: a query value does not satisfy it (#192).
        r = requests.post(f"{server.base}/find?q=ap", json={}, timeout=10)
        assert r.status_code == 400, r.text

    def test_mcp_tool_agrees_with_rest(self, server):
        # A read tool answers with the rows themselves (MCP already ran it as a read).
        payload = mcp_call(server, "find_items", {"q": "ap"})
        assert ids(payload) == [1, 2], payload

    def test_serves_from_the_cache(self, server):
        deadline = time.time() + 60
        while True:
            r = requests.post(f"{server.base}/find_cached", json={"q": "ban"}, timeout=10)
            if r.status_code != 503 or time.time() > deadline:
                break
            time.sleep(0.3)
        assert r.status_code == 200, r.text + log_tail(server)
        assert r.json()["data"] == [{"id": 3, "name": "banana", "source": "cache"}], r.text
