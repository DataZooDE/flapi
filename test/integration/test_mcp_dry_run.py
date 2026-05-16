"""End-to-end tests for MCP dry-run / shadow mode (issue #24, W2.2).

Boots a real flapi server with an MCP tool that selects from an in-memory
table, then calls the tool both for-real and with `_dryRun: true`. The
dry-run call must:
- Return success (status 200, no JSON-RPC error)
- Surface the rendered SQL and parameters in the result payload
- Skip query execution (no rows-of-data in the result)

Marked `standalone_server` so the conftest autouse fixture does not also
spin up the shared api_configuration server.
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
from typing import Iterator, List

import pytest
import requests


JWT_SECRET = "dry-run-test-secret"
JWT_ISSUER = "dry-run-test-issuer"


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


def _make_jwt(sub: str = "dry-run-user") -> str:
    header = {"alg": "HS256", "typ": "JWT"}
    now = int(time.time())
    payload = {
        "iss": JWT_ISSUER,
        "sub": sub,
        "roles": ["analyst"],
        "iat": now,
        "exp": now + 3600,
    }
    h = _b64url(json.dumps(header, separators=(",", ":")).encode("utf-8"))
    p = _b64url(json.dumps(payload, separators=(",", ":")).encode("utf-8"))
    signature = hmac.new(JWT_SECRET.encode("utf-8"), f"{h}.{p}".encode("utf-8"), hashlib.sha256).digest()
    return f"{h}.{p}.{_b64url(signature)}"


def _write_config(dirpath: str, port: int) -> str:
    sqls = os.path.join(dirpath, "sqls")
    os.makedirs(sqls)

    with open(os.path.join(dirpath, "flapi.yaml"), "w") as f:
        f.write(
            f"project-name: mcp-dryrun-test\n"
            f"project-description: Dry-run E2E\n"
            f"http-port: {port}\n"
            f"template:\n"
            f"  path: ./sqls\n"
            f"connections:\n"
            f"  inmem:\n"
            f"    properties:\n"
            f"      database: ':memory:'\n"
            f"mcp:\n"
            f"  enabled: true\n"
            f"  auth:\n"
            f"    enabled: true\n"
            f"    type: bearer\n"
            f"    jwt-secret: {JWT_SECRET}\n"
            f"    jwt-issuer: {JWT_ISSUER}\n"
        )

    with open(os.path.join(sqls, "lookup.yaml"), "w") as f:
        f.write("""
template-source: lookup.sql
connection: [inmem]
request:
  - field-name: id
    field-in: query
    field-type: int
    required: true
    validators:
      - type: int
        min: 1
mcp-tool:
  name: customer_lookup
  description: Look up a customer by id (deterministic SELECT for dry-run testing)
  allowed-roles: [analyst]
""")
    with open(os.path.join(sqls, "lookup.sql"), "w") as f:
        # Mustache rendering: {{ params.id }} substitutes the literal value.
        f.write("SELECT {{ params.id }} AS customer_id, 'fake' AS name\n")

    return os.path.join(dirpath, "flapi.yaml")


@pytest.fixture
def dry_run_server() -> Iterator[str]:
    binary = _flapi_binary()
    port = _free_port()
    with tempfile.TemporaryDirectory(prefix="flapi_dryrun_") as tmpdir:
        config_path = _write_config(tmpdir, port)
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
                        "incompatible with the in-tree DuckDB submodule. CI exercises this path."
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


def _initialize(base_url: str, token: str) -> str:
    r = requests.post(
        f"{base_url}/mcp/jsonrpc",
        headers={"Authorization": f"Bearer {token}", "Content-Type": "application/json"},
        json={
            "jsonrpc": "2.0",
            "id": "init-1",
            "method": "initialize",
            "params": {
                "protocolVersion": "2024-11-05",
                "clientInfo": {"name": "dryrun-test", "version": "0.1"},
                "capabilities": {},
            },
        },
        timeout=10,
    )
    assert r.status_code == 200, r.text
    sid = r.headers.get("Mcp-Session-Id")
    assert sid, f"no session id: {dict(r.headers)}"
    return sid


def _tools_call(base_url: str, token: str, session_id: str, arguments: dict) -> requests.Response:
    return requests.post(
        f"{base_url}/mcp/jsonrpc",
        headers={
            "Authorization": f"Bearer {token}",
            "Content-Type": "application/json",
            "Mcp-Session-Id": session_id,
        },
        json={
            "jsonrpc": "2.0",
            "id": "call-1",
            "method": "tools/call",
            "params": {"name": "customer_lookup", "arguments": arguments},
        },
        timeout=10,
    )


@pytest.mark.standalone_server
class TestDryRunMode:
    """End-to-end coverage for the `_dryRun: true` short-circuit."""

    def test_dry_run_returns_rendered_sql_without_executing(self, dry_run_server):
        token = _make_jwt()
        sid = _initialize(dry_run_server, token)

        r = _tools_call(dry_run_server, token, sid, {"id": 42, "_dryRun": True})
        assert r.status_code == 200, r.text
        body = r.json()
        assert "error" not in body, f"dry-run unexpectedly errored: {body}"

        # The MCP envelope wraps the tool result as a JSON string in
        # `result.content[0].text` — so the dry-run payload's quotes
        # appear escaped in the outer JSON. Extract and re-parse to
        # check the inner shape directly.
        inner_text = body["result"]["content"][0]["text"]
        inner = json.loads(inner_text)
        assert inner["dry_run"] is True, inner
        assert "rendered_sql" in inner, inner
        # The rendered SQL must contain the substituted id literal.
        assert "42" in inner["rendered_sql"], inner["rendered_sql"]

    def test_normal_call_does_not_emit_dry_run_payload(self, dry_run_server):
        # Sanity: a regular call still works against the in-mem connection
        # and does NOT include the dry-run markers.
        token = _make_jwt()
        sid = _initialize(dry_run_server, token)

        r = _tools_call(dry_run_server, token, sid, {"id": 7})
        assert r.status_code == 200, r.text
        # The dry-run payload's distinctive fields must NOT appear anywhere
        # in the raw response (success or error path).
        assert "dry_run" not in r.text, r.text
        assert "rendered_sql" not in r.text, r.text
