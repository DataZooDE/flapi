"""A rate-limited request must not run.

The rate limiter set a 429 and then let the request continue. Crow's middleware
chain short-circuits only on `res.is_completed()` (crow/middleware.h:151), and
only `res.end()` sets that flag - so auth still ran, the SQL still executed, and
the result set was appended to the 429 body:

    request 3: 429
    Rate limit exceeded. Try again later.{"data":[{"marker":"leaked-row"}]}

Which means the limiter offered no protection against load - the expensive work
happened anyway - and handed back the very data it had just refused.

These tests drive the real binary over real HTTP, because the defect lives in
the interaction between flAPI's middleware and Crow's chain. A unit test of the
middleware in isolation would have passed against the broken code.
"""
import os
import subprocess
import tempfile
import time

import pytest
import requests

from otel_helpers import flapi_binary, free_port

pytestmark = pytest.mark.standalone_server

MARKER = "rate-limit-marker-row"


class _Server:
    def __init__(self, max_requests: int):
        self.tmp = tempfile.mkdtemp(prefix="flapi_ratelimit_")
        self.port = free_port()
        self.base_url = f"http://127.0.0.1:{self.port}"
        self.log_path = os.path.join(self.tmp, "server.log")
        sqls = os.path.join(self.tmp, "sqls")
        os.makedirs(sqls, exist_ok=True)

        with open(os.path.join(sqls, "q.yaml"), "w") as f:
            f.write(
                "url-path: /q\nmethod: GET\n"
                "template-source: q.sql\nconnection: [inmem]\n"
                "rate-limit:\n  enabled: true\n"
                f"  max: {max_requests}\n  interval: 60\n")
        with open(os.path.join(sqls, "q.sql"), "w") as f:
            f.write(f"SELECT 42 AS answer, '{MARKER}' AS marker\n")

        with open(os.path.join(self.tmp, "flapi.yaml"), "w") as f:
            f.write(
                "project-name: ratelimit-test\n"
                "project-description: the limiter must stop the request\n"
                f"http-port: {self.port}\n"
                "template:\n  path: ./sqls\n"
                "connections:\n  inmem:\n    properties:\n      database: ':memory:'\n")
        self.proc = None

    def start(self):
        self.proc = subprocess.Popen(
            [flapi_binary(), "-c", os.path.join(self.tmp, "flapi.yaml"),
             "-p", str(self.port), "--log-level", "warning"],
            stdout=open(self.log_path, "w"), stderr=subprocess.STDOUT, cwd=self.tmp,
            env={**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"},
            preexec_fn=os.setsid)
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


class TestRateLimitEnforcement:
    def test_a_rejected_request_returns_no_data(self):
        # The contract, and the exact shape of the bug: the 429 body used to
        # carry the limit message with the full result set appended.
        with _Server(max_requests=2) as s:
            for _ in range(2):
                assert requests.get(f"{s.base_url}/q", timeout=10).status_code == 200

            r = requests.get(f"{s.base_url}/q", timeout=10)
            assert r.status_code == 429, r.text
            assert MARKER not in r.text, (
                f"the rejected request still executed its query and returned "
                f"the rows: {r.text[:300]!r}"
            )
            assert '"data"' not in r.text
            assert r.text.strip() == "Rate limit exceeded. Try again later."

    def test_the_limit_holds_for_subsequent_requests(self):
        # A limiter that lets the next one through is not a limiter.
        with _Server(max_requests=1) as s:
            assert requests.get(f"{s.base_url}/q", timeout=10).status_code == 200
            for _ in range(5):
                r = requests.get(f"{s.base_url}/q", timeout=10)
                assert r.status_code == 429
                assert MARKER not in r.text

    def test_requests_within_the_limit_are_untouched(self):
        # The fix must not break the allowed path - end() is called only on the
        # rejection branch.
        with _Server(max_requests=5) as s:
            for _ in range(5):
                r = requests.get(f"{s.base_url}/q", timeout=10)
                assert r.status_code == 200
                assert MARKER in r.text
                assert r.json()["data"][0]["answer"] == 42

    def test_the_rejection_still_carries_its_headers(self):
        # RFC 6585: the 429 must still tell the client when to retry. end()
        # must not cost the headers set before it.
        with _Server(max_requests=1) as s:
            requests.get(f"{s.base_url}/q", timeout=10)
            r = requests.get(f"{s.base_url}/q", timeout=10)
            assert r.status_code == 429
            assert r.headers.get("Retry-After") is not None
            assert int(r.headers["Retry-After"]) >= 1
            assert r.headers.get("X-RateLimit-Limit") == "1"
