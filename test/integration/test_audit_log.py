"""End-to-end tests for the JSONL audit log (issue #23, W1.3).

Boots a real flapi server with the audit log enabled and pointed at a
file in a temp directory. After issuing one or more MCP tool calls,
the audit file must contain one well-formed JSON line per call with
the expected fields and redaction applied to configured keys.

Marked `standalone_server` so the conftest autouse fixture does not
spin up the shared api_configuration server. Skips cleanly on local
environments where flapi cannot boot due to the v1.5.1/v1.5.2 DuckDB
extension-cache mismatch; CI runs against fresh extensions.
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
from typing import Dict, Iterator, List, Optional

import pytest
import requests


JWT_SECRET = "audit-test-secret"
JWT_ISSUER = "audit-test-issuer"


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


def _make_jwt(sub: str = "audit-user", roles: Optional[List[str]] = None) -> str:
    if roles is None:
        roles = ["analyst"]
    header = {"alg": "HS256", "typ": "JWT"}
    now = int(time.time())
    payload = {
        "iss": JWT_ISSUER,
        "sub": sub,
        "roles": roles,
        "iat": now,
        "exp": now + 3600,
    }
    h = _b64url(json.dumps(header, separators=(",", ":")).encode("utf-8"))
    p = _b64url(json.dumps(payload, separators=(",", ":")).encode("utf-8"))
    signature = hmac.new(JWT_SECRET.encode("utf-8"), f"{h}.{p}".encode("utf-8"), hashlib.sha256).digest()
    return f"{h}.{p}.{_b64url(signature)}"


def _write_config(dirpath: str, port: int, audit_path: str) -> str:
    sqls = os.path.join(dirpath, "sqls")
    os.makedirs(sqls)

    with open(os.path.join(dirpath, "flapi.yaml"), "w") as f:
        f.write(
            f"project-name: audit-log-test\n"
            f"project-description: Audit log E2E\n"
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
            f"audit:\n"
            f"  enabled: true\n"
            f"  sink: file\n"
            f"  path: {audit_path}\n"
            f"  redact:\n"
            f"    - token\n"
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
  - field-name: token
    field-in: query
    field-type: string
    required: false
mcp-tool:
  name: customer_lookup
  description: Look up a customer by id
  allowed-roles: [analyst]
""")
    with open(os.path.join(sqls, "lookup.sql"), "w") as f:
        f.write("SELECT {{ params.id }} AS id\n")

    return os.path.join(dirpath, "flapi.yaml")


@pytest.fixture
def audit_server() -> Iterator[Dict[str, str]]:
    """Start a flapi server with audit enabled; yield {base_url, audit_path}."""
    binary = _flapi_binary()
    port = _free_port()
    with tempfile.TemporaryDirectory(prefix="flapi_audit_") as tmpdir:
        audit_path = os.path.join(tmpdir, "audit.jsonl")
        config_path = _write_config(tmpdir, port, audit_path)
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
            yield {"base_url": base_url, "audit_path": audit_path}
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
            log_file.close()


def _open_session(base_url: str, token: str) -> str:
    r = requests.post(
        f"{base_url}/mcp/jsonrpc",
        headers={"Authorization": f"Bearer {token}", "Content-Type": "application/json"},
        json={
            "jsonrpc": "2.0",
            "id": "init-1",
            "method": "initialize",
            "params": {
                "protocolVersion": "2024-11-05",
                "clientInfo": {"name": "audit-test", "version": "0.1"},
                "capabilities": {},
            },
        },
        timeout=10,
    )
    assert r.status_code == 200, r.text
    sid = r.headers.get("Mcp-Session-Id")
    assert sid, f"no session id: {dict(r.headers)}"
    return sid


def _tools_call(base_url: str, token: str, session_id: str, args: dict) -> requests.Response:
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
            "params": {"name": "customer_lookup", "arguments": args},
        },
        timeout=10,
    )


def _read_audit_lines(audit_path: str) -> List[dict]:
    """Wait briefly for the audit file to be flushed, then read parsed lines."""
    deadline = time.time() + 5
    lines: List[str] = []
    while time.time() < deadline:
        if os.path.exists(audit_path):
            with open(audit_path) as f:
                lines = [l for l in f.read().splitlines() if l.strip()]
            if lines:
                break
        time.sleep(0.1)
    return [json.loads(l) for l in lines]


@pytest.mark.standalone_server
class TestAuditLog:
    """End-to-end coverage for `audit.enabled` + `audit.sink=file`."""

    def test_successful_tool_call_writes_audit_line(self, audit_server):
        token = _make_jwt(sub="alice")
        sid = _open_session(audit_server["base_url"], token)

        r = _tools_call(audit_server["base_url"], token, sid, {"id": 42, "token": "secret123"})
        assert r.status_code == 200, r.text

        events = _read_audit_lines(audit_server["audit_path"])
        # There should be exactly one event for the tool call.
        assert len(events) == 1, events
        ev = events[0]
        assert ev["method"] == "tools/call"
        assert ev["target"] == "customer_lookup"
        assert ev["principal"] == "alice"
        # Status may be success or error depending on whether the DB actually
        # executed the query (e.g., on environments where in-mem works); the
        # contract here is "an event was emitted", not "the query succeeded".
        assert ev["status"], ev
        assert "request_id" in ev and ev["request_id"]
        assert "timestamp" in ev and ev["timestamp"]
        # Redaction: token must be masked, non-redacted id must survive.
        assert ev["params"]["token"] == "<redacted>"
        assert ev["params"]["id"] == "42"
        # The literal secret must not appear anywhere in the line.
        with open(audit_server["audit_path"]) as f:
            content = f.read()
        assert "secret123" not in content

    def test_invalid_arguments_still_audited(self, audit_server):
        # When validation rejects the call, an audit event is still emitted
        # so operators can investigate "why is this client failing?" without
        # turning on debug logging.
        token = _make_jwt(sub="bob")
        sid = _open_session(audit_server["base_url"], token)

        # `id` is required and must be int>=1; send a string instead.
        r = _tools_call(audit_server["base_url"], token, sid, {"id": "not-a-number"})
        assert r.status_code == 200, r.text  # JSON-RPC error in body

        events = _read_audit_lines(audit_server["audit_path"])
        assert len(events) == 1, events
        ev = events[0]
        assert ev["status"].startswith("error:"), ev
        assert ev["principal"] == "bob"
        assert ev["target"] == "customer_lookup"
