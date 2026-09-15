"""SEP-414 trace-context ingestion, end to end (epic issue 3).

This is the acceptance test for the conformance defect the whole epic began from.

flAPI advertises MCP revision 2026-07-28. That revision, via SEP-414 (status
Final), reserves `traceparent` inside `params._meta` as an UNPREFIXED key.
Conforming clients - the MCP C#/Python SDKs, OpenInference, Logfire, Envoy AI
Gateway, ToolHive - already send it on every tools/call. flAPI parsed _meta for
three io.modelcontextprotocol/* keys and walked straight past traceparent, so
every call was a hole in the caller's trace.

These tests drive the real binary and assert the extracted context reaches the
audit log, which is where correlation becomes observable without an exporter.
"""
import json
import os
import signal
import socket
import subprocess
import tempfile
import time
from typing import Dict, Iterator, List

import pytest
import requests

# A fixed, known context, so nothing is correlated heuristically.
TRACE_ID = "4bf92f3577b34da6a3ce929d0e0e4736"
SPAN_ID = "00f067aa0ba902b7"
TRACEPARENT = f"00-{TRACE_ID}-{SPAN_ID}-01"

OTHER_TRACE_ID = "11111111111111111111111111111111"
OTHER_SPAN_ID = "1111111111111111"
OTHER_TRACEPARENT = f"00-{OTHER_TRACE_ID}-{OTHER_SPAN_ID}-01"


def _repo_root() -> str:
    return os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def _flapi_binary() -> str:
    build_type = os.getenv("FLAPI_BUILD_TYPE", "release")
    for candidate in (
        os.path.join(_repo_root(), "build", build_type, "flapi"),
        os.path.join(_repo_root(), "build", "release", "flapi"),
        os.path.join(_repo_root(), "build", "debug", "flapi"),
    ):
        if os.path.exists(candidate):
            return candidate
    pytest.skip("flapi binary not found")


def _free_port() -> int:
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def _write_config(dirpath: str, port: int, audit_path: str) -> str:
    sqls = os.path.join(dirpath, "sqls")
    os.makedirs(sqls)
    with open(os.path.join(dirpath, "flapi.yaml"), "w") as f:
        f.write(
            f"project-name: trace-context-test\n"
            f"project-description: SEP-414 trace context E2E\n"
            f"http-port: {port}\n"
            f"template:\n"
            f"  path: ./sqls\n"
            f"connections:\n"
            f"  inmem:\n"
            f"    properties:\n"
            f"      database: ':memory:'\n"
            f"mcp:\n"
            f"  enabled: true\n"
            f"audit:\n"
            f"  enabled: true\n"
            f"  sink: file\n"
            f"  path: {audit_path}\n"
        )
    with open(os.path.join(sqls, "lookup.yaml"), "w") as f:
        f.write(
            "url-path: /lookup\n"
            "method: GET\n"
            "template-source: lookup.sql\n"
            "connection: [inmem]\n"
            "mcp-tool:\n"
            "  name: lookup_tool\n"
            "  description: Look something up\n"
        )
    with open(os.path.join(sqls, "lookup.sql"), "w") as f:
        f.write("SELECT 1 AS n\n")
    return os.path.join(dirpath, "flapi.yaml")


@pytest.fixture
def trace_server() -> Iterator[Dict[str, str]]:
    tmp = tempfile.mkdtemp(prefix="flapi_tracectx_")
    port = _free_port()
    audit_path = os.path.join(tmp, "audit.jsonl")
    config = _write_config(tmp, port, audit_path)
    log_path = os.path.join(tmp, "server.log")
    with open(log_path, "w") as log:
        proc = subprocess.Popen(
            [_flapi_binary(), "-c", config, "-p", str(port), "--log-level", "warning"],
            stdout=log, stderr=subprocess.STDOUT, cwd=tmp,
            env={**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"},
            preexec_fn=os.setsid,
        )
    base_url = f"http://127.0.0.1:{port}"
    deadline = time.time() + 60
    try:
        while time.time() < deadline:
            if proc.poll() is not None:
                with open(log_path) as fh:
                    pytest.fail(f"server exited during startup:\n{fh.read()}")
            try:
                if requests.get(f"{base_url}/health/live", timeout=1).status_code == 200:
                    break
            except requests.RequestException:
                time.sleep(0.3)
        else:
            pytest.fail("server never became live")
        yield {"base_url": base_url, "audit_path": audit_path}
    finally:
        try:
            os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
            proc.wait(timeout=10)
        except Exception:
            try:
                os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
            except Exception:
                pass


def _audit(path: str) -> List[dict]:
    if not os.path.exists(path):
        return []
    with open(path) as f:
        return [json.loads(ln) for ln in f if ln.strip()]


def _mcp_call(base_url: str, meta: dict = None, headers: dict = None) -> requests.Response:
    params = {"name": "lookup_tool", "arguments": {}}
    if meta is not None:
        params["_meta"] = meta
    body = {"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": params}
    return requests.post(f"{base_url}/mcp/jsonrpc", json=body,
                         headers=headers or {}, timeout=15)


@pytest.mark.standalone_server
class TestTraceContextIngestion:
    def test_rest_traceparent_header_reaches_the_audit_log(self, trace_server):
        r = requests.get(f"{trace_server['base_url']}/lookup",
                         headers={"traceparent": TRACEPARENT}, timeout=10)
        assert r.status_code == 200

        events = [e for e in _audit(trace_server["audit_path"]) if e.get("trace_id")]
        assert events, "a REST request with a traceparent header must record trace_id"
        assert events[-1]["trace_id"] == TRACE_ID
        assert events[-1]["span_id"] == SPAN_ID

    def test_mcp_meta_traceparent_is_honoured(self, trace_server):
        # THE conformance test. SEP-414 puts traceparent in params._meta,
        # unprefixed; flAPI used to discard it.
        r = _mcp_call(trace_server["base_url"], meta={"traceparent": TRACEPARENT})
        assert r.status_code == 200, r.text

        events = [e for e in _audit(trace_server["audit_path"]) if e.get("trace_id")]
        assert events, (
            "an MCP tools/call carrying params._meta.traceparent must be "
            "correlated; discarding it is the SEP-414 conformance defect"
        )
        assert events[-1]["trace_id"] == TRACE_ID
        assert events[-1]["span_id"] == SPAN_ID

    def test_meta_wins_over_a_disagreeing_header(self, trace_server):
        # Over a gateway the HTTP hop may carry the gateway's span while _meta
        # carries the agent's, so _meta must win.
        r = _mcp_call(trace_server["base_url"],
                      meta={"traceparent": TRACEPARENT},
                      headers={"traceparent": OTHER_TRACEPARENT})
        assert r.status_code == 200, r.text

        events = [e for e in _audit(trace_server["audit_path"]) if e.get("trace_id")]
        assert events
        assert events[-1]["trace_id"] == TRACE_ID, (
            "params._meta must take precedence over the HTTP header"
        )

    def test_a_malformed_traceparent_does_not_fail_the_request(self, trace_server):
        # A broken header from an upstream is not the caller's fault. W3C says
        # reject it and start a new root trace - never abort the request.
        for bad in ("garbage", "00-tooshort-00f067aa0ba902b7-01",
                    "ff-" + TRACE_ID + "-" + SPAN_ID + "-01",
                    "00-" + "0" * 32 + "-" + SPAN_ID + "-01"):
            r = requests.get(f"{trace_server['base_url']}/lookup",
                             headers={"traceparent": bad}, timeout=10)
            assert r.status_code == 200, f"malformed traceparent {bad!r} broke the request"

        for ev in _audit(trace_server["audit_path"]):
            assert not ev.get("trace_id"), (
                f"a malformed traceparent must be rejected, not recorded: {ev.get('trace_id')!r}"
            )

    def test_no_traceparent_means_no_trace_ids(self, trace_server):
        r = requests.get(f"{trace_server['base_url']}/lookup", timeout=10)
        assert r.status_code == 200
        for ev in _audit(trace_server["audit_path"]):
            assert "trace_id" not in ev
