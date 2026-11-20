"""MCP JSON-RPC responses carry X-Request-Id, like REST ones (#215).

RequestContextMiddleware set the header in before_handle, but the MCP route
handlers return a fresh crow::response that replaces it, so the header was lost
and an operator could not join an MCP call to its audit/log lines.
"""
import json
import os
import signal
import subprocess
import tempfile
import time

import pytest
import requests

from otel_helpers import flapi_binary, free_port

pytestmark = pytest.mark.standalone_server

CONFIG = """\
project-name: mcp-request-id
template:
  path: ./sqls
connections:
  inmem:
    properties:
      database: ':memory:'
mcp:
  enabled: true
"""


@pytest.fixture(scope="module")
def server():
    root = tempfile.mkdtemp(prefix="flapi_mcpreqid_")
    os.makedirs(os.path.join(root, "sqls"))
    with open(os.path.join(root, "flapi.yaml"), "w") as f:
        f.write(CONFIG)
    with open(os.path.join(root, "sqls", "t.yaml"), "w") as f:
        f.write("mcp-tool:\n  name: get_t\n  description: t\n"
                "template-source: t.sql\nconnection: [inmem]\n")
    with open(os.path.join(root, "sqls", "t.sql"), "w") as f:
        f.write("SELECT 1 AS n\n")
    port = free_port()
    log = open(os.path.join(root, "server.log"), "w")
    proc = subprocess.Popen(
        [flapi_binary(), "-c", "flapi.yaml", "-p", str(port), "--log-level", "info"],
        cwd=root, stdout=log, stderr=subprocess.STDOUT, preexec_fn=os.setsid,
        env={**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"})
    base = f"http://127.0.0.1:{port}"
    deadline = time.time() + 60
    while time.time() < deadline:
        try:
            if requests.get(f"{base}/health/live", timeout=1).status_code == 200:
                break
        except requests.RequestException:
            time.sleep(0.2)
    else:
        pytest.fail("server never became live")
    yield base
    os.killpg(proc.pid, signal.SIGTERM)
    proc.wait(timeout=20)


def rpc(base, method, params=None, req_id=1):
    return requests.post(
        f"{base}/mcp/jsonrpc", timeout=30,
        headers={"Content-Type": "application/json"},
        data=json.dumps({"jsonrpc": "2.0", "id": req_id, "method": method, "params": params or {}}))


class TestMcpRequestId:
    @pytest.mark.parametrize("method,params", [
        ("initialize", {"protocolVersion": "2025-03-26", "capabilities": {},
                        "clientInfo": {"name": "t", "version": "1"}}),
        ("tools/list", {}),
        ("tools/call", {"name": "get_t", "arguments": {}}),
        ("tools/call", {"name": "no_such_tool", "arguments": {}}),
    ])
    def test_response_has_request_id(self, server, method, params):
        r = rpc(server, method, params)
        assert r.headers.get("X-Request-Id"), (
            f"{method}: status {r.status_code}, no X-Request-Id; headers={dict(r.headers)}")

    def test_malformed_body_error_response_has_request_id(self, server):
        r = requests.post(f"{server}/mcp/jsonrpc", data="{not json", timeout=10,
                          headers={"Content-Type": "application/json"})
        assert r.headers.get("X-Request-Id"), dict(r.headers)

    def test_ids_are_unique_per_request(self, server):
        a = rpc(server, "tools/list").headers.get("X-Request-Id")
        b = rpc(server, "tools/list").headers.get("X-Request-Id")
        assert a and b and a != b
