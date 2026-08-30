"""Security regression matrix for MCP Layer-1 method authorization and
Layer-2 per-entity (resource / prompt) RBAC.

Reproduces and locks down the vulnerability where MCP method authorization was
skipped whenever the client omitted the ``Mcp-Session-Id`` header: an
unauthenticated caller could reach ``resources/read`` (and every other method)
and read a resource's full query result. See the ``no session header`` cases
below — before the fix, case A returned the resource payload with HTTP 200.

Boots a real flapi server with ``mcp.auth.enabled: true`` and:
  - one mcp-tool gated to role ``admin``
  - one mcp-resource WITHOUT allowed-roles (deny-by-default under auth)
  - one mcp-resource gated to role ``reader``
  - one mcp-prompt gated to role ``reader``
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


JWT_SECRET = "auth-matrix-secret"
JWT_ISSUER = "auth-matrix-issuer"


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


def _make_jwt(roles: List[str], sub: str = "auth-matrix-user") -> str:
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
    sqls = os.path.join(dirpath, "sqls")
    os.makedirs(sqls)

    flapi_yaml = f"""
project-name: mcp-auth-matrix-test
project-description: MCP auth matrix E2E
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

    # Tool gated to admin.
    with open(os.path.join(sqls, "admin_tool.yaml"), "w") as f:
        f.write("""
template-source: admin_tool.sql
connection: [inmem]
mcp-tool:
  name: admin_only_tool
  description: Tool gated to the admin role
  allowed-roles: [admin]
""")
    with open(os.path.join(sqls, "admin_tool.sql"), "w") as f:
        f.write("SELECT 'admin-result' AS message\n")

    # Resource WITHOUT allowed-roles: deny-by-default once auth is enabled.
    with open(os.path.join(sqls, "secret_resource.yaml"), "w") as f:
        f.write("""
template-source: secret_resource.sql
connection: [inmem]
mcp-resource:
  name: secret_data
  description: Sensitive resource with no allowed-roles
  mime-type: application/json
""")
    with open(os.path.join(sqls, "secret_resource.sql"), "w") as f:
        f.write("SELECT 'TOP-SECRET' AS confidential\n")

    # Resource gated to reader.
    with open(os.path.join(sqls, "gated_resource.yaml"), "w") as f:
        f.write("""
template-source: gated_resource.sql
connection: [inmem]
mcp-resource:
  name: gated_data
  description: Resource gated to the reader role
  allowed-roles: [reader]
""")
    with open(os.path.join(sqls, "gated_resource.sql"), "w") as f:
        f.write("SELECT 'reader-visible' AS ok\n")

    # Prompt gated to reader.
    with open(os.path.join(sqls, "gated_prompt.yaml"), "w") as f:
        f.write("""
mcp-prompt:
  name: gated_prompt
  description: Prompt gated to the reader role
  template: "Hello {{name}}"
  arguments: [name]
  allowed-roles: [reader]
""")

    return os.path.join(dirpath, "flapi.yaml")


@pytest.fixture
def auth_server() -> Iterator[str]:
    binary = _flapi_binary()
    port = _free_port()
    with tempfile.TemporaryDirectory(prefix="flapi_authmatrix_") as tmpdir:
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
                if "core_functions_duckdb_cpp_init" in log_text and "unique_ptr that is NULL" in log_text:
                    pytest.skip(
                        "flapi could not boot: local DuckDB extension cache is "
                        "incompatible with the in-tree DuckDB submodule."
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


def _post(base_url: str, method: str, params: dict, token: Optional[str] = None,
          session_id: Optional[str] = None) -> requests.Response:
    headers = {"Content-Type": "application/json"}
    if token:
        headers["Authorization"] = f"Bearer {token}"
    if session_id:
        headers["Mcp-Session-Id"] = session_id
    return requests.post(
        f"{base_url}/mcp/jsonrpc",
        headers=headers,
        json={"jsonrpc": "2.0", "id": "m", "method": method, "params": params},
        timeout=10,
    )


READ_SECRET = ("resources/read", {"uri": "flapi://secret_data"})
READ_GATED = ("resources/read", {"uri": "flapi://gated_data"})
GET_PROMPT = ("prompts/get", {"name": "gated_prompt", "arguments": {"name": "x"}})
CALL_TOOL = ("tools/call", {"name": "admin_only_tool", "arguments": {}})
LIST_TOOLS = ("tools/list", {})


@pytest.mark.standalone_server
class TestMcpAuthMatrix:
    # ---- Layer-1: method auth cannot be bypassed by omitting the session header ----

    @pytest.mark.parametrize("method,params", [READ_SECRET, READ_GATED, GET_PROMPT, CALL_TOOL, LIST_TOOLS])
    def test_no_session_no_auth_is_denied(self, auth_server, method, params):
        """The core vulnerability: no Mcp-Session-Id, no Authorization -> denied."""
        r = _post(auth_server, method, params)
        body = r.json()
        assert "error" in body, f"{method} leaked without auth: {body}"
        assert "TOP-SECRET" not in r.text
        assert "reader-visible" not in r.text

    @pytest.mark.parametrize("method,params", [READ_SECRET, READ_GATED, GET_PROMPT, CALL_TOOL, LIST_TOOLS])
    def test_forged_session_no_auth_is_denied(self, auth_server, method, params):
        r = _post(auth_server, method, params, session_id="forged-session-id")
        body = r.json()
        assert "error" in body, f"{method} leaked with forged session: {body}"
        assert "TOP-SECRET" not in r.text
        assert "reader-visible" not in r.text

    def test_invalid_token_no_session_is_denied(self, auth_server):
        r = _post(auth_server, *READ_SECRET, token="not.a.jwt")
        body = r.json()
        assert "error" in body, body
        assert "TOP-SECRET" not in r.text

    # ---- Layer-2: per-entity RBAC behind a valid token ----

    def test_valid_token_denied_when_resource_has_no_allowed_roles(self, auth_server):
        """Deny-by-default: a resource with no allowed-roles is closed under auth."""
        token = _make_jwt(roles=["admin"])
        r = _post(auth_server, *READ_SECRET, token=token)
        body = r.json()
        assert "error" in body, f"secret_data should deny-by-default: {body}"
        assert "TOP-SECRET" not in r.text

    def test_wrong_role_denied_on_gated_resource(self, auth_server):
        token = _make_jwt(roles=["admin"])
        r = _post(auth_server, *READ_GATED, token=token)
        body = r.json()
        assert "error" in body, body
        assert "reader-visible" not in r.text

    def test_matching_role_allowed_on_gated_resource(self, auth_server):
        token = _make_jwt(roles=["reader"])
        r = _post(auth_server, *READ_GATED, token=token)
        body = r.json()
        assert "error" not in body, f"reader should read gated_data: {body}"
        assert "reader-visible" in r.text

    def test_wrong_role_denied_on_gated_prompt(self, auth_server):
        token = _make_jwt(roles=["admin"])
        r = _post(auth_server, *GET_PROMPT, token=token)
        body = r.json()
        assert "error" in body, body

    def test_matching_role_allowed_on_gated_prompt(self, auth_server):
        token = _make_jwt(roles=["reader"])
        r = _post(auth_server, *GET_PROMPT, token=token)
        body = r.json()
        assert "error" not in body, f"reader should get gated_prompt: {body}"

    # ---- A valid, authorized caller still works with no session header (stateless) ----

    def test_valid_admin_can_call_tool_without_session_header(self, auth_server):
        token = _make_jwt(roles=["admin"])
        r = _post(auth_server, *CALL_TOOL, token=token)
        body = r.json()
        assert "error" not in body, f"admin tool call should succeed: {body}"
        assert "admin-result" in r.text

    # ---- C5: HTTP status + WWW-Authenticate (RFC 9728 / RFC 6750) ----

    def test_unauthenticated_gets_401_with_www_authenticate(self, auth_server):
        r = _post(auth_server, *READ_SECRET)
        assert r.status_code == 401, r.text
        assert r.headers.get("WWW-Authenticate", "").startswith("Bearer")

    def test_authenticated_wrong_role_gets_403_insufficient_scope(self, auth_server):
        # gated_data requires 'reader'; an 'admin' token is authenticated but
        # lacks the role -> 403 insufficient_scope.
        token = _make_jwt(roles=["admin"])
        r = _post(auth_server, *READ_GATED, token=token)
        assert r.status_code == 403, r.text
        assert "insufficient_scope" in r.headers.get("WWW-Authenticate", "")

    def test_well_known_metadata_absent_without_oidc(self, auth_server):
        # This fixture uses bearer auth (no OIDC authorization server), so the
        # protected-resource metadata document is not served.
        r = requests.get(f"{auth_server}/.well-known/oauth-protected-resource", timeout=10)
        assert r.status_code == 404
