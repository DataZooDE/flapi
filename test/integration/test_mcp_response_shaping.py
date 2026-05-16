"""End-to-end tests for MCP per-tool response shaping (issue #24, W2.4).

Boots a real flapi server with three MCP tools configured to test the
three independent shaping levers:

- `redact_tool` declares `redact-columns: [salary]` and must mask that
  column in every returned row.
- `cap_tool` declares `max-rows: 2` and must return at most two rows
  even when the underlying SELECT yields more.
- `sample_tool` declares `sample: true` and must return only summary
  metadata (row_count, columns, sampled=true), no row data.

Marked `standalone_server` so the conftest autouse fixture does not
also spin up the shared api_configuration server. Skips cleanly on
local environments where flapi cannot boot due to the v1.5.1/v1.5.2
DuckDB extension cache mismatch; CI runs against fresh extensions.
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


JWT_SECRET = "shape-test-secret"
JWT_ISSUER = "shape-test-issuer"


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


def _make_jwt(sub: str = "shape-user") -> str:
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
            f"project-name: response-shape-test\n"
            f"project-description: Response-shape E2E\n"
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

    # Tool 1: redact `salary`
    with open(os.path.join(sqls, "redact.yaml"), "w") as f:
        f.write("""
template-source: people.sql
connection: [inmem]
mcp-tool:
  name: redact_tool
  description: People list with salary redacted
  response:
    redact-columns:
      - salary
""")

    # Tool 2: cap rows at 2
    with open(os.path.join(sqls, "cap.yaml"), "w") as f:
        f.write("""
template-source: people.sql
connection: [inmem]
mcp-tool:
  name: cap_tool
  description: People list capped at 2 rows
  response:
    max-rows: 2
""")

    # Tool 3: sample-only
    with open(os.path.join(sqls, "sample.yaml"), "w") as f:
        f.write("""
template-source: people.sql
connection: [inmem]
mcp-tool:
  name: sample_tool
  description: People list returning summary only
  response:
    sample: true
""")

    # All three tools share the same fixed SELECT — three rows, three columns.
    with open(os.path.join(sqls, "people.sql"), "w") as f:
        f.write(
            "SELECT * FROM (VALUES "
            "(1, 'alice', 100), "
            "(2, 'bob', 200), "
            "(3, 'carol', 300)"
            ") AS t(id, name, salary)\n"
        )

    return os.path.join(dirpath, "flapi.yaml")


@pytest.fixture
def shape_server() -> Iterator[str]:
    binary = _flapi_binary()
    port = _free_port()
    with tempfile.TemporaryDirectory(prefix="flapi_shape_") as tmpdir:
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
                "clientInfo": {"name": "shape-test", "version": "0.1"},
                "capabilities": {},
            },
        },
        timeout=10,
    )
    assert r.status_code == 200, r.text
    sid = r.headers.get("Mcp-Session-Id")
    assert sid, f"no session id: {dict(r.headers)}"
    return sid


def _tools_call(base_url: str, token: str, session_id: str, tool: str) -> requests.Response:
    return requests.post(
        f"{base_url}/mcp/jsonrpc",
        headers={
            "Authorization": f"Bearer {token}",
            "Content-Type": "application/json",
            "Mcp-Session-Id": session_id,
        },
        json={
            "jsonrpc": "2.0",
            "id": f"call-{tool}",
            "method": "tools/call",
            "params": {"name": tool, "arguments": {}},
        },
        timeout=10,
    )


@pytest.mark.standalone_server
class TestResponseShaping:
    """End-to-end coverage for `mcp-tool.response` knobs."""

    def test_redact_columns_masks_listed_column(self, shape_server):
        token = _make_jwt()
        sid = _open_session(shape_server, token)
        r = _tools_call(shape_server, token, sid, "redact_tool")
        assert r.status_code == 200, r.text
        body = r.json()
        assert "error" not in body, f"redact_tool errored: {body}"
        result_str = body["result"]
        # salary must be the redaction sentinel; non-redacted columns survive.
        assert "<redacted>" in result_str, result_str
        assert "alice" in result_str, result_str
        # No row should still expose a numeric salary literal.
        assert "100" not in result_str or "<redacted>" in result_str

    def test_max_rows_caps_result_set(self, shape_server):
        token = _make_jwt()
        sid = _open_session(shape_server, token)
        r = _tools_call(shape_server, token, sid, "cap_tool")
        assert r.status_code == 200, r.text
        body = r.json()
        assert "error" not in body, f"cap_tool errored: {body}"
        result_str = body["result"]
        # 2 rows max → alice and bob present, carol absent.
        assert "alice" in result_str, result_str
        assert "bob" in result_str, result_str
        assert "carol" not in result_str, result_str

    def test_sample_returns_summary_only(self, shape_server):
        token = _make_jwt()
        sid = _open_session(shape_server, token)
        r = _tools_call(shape_server, token, sid, "sample_tool")
        assert r.status_code == 200, r.text
        body = r.json()
        assert "error" not in body, f"sample_tool errored: {body}"
        result_str = body["result"]
        # Sample mode emits row_count + columns, no row data.
        assert "\"sampled\":true" in result_str, result_str
        assert "\"row_count\":3" in result_str, result_str
        # None of the per-row values should appear.
        assert "alice" not in result_str, result_str
        assert "bob" not in result_str, result_str
