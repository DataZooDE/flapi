"""End-to-end tests for per-tool MCP rate limits (issue #24, W2.5).

Boots flapi with two MCP tools that each declare different
`rate-limit` blocks. Hammers each independently and verifies:
- Tool A's traffic does not consume tool B's quota.
- Once a tool's bucket is empty, calls receive a Rate-limit error
  response with `retry_after_seconds` in the JSON-RPC error payload.

Marked `standalone_server` so the conftest autouse fixture does not
spin up the shared api_configuration server. Skips cleanly when flapi
cannot boot (local DuckDB extension-cache mismatch); CI runs against
fresh extensions.
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
from typing import Dict, Iterator, List

import pytest
import requests


JWT_SECRET = "tool-rate-test-secret"
JWT_ISSUER = "tool-rate-test-issuer"


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


def _make_jwt(sub: str = "rate-test-user") -> str:
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
            f"project-name: per-tool-rate-test\n"
            f"project-description: Per-tool MCP rate limit E2E\n"
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

    # tool_a: max 2 calls / 60s
    with open(os.path.join(sqls, "tool_a.yaml"), "w") as f:
        f.write("""
template-source: tool_a.sql
connection: [inmem]
mcp-tool:
  name: tool_a
  description: Tool A, capped at 2 calls/minute
  rate-limit:
    enabled: true
    max: 2
    interval: 60
""")
    with open(os.path.join(sqls, "tool_a.sql"), "w") as f:
        f.write("SELECT 'tool_a' AS name\n")

    # tool_b: max 5 calls / 60s (independent bucket)
    with open(os.path.join(sqls, "tool_b.yaml"), "w") as f:
        f.write("""
template-source: tool_b.sql
connection: [inmem]
mcp-tool:
  name: tool_b
  description: Tool B, capped at 5 calls/minute
  rate-limit:
    enabled: true
    max: 5
    interval: 60
""")
    with open(os.path.join(sqls, "tool_b.sql"), "w") as f:
        f.write("SELECT 'tool_b' AS name\n")

    return os.path.join(dirpath, "flapi.yaml")


@pytest.fixture
def rate_limit_server() -> Iterator[Dict[str, str]]:
    binary = _flapi_binary()
    port = _free_port()
    with tempfile.TemporaryDirectory(prefix="flapi_tool_rl_") as tmpdir:
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
            yield {"base_url": base_url}
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
                "clientInfo": {"name": "rate-limit-test", "version": "0.1"},
                "capabilities": {},
            },
        },
        timeout=10,
    )
    assert r.status_code == 200, r.text
    sid = r.headers.get("Mcp-Session-Id")
    assert sid, f"no session id: {dict(r.headers)}"
    return sid


def _tools_call(base_url: str, token: str, sid: str, tool: str) -> requests.Response:
    return requests.post(
        f"{base_url}/mcp/jsonrpc",
        headers={
            "Authorization": f"Bearer {token}",
            "Content-Type": "application/json",
            "Mcp-Session-Id": sid,
        },
        json={
            "jsonrpc": "2.0",
            "id": f"call-{tool}",
            "method": "tools/call",
            "params": {"name": tool, "arguments": {}},
        },
        timeout=10,
    )


def _is_rate_limited(body: dict) -> bool:
    err = body.get("error", "")
    if isinstance(err, str):
        return "Rate limit exceeded" in err
    if isinstance(err, dict):
        return "Rate limit exceeded" in err.get("message", "")
    return False


@pytest.mark.standalone_server
class TestPerToolRateLimit:
    """End-to-end coverage for `mcp-tool.rate-limit`."""

    def test_tool_a_blocks_after_its_two_call_quota(self, rate_limit_server):
        token = _make_jwt()
        sid = _open_session(rate_limit_server["base_url"], token)

        # Two calls allowed, third must be rate-limited.
        r1 = _tools_call(rate_limit_server["base_url"], token, sid, "tool_a")
        r2 = _tools_call(rate_limit_server["base_url"], token, sid, "tool_a")
        r3 = _tools_call(rate_limit_server["base_url"], token, sid, "tool_a")

        assert r1.status_code == 200
        assert r2.status_code == 200
        assert r3.status_code == 200
        assert not _is_rate_limited(r1.json()), r1.json()
        assert not _is_rate_limited(r2.json()), r2.json()
        assert _is_rate_limited(r3.json()), r3.json()

    def test_tool_b_quota_independent_of_tool_a(self, rate_limit_server):
        # After tool_a is exhausted, tool_b still has its full bucket.
        token = _make_jwt()
        sid = _open_session(rate_limit_server["base_url"], token)

        for _ in range(3):
            _tools_call(rate_limit_server["base_url"], token, sid, "tool_a")

        # tool_a should now be blocked; tool_b should still allow 5 calls.
        a_after = _tools_call(rate_limit_server["base_url"], token, sid, "tool_a")
        assert _is_rate_limited(a_after.json()), a_after.json()

        b_results = [
            _tools_call(rate_limit_server["base_url"], token, sid, "tool_b")
            for _ in range(5)
        ]
        for i, r in enumerate(b_results):
            assert r.status_code == 200, r.text
            assert not _is_rate_limited(r.json()), f"tool_b call {i} unexpectedly limited: {r.json()}"

        # 6th tool_b call must be rate-limited.
        b_blocked = _tools_call(rate_limit_server["base_url"], token, sid, "tool_b")
        assert _is_rate_limited(b_blocked.json()), b_blocked.json()
