"""A slow request must not block other connections (#120).

Crow runs a handler on the io thread that owns its connection, so a slow
synchronous query held that thread for the whole query and every other
connection assigned to it waited - including GET /health, which is how a
stalled instance is supposed to report itself.

Measured on one ~6.5s query with 40 concurrent probes and a single io thread:

    offload OFF : 40/40 probes blocked,  0/40 reported the stall, worst 5.04s
    offload ON  :  0/40 probes blocked, 40/40 reported the stall, worst 0.005s

These tests use the shipped thread count rather than forcing one io thread, so
they assert the property (other work proceeds) rather than the measurement.
"""
import json
import os
import subprocess
import tempfile
import threading
import time

import pytest
import requests

from otel_helpers import flapi_binary, free_port

pytestmark = pytest.mark.standalone_server


class _Server:
    def __init__(self, offload: bool = True):
        self.offload = offload
        self.tmp = tempfile.mkdtemp(prefix="flapi_offload_")
        self.port = free_port()
        self.base_url = f"http://127.0.0.1:{self.port}"
        self.log_path = os.path.join(self.tmp, "server.log")
        sqls = os.path.join(self.tmp, "sqls")
        os.makedirs(sqls, exist_ok=True)
        with open(os.path.join(sqls, "slow.yaml"), "w") as f:
            f.write("url-path: /slow\nmethod: GET\n"
                    "template-source: slow.sql\nconnection: [inmem]\n")
        with open(os.path.join(sqls, "slow.sql"), "w") as f:
            f.write("SELECT sum(i*i) AS s FROM range(0, 1200000000) t(i)\n")
        with open(os.path.join(sqls, "fast.yaml"), "w") as f:
            f.write("url-path: /fast\nmethod: GET\n"
                    "template-source: fast.sql\nconnection: [inmem]\n")
        with open(os.path.join(sqls, "fast.sql"), "w") as f:
            f.write("SELECT 1 AS n\n")
        with open(os.path.join(self.tmp, "flapi.yaml"), "w") as f:
            f.write(
                "project-name: offload-test\n"
                "project-description: a slow request must not block others\n"
                f"http-port: {self.port}\n"
                "stall-timeout-s: 1\n"
                "template:\n  path: ./sqls\n"
                "connections:\n  inmem:\n    properties:\n      database: ':memory:'\n")
        self.proc = None

    def start(self):
        env = {**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"}
        env["FLAPI_DISABLE_HANDLER_OFFLOAD"] = "0" if self.offload else "1"
        self.proc = subprocess.Popen(
            [flapi_binary(), "-c", os.path.join(self.tmp, "flapi.yaml"),
             "-p", str(self.port), "--log-level", "warning"],
            stdout=open(self.log_path, "w"), stderr=subprocess.STDOUT,
            cwd=self.tmp, env=env, preexec_fn=os.setsid)
        deadline = time.time() + 90
        while time.time() < deadline:
            try:
                if requests.get(f"{self.base_url}/health/live", timeout=2).status_code == 200:
                    return self
            except requests.RequestException:
                pass
            time.sleep(0.2)
        pytest.fail(f"server did not start:\n{open(self.log_path).read()[-3000:]}")

    def stop(self):
        if self.proc:
            import signal
            try:
                os.killpg(os.getpgid(self.proc.pid), signal.SIGKILL)
            except ProcessLookupError:
                pass
            self.proc.wait(timeout=30)

    def __enter__(self):
        return self.start()

    def __exit__(self, *exc):
        self.stop()


def _in_flight_slow(server):
    """Start a slow request and return a thread plus a done-event."""
    done = threading.Event()

    def run():
        try:
            requests.get(f"{server.base_url}/slow", timeout=180)
        finally:
            done.set()

    t = threading.Thread(target=run, daemon=True)
    t.start()
    return t, done


class TestHandlerOffload:
    def test_other_endpoints_answer_while_a_slow_query_runs(self):
        # The contract. Without the offload these queue behind the query on
        # whichever io thread the slow request occupies.
        with _Server() as s:
            worker, done = _in_flight_slow(s)
            try:
                time.sleep(1.5)
                assert not done.is_set(), "the slow query finished too early to test anything"
                for _ in range(20):
                    r = requests.get(f"{s.base_url}/fast", timeout=10)
                    assert r.status_code == 200, r.text[:200]
                    assert r.json()["data"][0]["n"] == 1
            finally:
                done.wait(timeout=180)

    def test_readiness_reports_the_stall_while_it_is_happening(self):
        # The point of #120: the probe has to be able to ANSWER in order to
        # report. Blocked probes cannot say anything.
        with _Server() as s:
            worker, done = _in_flight_slow(s)
            try:
                deadline = time.time() + 30
                saw = None
                while time.time() < deadline and not done.is_set():
                    r = requests.get(f"{s.base_url}/health", timeout=5)
                    if r.status_code == 503 and r.json().get("status") == "stalled":
                        saw = r.json()
                        break
                    time.sleep(0.1)
                assert saw is not None, "readiness never reported the stall"
                assert saw["requests"]["in_flight"] >= 1
            finally:
                done.wait(timeout=180)

    def test_the_slow_request_still_gets_its_answer(self):
        # Offloading must not lose the response.
        with _Server() as s:
            r = requests.get(f"{s.base_url}/slow", timeout=180)
            assert r.status_code == 200, r.text[:200]
            assert r.json()["data"][0]["s"] is not None

    def test_the_offload_can_be_disabled(self):
        # The escape hatch must actually serve requests, so a deployment that
        # hits something unanticipated can fall back.
        with _Server(offload=False) as s:
            r = requests.get(f"{s.base_url}/fast", timeout=20)
            assert r.status_code == 200
            assert r.json()["data"][0]["n"] == 1
