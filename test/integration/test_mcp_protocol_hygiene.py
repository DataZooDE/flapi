"""JSON-RPC protocol-hygiene regressions for the MCP endpoint (C2):

- a notification (request object with no `id`) gets HTTP 202 and no body,
  instead of a spurious `-32601` with `id: null`;
- a large integer id is echoed back losslessly (previously mangled by a
  std::stod round-trip);
- string and null ids are echoed with the correct JSON type;
- `initialize` honestly advertises `listChanged: false`.
"""

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
    for build_type in ("release", "debug"):
        path = os.path.join(_repo_root(), "build", build_type, "flapi")
        if os.path.exists(path):
            return path
    pytest.skip("flapi binary not found in build/release or build/debug")


def _free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def _write_config(dirpath: str, port: int) -> str:
    sqls = os.path.join(dirpath, "sqls")
    os.makedirs(sqls)
    with open(os.path.join(dirpath, "flapi.yaml"), "w") as f:
        f.write(f"""
project-name: mcp-hygiene-test
project-description: JSON-RPC hygiene E2E
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
    with open(os.path.join(sqls, "ping_tool.yaml"), "w") as f:
        f.write("""
template-source: ping_tool.sql
connection: [inmem]
mcp-tool:
  name: ping_tool
  description: trivial tool
""")
    with open(os.path.join(sqls, "ping_tool.sql"), "w") as f:
        f.write("SELECT 1 AS ok\n")
    return os.path.join(dirpath, "flapi.yaml")


@pytest.fixture
def hygiene_server() -> Iterator[str]:
    binary = _flapi_binary()
    port = _free_port()
    with tempfile.TemporaryDirectory(prefix="flapi_hygiene_") as tmpdir:
        config_path = _write_config(tmpdir, port)
        log_path = os.path.join(tmpdir, "server.log")
        log_file = open(log_path, "w")
        proc = subprocess.Popen(
            [binary, "-c", config_path, "--no-telemetry"],
            cwd=tmpdir, stdout=log_file, stderr=subprocess.STDOUT,
        )
        try:
            base_url = f"http://127.0.0.1:{port}"
            deadline = time.time() + 30
            up = False
            while time.time() < deadline:
                if proc.poll() is not None:
                    break
                try:
                    if requests.get(f"{base_url}/mcp/health", timeout=1).status_code < 500:
                        up = True
                        break
                except requests.exceptions.RequestException:
                    time.sleep(0.5)
            if not up:
                proc.terminate()
                log_file.close()
                with open(log_path) as f:
                    raise RuntimeError("flapi failed to start:\n" + f.read())
            yield base_url
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
            log_file.close()


def _post(base_url: str, payload: dict) -> requests.Response:
    return requests.post(f"{base_url}/mcp/jsonrpc",
                         headers={"Content-Type": "application/json"},
                         json=payload, timeout=10)


@pytest.mark.standalone_server
class TestMcpProtocolHygiene:
    def test_notification_gets_202_and_no_body(self, hygiene_server):
        r = _post(hygiene_server, {"jsonrpc": "2.0", "method": "notifications/initialized"})
        assert r.status_code == 202, r.text
        assert r.text == "", f"notification must not get a response body: {r.text!r}"

    def test_large_integer_id_is_echoed_losslessly(self, hygiene_server):
        big = 12345678901234567
        r = _post(hygiene_server, {"jsonrpc": "2.0", "id": big, "method": "tools/list", "params": {}})
        assert r.status_code == 200, r.text
        assert r.json()["id"] == big

    def test_string_id_is_echoed_as_string(self, hygiene_server):
        r = _post(hygiene_server, {"jsonrpc": "2.0", "id": "abc-1", "method": "tools/list", "params": {}})
        assert r.json()["id"] == "abc-1"

    def test_null_id_is_echoed_as_null(self, hygiene_server):
        r = _post(hygiene_server, {"jsonrpc": "2.0", "id": None, "method": "tools/list", "params": {}})
        assert r.json()["id"] is None

    def test_initialize_advertises_listchanged_false(self, hygiene_server):
        r = _post(hygiene_server, {
            "jsonrpc": "2.0", "id": 1, "method": "initialize",
            "params": {"protocolVersion": "2025-11-25", "capabilities": {},
                       "clientInfo": {"name": "c", "version": "1"}},
        })
        caps = r.json()["result"]["capabilities"]
        assert caps["tools"]["listChanged"] is False
        assert caps["resources"]["listChanged"] is False
        assert caps["prompts"]["listChanged"] is False
