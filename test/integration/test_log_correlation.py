"""Log correlation end to end (epic issue 2).

flAPI has ~658 CROW_LOG_* call sites and none of them carried any request
identity, so an application log line could not be joined to an audit line or to
the request that produced it. Rather than edit 658 sites, a crow::ILogHandler
reads the ambient RequestContext and stamps the identity on the way out.

These tests run the real binary and assert on its actual stderr:
  * log-level is honoured from the config file (it was silently ignored before)
  * log-format: json produces one parseable object per line
  * lines emitted while serving a request carry that request's id
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


def _write_config(dirpath: str, port: int, log_format: str, log_level: str) -> str:
    sqls = os.path.join(dirpath, "sqls")
    os.makedirs(sqls)
    with open(os.path.join(dirpath, "flapi.yaml"), "w") as f:
        f.write(
            f"project-name: log-correlation-test\n"
            f"project-description: Log correlation E2E\n"
            f"http-port: {port}\n"
            f"log-level: {log_level}\n"
            f"log-format: {log_format}\n"
            f"template:\n"
            f"  path: ./sqls\n"
            f"connections:\n"
            f"  inmem:\n"
            f"    properties:\n"
            f"      database: ':memory:'\n"
        )
    with open(os.path.join(sqls, "open.yaml"), "w") as f:
        f.write("url-path: /open\nmethod: GET\ntemplate-source: open.sql\nconnection: [inmem]\n")
    with open(os.path.join(sqls, "open.sql"), "w") as f:
        f.write("SELECT 1 AS n\n")
    return os.path.join(dirpath, "flapi.yaml")


def _start(log_format: str = "json", log_level: str = "debug") -> Dict[str, str]:
    tmp = tempfile.mkdtemp(prefix="flapi_logcorr_")
    port = _free_port()
    config = _write_config(tmp, port, log_format, log_level)
    log_path = os.path.join(tmp, "server.log")
    with open(log_path, "w") as log:
        proc = subprocess.Popen(
            [_flapi_binary(), "-c", config, "-p", str(port)],
            stdout=log, stderr=subprocess.STDOUT, cwd=tmp,
            env={**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"},
            preexec_fn=os.setsid,
        )
    base_url = f"http://127.0.0.1:{port}"
    deadline = time.time() + 60
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
    return {"base_url": base_url, "log_path": log_path, "proc": proc}


def _stop(server: Dict[str, str]) -> None:
    proc = server["proc"]
    try:
        os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
        proc.wait(timeout=10)
    except Exception:
        try:
            os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
        except Exception:
            pass


@pytest.fixture
def json_log_server() -> Iterator[Dict[str, str]]:
    server = _start(log_format="json", log_level="debug")
    try:
        yield server
    finally:
        _stop(server)


def _log_lines(path: str) -> List[str]:
    with open(path, errors="replace") as f:
        return [ln.rstrip("\n") for ln in f if ln.strip()]


@pytest.mark.standalone_server
class TestLogCorrelation:
    def test_json_format_produces_parseable_lines(self, json_log_server):
        requests.get(f"{json_log_server['base_url']}/open", timeout=10)
        time.sleep(0.3)

        lines = _log_lines(json_log_server["log_path"])
        parsed = []
        for ln in lines:
            if not ln.startswith("{"):
                continue   # the startup banner is not JSON
            parsed.append(json.loads(ln))   # raises if a line is malformed

        assert parsed, "expected JSON log records"
        for rec in parsed:
            assert "level" in rec and "message" in rec and "timestamp" in rec

    def test_lines_emitted_during_a_request_carry_its_id(self, json_log_server):
        r = requests.get(f"{json_log_server['base_url']}/open", timeout=10)
        assert r.status_code == 200
        request_id = r.headers["X-Request-Id"]
        time.sleep(0.3)

        correlated = []
        for ln in _log_lines(json_log_server["log_path"]):
            if not ln.startswith("{"):
                continue
            rec = json.loads(ln)
            if rec.get("request_id") == request_id:
                correlated.append(rec)

        assert correlated, (
            f"no log line carried request_id={request_id}; the whole point is that "
            "an operator grepping the log lands on the right request"
        )

    def test_startup_lines_carry_no_request_id(self, json_log_server):
        # Nothing is in flight during boot, so a request id there would mean the
        # ambient context had leaked.
        for ln in _log_lines(json_log_server["log_path"])[:5]:
            if ln.startswith("{"):
                assert "request_id" not in json.loads(ln)

    def test_log_level_is_honoured_from_config(self):
        # Previously a dead key: examples/flapi-{s3,gcs,azure}.yaml shipped a
        # `server:` block flAPI never parsed, so the level silently did nothing.
        quiet = _start(log_format="json", log_level="warning")
        try:
            requests.get(f"{quiet['base_url']}/open", timeout=10)
            time.sleep(0.3)
            levels = {
                json.loads(ln).get("level")
                for ln in _log_lines(quiet["log_path"]) if ln.startswith("{")
            }
            assert "debug" not in levels, (
                f"log-level: warning must suppress debug lines, saw {levels}"
            )
        finally:
            _stop(quiet)
