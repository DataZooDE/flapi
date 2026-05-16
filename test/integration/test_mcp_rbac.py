"""End-to-end tests for per-tool MCP RBAC (issue #24, W2.1).

Boots a real flapi server with MCP auth enabled and two MCP tools that
declare different `allowed-roles`. Calls each tool with JWTs carrying
different role claims and asserts that the per-tool authorisation policy
allows or denies as expected.
"""

import base64
import hashlib
import hmac
import json
import os
import socket
import subprocess
import tempfile
import time
from typing import Iterator, List, Optional

import pytest
import requests


JWT_SECRET = "rbac-test-secret"
JWT_ISSUER = "rbac-test-issuer"


def _repo_root() -> str:
    return os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def _flapi_binary() -> str:
    candidates: List[str] = []
    for build_type in ("release", "debug"):
        path = os.path.join(_repo_root(), "build", build_type, "flapi")
        if os.path.exists(path):
            candidates.append(path)
    if not candidates:
        pytest.skip("flapi binary not found in build/release or build/debug")
    candidates.sort(key=os.path.getmtime, reverse=True)
    return candidates[0]


def _free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def _b64url(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode("utf-8")


def _make_jwt(roles: List[str], sub: str = "rbac-test-user") -> str:
    header = {"alg": "HS256", "typ": "JWT"}
    now = int(time.time())
    payload = {
        "iss": JWT_ISSUER,
        "sub": sub,
        "roles": roles,
        "iat": now,
        "exp": now + 3600,
    }
    header_b64 = _b64url(json.dumps(header, separators=(",", ":")).encode("utf-8"))
    payload_b64 = _b64url(json.dumps(payload, separators=(",", ":")).encode("utf-8"))
    signing_input = f"{header_b64}.{payload_b64}".encode("utf-8")
    signature = hmac.new(JWT_SECRET.encode("utf-8"), signing_input, hashlib.sha256).digest()
    return f"{header_b64}.{payload_b64}.{_b64url(signature)}"


def _write_config_tree(dirpath: str, port: int) -> str:
    """Write flapi.yaml plus two role-gated MCP tools into a temp dir."""
    sqls = os.path.join(dirpath, "sqls")
    os.makedirs(sqls)

    flapi_yaml = f"""
project-name: mcp-rbac-test
project-description: Per-tool RBAC E2E
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
  auth:
    enabled: true
    type: bearer
    jwt-secret: {JWT_SECRET}
    jwt-issuer: {JWT_ISSUER}
"""
    with open(os.path.join(dirpath, "flapi.yaml"), "w") as f:
        f.write(flapi_yaml)

    with open(os.path.join(sqls, "admin_tool.yaml"), "w") as f:
        f.write("""
template-source: admin_tool.sql
connection: [inmem]
mcp-tool:
  name: admin_only_tool
  description: Tool gated to the admin role
  allowed-roles:
    - admin
""")
    with open(os.path.join(sqls, "admin_tool.sql"), "w") as f:
        f.write("SELECT 'admin-result' AS message\n")

    with open(os.path.join(sqls, "analyst_tool.yaml"), "w") as f:
        f.write("""
template-source: analyst_tool.sql
connection: [inmem]
mcp-tool:
  name: analyst_only_tool
  description: Tool gated to the analyst role
  allowed-roles:
    - analyst
""")
    with open(os.path.join(sqls, "analyst_tool.sql"), "w") as f:
        f.write("SELECT 'analyst-result' AS message\n")

    return os.path.join(dirpath, "flapi.yaml")


@pytest.fixture
def rbac_server() -> Iterator[str]:
    """Start a flapi server configured for the RBAC tests; yield base URL."""
    binary = _flapi_binary()
    port = _free_port()
    with tempfile.TemporaryDirectory(prefix="flapi_rbac_") as tmpdir:
        config_path = _write_config_tree(tmpdir, port)
        log_path = os.path.join(tmpdir, "server.log")
        log_file = open(log_path, "w")
        proc = subprocess.Popen(
            [binary, "-c", config_path, "--no-telemetry"],
            cwd=tmpdir,
            stdout=log_file,
            stderr=subprocess.STDOUT,
        )
        try:
            base_url = f"http://127.0.0.1:{port}"
            deadline = time.time() + 30
            up = False
            while time.time() < deadline:
                # If the process already died, no point polling further.
                if proc.poll() is not None:
                    break
                try:
                    r = requests.get(f"{base_url}/mcp/health", timeout=1)
                    if r.status_code < 500:
                        up = True
                        break
                except requests.exceptions.RequestException:
                    time.sleep(0.5)
            if not up:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                log_file.close()
                with open(log_path) as f:
                    log_text = f.read()
                # A locally-corrupt or version-mismatched DuckDB extension cache
                # surfaces here as a startup crash; that's an environment fault,
                # not a test failure. CI runs against fresh caches and exercises
                # this path normally.
                if "core_functions_duckdb_cpp_init" in log_text and "unique_ptr that is NULL" in log_text:
                    pytest.skip(
                        "flapi could not boot: local DuckDB extension cache is "
                        "incompatible with the in-tree DuckDB submodule. "
                        "Refresh ~/.duckdb/extensions or run this test in CI."
                    )
                raise RuntimeError(f"flapi failed to start. Log:\n{log_text}")
            yield base_url
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
            log_file.close()


def _mcp_initialize(base_url: str, token: str) -> requests.Response:
    return requests.post(
        f"{base_url}/mcp/jsonrpc",
        headers={"Authorization": f"Bearer {token}", "Content-Type": "application/json"},
        json={
            "jsonrpc": "2.0",
            "id": "init-1",
            "method": "initialize",
            "params": {
                "protocolVersion": "2024-11-05",
                "clientInfo": {"name": "rbac-test-client", "version": "0.1"},
                "capabilities": {},
            },
        },
        timeout=10,
    )


def _mcp_tools_call(
    base_url: str, token: str, tool_name: str, session_id: Optional[str] = None
) -> requests.Response:
    headers = {"Authorization": f"Bearer {token}", "Content-Type": "application/json"}
    if session_id:
        headers["Mcp-Session-Id"] = session_id
    return requests.post(
        f"{base_url}/mcp/jsonrpc",
        headers=headers,
        json={
            "jsonrpc": "2.0",
            "id": f"call-{tool_name}",
            "method": "tools/call",
            "params": {"name": tool_name, "arguments": {}},
        },
        timeout=10,
    )


def _open_session(base_url: str, token: str) -> str:
    """Initialize an MCP session with the given JWT and return the session id."""
    r = _mcp_initialize(base_url, token)
    assert r.status_code == 200, f"initialize failed: {r.status_code} {r.text}"
    session_id = r.headers.get("Mcp-Session-Id")
    assert session_id, f"initialize did not return Mcp-Session-Id header: {dict(r.headers)}"
    return session_id


@pytest.mark.standalone_server
class TestPerToolRbac:
    """End-to-end coverage for `mcp-tool.allowed-roles` enforcement.

    Marked `standalone_server` so the conftest autouse fixture does not also
    spin up the shared api_configuration server — each test here starts its
    own flapi process via the `rbac_server` fixture.
    """

    def test_admin_token_can_call_admin_tool(self, rbac_server):
        token = _make_jwt(roles=["admin"])
        sid = _open_session(rbac_server, token)

        r = _mcp_tools_call(rbac_server, token, "admin_only_tool", session_id=sid)
        assert r.status_code == 200, r.text
        body = r.json()
        assert "error" not in body, f"Expected success, got error: {body}"
        # The MCP envelope wraps the tool's JSON payload in result.content[0].text.
        assert "admin-result" in r.text, body

    def test_admin_token_cannot_call_analyst_tool(self, rbac_server):
        token = _make_jwt(roles=["admin"])
        sid = _open_session(rbac_server, token)

        r = _mcp_tools_call(rbac_server, token, "analyst_only_tool", session_id=sid)
        assert r.status_code == 200, r.text
        body = r.json()
        assert "error" in body, f"Expected denial, got success: {body}"
        assert "Permission denied" in r.text
        assert "analyst" in r.text

    def test_analyst_token_can_call_analyst_tool(self, rbac_server):
        token = _make_jwt(roles=["analyst"])
        sid = _open_session(rbac_server, token)

        r = _mcp_tools_call(rbac_server, token, "analyst_only_tool", session_id=sid)
        assert r.status_code == 200, r.text
        body = r.json()
        assert "error" not in body, body
        assert "analyst-result" in r.text

    def test_analyst_token_cannot_call_admin_tool(self, rbac_server):
        token = _make_jwt(roles=["analyst"])
        sid = _open_session(rbac_server, token)

        r = _mcp_tools_call(rbac_server, token, "admin_only_tool", session_id=sid)
        assert r.status_code == 200, r.text
        body = r.json()
        assert "error" in body, body
        assert "Permission denied" in r.text
        assert "admin" in r.text

    def test_token_with_no_roles_is_denied_for_role_gated_tools(self, rbac_server):
        token = _make_jwt(roles=[])
        sid = _open_session(rbac_server, token)

        for tool in ("admin_only_tool", "analyst_only_tool"):
            r = _mcp_tools_call(rbac_server, token, tool, session_id=sid)
            assert r.status_code == 200, r.text
            body = r.json()
            assert "error" in body, f"{tool} unexpectedly allowed: {body}"
            assert "Permission denied" in r.text
