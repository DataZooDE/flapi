"""outputSchema is learned from a tool's first successful result and then
advertised in tools/list (flAPI cannot know a parameterised query's columns
statically, so the schema is derived from real data)."""

import os
import socket
import subprocess
import tempfile
import time
from typing import Iterator

import pytest
import requests


def _repo_root() -> str:
    return os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def _flapi_binary() -> str:
    for bt in ("release", "debug"):
        p = os.path.join(_repo_root(), "build", bt, "flapi")
        if os.path.exists(p):
            return p
    pytest.skip("flapi binary not found")


def _free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


@pytest.fixture
def server() -> Iterator[str]:
    binary = _flapi_binary()
    port = _free_port()
    tmp = tempfile.mkdtemp(prefix="flapi_osch_")
    sqls = os.path.join(tmp, "sqls")
    os.makedirs(sqls)
    with open(os.path.join(tmp, "flapi.yaml"), "w") as f:
        f.write(f"""
project-name: mcp-outputschema-test
project-description: outputSchema E2E
http-port: {port}
template:
  path: ./sqls
connections:
  inmem:
    properties:
      database: ':memory:'
duckdb:
  access_mode: READ_WRITE
  threads: 1
mcp:
  enabled: true
""")
    with open(os.path.join(sqls, "c.sql"), "w") as f:
        f.write("SELECT 42 AS id, 'Alice' AS name, true AS active, 3.14 AS score\n")
    with open(os.path.join(sqls, "c.yaml"), "w") as f:
        f.write("template-source: c.sql\nconnection: [inmem]\nmcp-tool: {name: cust, description: c}\n")

    log = open(os.path.join(tmp, "server.log"), "w")
    proc = subprocess.Popen([binary, "-c", os.path.join(tmp, "flapi.yaml"), "--no-telemetry"],
                            cwd=tmp, stdout=log, stderr=subprocess.STDOUT)
    base = f"http://127.0.0.1:{port}"
    try:
        deadline = time.time() + 30
        while time.time() < deadline:
            if proc.poll() is not None:
                raise RuntimeError("flapi exited early")
            try:
                if requests.get(f"{base}/mcp/health", timeout=1).status_code < 500:
                    break
            except requests.exceptions.RequestException:
                time.sleep(0.5)
        yield base
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
        log.close()


def _rpc(base, method, params):
    return requests.post(f"{base}/mcp/jsonrpc", headers={"Content-Type": "application/json"},
                         json={"jsonrpc": "2.0", "id": "x", "method": method, "params": params},
                         timeout=10).json()


@pytest.mark.standalone_server
class TestOutputSchema:
    def test_output_schema_learned_after_first_call(self, server):
        # Absent before any call.
        t = _rpc(server, "tools/list", {})["result"]["tools"][0]
        assert "outputSchema" not in t

        # Call the tool, then it appears with correct per-column types.
        _rpc(server, "tools/call", {"name": "cust", "arguments": {}})
        t = _rpc(server, "tools/list", {})["result"]["tools"][0]
        assert "outputSchema" in t
        cols = t["outputSchema"]["properties"]["rows"]["items"]["properties"]
        # JSON numbers are typed as "number" (not guessed "integer" from one row's
        # runtime value), so an integer-looking column and a fractional one agree.
        assert cols["id"]["type"] == "number"
        assert cols["name"]["type"] == "string"
        assert cols["active"]["type"] == "boolean"
        assert cols["score"]["type"] == "number"
