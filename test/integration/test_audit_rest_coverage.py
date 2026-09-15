"""REST audit coverage and request identity (epic issue 1).

docs/CONFIG_REFERENCE.md and examples/flapi.yaml have always claimed the audit
log "covers both REST and MCP traffic". It did not: the only AuditLogger call
site was inside the MCP tool handler, so no REST request was ever audited.

These tests pin the corrected behaviour end to end against the real binary:

  * every REST request produces exactly one audit line
  * the line's request_id matches the X-Request-Id response header
  * a 401 rejected in AuthMiddleware is audited too

That last one is the interesting case. Crow does not call after_handle when a
middleware completes the response inside before_handle
(crow/http_connection.h:183-207), which is exactly what AuthMiddleware's 401
paths do - so a middleware that simply trusts after_handle silently loses every
401. This test fails against such an implementation.
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
            f"project-name: audit-rest-test\n"
            f"project-description: REST audit coverage E2E\n"
            f"http-port: {port}\n"
            f"template:\n"
            f"  path: ./sqls\n"
            f"connections:\n"
            f"  inmem:\n"
            f"    properties:\n"
            f"      database: ':memory:'\n"
            f"audit:\n"
            f"  enabled: true\n"
            f"  sink: file\n"
            f"  path: {audit_path}\n"
            f"  redact:\n"
            f"    - secret\n"
        )

    # An open endpoint, to prove ordinary REST traffic is audited.
    with open(os.path.join(sqls, "open.yaml"), "w") as f:
        f.write(
            "url-path: /open\n"
            "method: GET\n"
            "template-source: open.sql\n"
            "connection: [inmem]\n"
        )
    with open(os.path.join(sqls, "open.sql"), "w") as f:
        f.write("SELECT 1 AS n\n")

    # A basic-auth endpoint, to reach AuthMiddleware's res.end() 401 path.
    with open(os.path.join(sqls, "guarded.yaml"), "w") as f:
        f.write(
            "url-path: /guarded\n"
            "method: GET\n"
            "template-source: open.sql\n"
            "connection: [inmem]\n"
            "auth:\n"
            "  enabled: true\n"
            "  type: basic\n"
            "  users:\n"
            "    - username: alice\n"
            "      password: correct-horse\n"
            "      roles: [reader]\n"
        )

    return os.path.join(dirpath, "flapi.yaml")


@pytest.fixture
def audit_rest_server() -> Iterator[Dict[str, str]]:
    tmp = tempfile.mkdtemp(prefix="flapi_audit_rest_")
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

        yield {"base_url": base_url, "audit_path": audit_path, "log_path": log_path}
    finally:
        try:
            os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
            proc.wait(timeout=10)
        except Exception:
            try:
                os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
            except Exception:
                pass


def _audit_lines(path: str) -> List[dict]:
    if not os.path.exists(path):
        return []
    with open(path) as f:
        return [json.loads(line) for line in f if line.strip()]


@pytest.mark.standalone_server
class TestRestAuditCoverage:
    def test_rest_request_is_audited(self, audit_rest_server):
        r = requests.get(f"{audit_rest_server['base_url']}/open", timeout=10)
        assert r.status_code == 200

        events = [e for e in _audit_lines(audit_rest_server["audit_path"])
                  if e.get("target") == "/open"]
        assert len(events) == 1, f"expected exactly one audit line for /open, got {events}"

        ev = events[0]
        assert ev["method"] == "GET"
        assert ev["status"] == "success"
        assert ev["request_id"].startswith("req-")
        assert ev["latency_ms"] >= 0

    def test_request_id_header_matches_the_audit_line(self, audit_rest_server):
        r = requests.get(f"{audit_rest_server['base_url']}/open", timeout=10)
        assert r.status_code == 200

        header_id = r.headers.get("X-Request-Id")
        assert header_id, "X-Request-Id must be present on every response"
        assert header_id.startswith("req-")

        ids = [e["request_id"] for e in _audit_lines(audit_rest_server["audit_path"])]
        assert header_id in ids, (
            "the response header and the audit line must carry the same request id, "
            "or a support report cannot be joined to the log"
        )

    def test_request_ids_are_unique_per_request(self, audit_rest_server):
        seen = set()
        for _ in range(5):
            r = requests.get(f"{audit_rest_server['base_url']}/open", timeout=10)
            seen.add(r.headers.get("X-Request-Id"))
        assert len(seen) == 5, f"request ids must not repeat, got {seen}"

    def test_inbound_request_id_is_not_trusted(self, audit_rest_server):
        forged = "req-deadbeefdeadbeef"
        r = requests.get(f"{audit_rest_server['base_url']}/open",
                         headers={"X-Request-Id": forged}, timeout=10)
        assert r.status_code == 200
        assert r.headers.get("X-Request-Id") != forged, (
            "an inbound X-Request-Id must never be echoed: a client could forge "
            "collisions and inject into log lines"
        )

    def test_401_rejected_in_middleware_is_audited(self, audit_rest_server):
        # The regression test for the Crow after_handle gap. AuthMiddleware calls
        # res.end() inside before_handle, so Crow never runs after_handle for this
        # response; an implementation that only completes there loses the 401.
        r = requests.get(f"{audit_rest_server['base_url']}/guarded", timeout=10)
        assert r.status_code == 401

        events = [e for e in _audit_lines(audit_rest_server["audit_path"])
                  if e.get("target") == "/guarded" or e.get("status") == "denied"]
        assert events, (
            "a 401 rejected in AuthMiddleware must still be audited; Crow skips "
            "after_handle for responses completed in before_handle"
        )
        assert events[-1]["status"] == "denied"
        assert events[-1]["request_id"].startswith("req-")

    def test_401_produces_exactly_one_audit_line(self, audit_rest_server):
        # Counting matters. Crow runs after_handle for EVERY middleware during the
        # short-circuit unwind (crow/middleware.h:151-155), so a design that also
        # completes the request explicitly on the 401 path emits twice. Asserting
        # "at least one" hides that; assert exactly one.
        before = len(_audit_lines(audit_rest_server["audit_path"]))
        r = requests.get(f"{audit_rest_server['base_url']}/guarded", timeout=10)
        assert r.status_code == 401

        new = _audit_lines(audit_rest_server["audit_path"])[before:]
        assert len(new) == 1, f"a 401 must produce exactly one audit line, got {len(new)}: {new}"

    def test_successful_auth_records_the_principal(self, audit_rest_server):
        r = requests.get(f"{audit_rest_server['base_url']}/guarded",
                         auth=("alice", "correct-horse"), timeout=10)
        assert r.status_code == 200

        events = [e for e in _audit_lines(audit_rest_server["audit_path"])
                  if e.get("target") == "/guarded" and e.get("status") == "success"]
        assert events, "an authenticated REST request must be audited"
        assert events[-1]["principal"] == "alice"

    def test_health_probes_are_not_audited(self, audit_rest_server):
        # In Kubernetes probes are the highest-volume route in the deployment and
        # carry no compliance information. Auditing them would bury the entries an
        # operator actually needs and inflate the log by orders of magnitude.
        for _ in range(10):
            requests.get(f"{audit_rest_server['base_url']}/health/live", timeout=5)
            requests.get(f"{audit_rest_server['base_url']}/health", timeout=5)

        targets = [e.get("target") for e in _audit_lines(audit_rest_server["audit_path"])]
        assert not any(t in ("/health", "/health/live") for t in targets), (
            f"probe routes must not be audited, saw {targets}"
        )

    def test_audit_lines_carry_no_trace_ids_when_tracing_is_off(self, audit_rest_server):
        requests.get(f"{audit_rest_server['base_url']}/open", timeout=10)
        for ev in _audit_lines(audit_rest_server["audit_path"]):
            assert "trace_id" not in ev, (
                "the audit schema must be unchanged for operators who never "
                "enable tracing"
            )
