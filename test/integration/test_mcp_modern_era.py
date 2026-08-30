"""C7: MCP 2026-07-28 dual-era server.

A modern request carries params._meta["io.modelcontextprotocol/protocolVersion"]
and gets the stateless path: server/discover, resultType on every result,
ttlMs/cacheScope on cacheable results, no Mcp-Session-Id, ping/logging/setLevel
removed, GET/unsupported-version rejected. Legacy requests (no _meta) keep the
initialize + session behaviour unchanged.
"""

import os
import socket
import subprocess
import tempfile
import time
from typing import Iterator

import pytest
import requests

PV = "io.modelcontextprotocol/protocolVersion"
CC = "io.modelcontextprotocol/clientCapabilities"
MODERN_META = {PV: "2026-07-28", CC: {}}


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
    tmp = tempfile.mkdtemp(prefix="flapi_modern_")
    sqls = os.path.join(tmp, "sqls")
    os.makedirs(sqls)
    with open(os.path.join(tmp, "flapi.yaml"), "w") as f:
        f.write(f"""
project-name: mcp-modern-test
project-description: modern era E2E
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
    with open(os.path.join(sqls, "t.sql"), "w") as f:
        f.write("SELECT 1 AS x\n")
    with open(os.path.join(sqls, "t.yaml"), "w") as f:
        f.write("template-source: t.sql\nconnection: [inmem]\nmcp-tool: {name: tool_a, description: a}\n")

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


def _post(base, method, params, id_="x"):
    return requests.post(f"{base}/mcp/jsonrpc",
                         headers={"Content-Type": "application/json"},
                         json={"jsonrpc": "2.0", "id": id_, "method": method, "params": params},
                         timeout=10)


@pytest.mark.standalone_server
class TestModernEra:
    def test_server_discover(self, server):
        r = _post(server, "server/discover", {"_meta": MODERN_META})
        assert r.status_code == 200, r.text
        result = r.json()["result"]
        assert "2026-07-28" in result["supportedVersions"]
        assert result["resultType"] == "complete"
        assert result["ttlMs"] == 3600000
        assert result["cacheScope"] == "public"
        assert "io.modelcontextprotocol/tasks" in result["capabilities"]["extensions"]

    def test_modern_result_envelope_and_no_session(self, server):
        r = _post(server, "tools/list", {"_meta": MODERN_META})
        assert r.status_code == 200
        assert "Mcp-Session-Id" not in r.headers
        result = r.json()["result"]
        assert result["resultType"] == "complete"
        assert result["cacheScope"] == "private"
        assert result["ttlMs"] == 300000

    def test_unsupported_version_rejected(self, server):
        r = _post(server, "tools/list", {"_meta": {PV: "1999-01-01", CC: {}}})
        assert r.status_code == 400
        err = r.json()["error"]
        assert err["code"] == -32022
        assert "supported" in err["data"]

    def test_missing_client_capabilities_rejected(self, server):
        r = _post(server, "tools/list", {"_meta": {PV: "2026-07-28"}})
        assert r.status_code == 400
        assert r.json()["error"]["code"] == -32602

    def test_ping_removed_on_modern_path(self, server):
        r = _post(server, "ping", {"_meta": MODERN_META})
        assert r.json()["error"]["code"] == -32601

    def test_logging_setlevel_removed_on_modern_path(self, server):
        r = _post(server, "logging/setLevel", {"_meta": MODERN_META, "level": "debug"})
        assert r.json()["error"]["code"] == -32601

    def test_get_returns_405(self, server):
        r = requests.get(f"{server}/mcp/jsonrpc", timeout=10)
        assert r.status_code == 405

    def test_modern_ignores_inbound_session_header(self, server):
        r = requests.post(f"{server}/mcp/jsonrpc",
                          headers={"Content-Type": "application/json", "Mcp-Session-Id": "abc"},
                          json={"jsonrpc": "2.0", "id": 1, "method": "tools/list",
                                "params": {"_meta": MODERN_META}}, timeout=10)
        assert "Mcp-Session-Id" not in r.headers

    # ---- legacy coexistence ----

    def test_legacy_initialize_unchanged(self, server):
        r = _post(server, "initialize", {"protocolVersion": "2025-11-25", "capabilities": {},
                                         "clientInfo": {"name": "c", "version": "1"}})
        assert r.status_code == 200
        result = r.json()["result"]
        assert result["protocolVersion"] == "2025-11-25"
        # Legacy envelope: no modern resultType, and a session is minted.
        assert "resultType" not in result
        assert r.headers.get("Mcp-Session-Id")

    def test_legacy_ping_still_works(self, server):
        r = _post(server, "ping", {})
        assert "error" not in r.json()
