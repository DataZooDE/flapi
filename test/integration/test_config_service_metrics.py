"""GET /api/v1/_config/metrics (epic issue 10, drift bug #3).

This endpoint has been published in flAPI's OpenAPI document since
open_api_doc_generator.cpp:563 with no route behind it - a documented endpoint
that 404s. It is implemented rather than deleted because the tracing counters
need a surface anyway: NFR-4 requires that an unreachable collector be observable
as dropped spans rather than as a trace that quietly has holes in it.
"""
import pytest
import requests

from otel_helpers import traced_server

pytestmark = pytest.mark.standalone_server

TOKEN = "metrics-test-token"


@pytest.fixture
def server():
    # The config service needs its own flag and token, so build the server with
    # the extra CLI arguments rather than the default fixture.
    import os
    import signal
    import subprocess
    import time
    from otel_helpers import TracedServer, flapi_binary

    s = TracedServer()
    with open(s.log_path, "w") as log:
        s.proc = subprocess.Popen(
            [flapi_binary(), "-c", os.path.join(s.tmp, "flapi.yaml"),
             "-p", str(s.port), "--log-level", "warning",
             "--config-service", "--config-service-token", TOKEN],
            stdout=log, stderr=subprocess.STDOUT, cwd=s.tmp,
            env={**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"},
            preexec_fn=os.setsid,
        )
    deadline = time.time() + 60
    while time.time() < deadline:
        if s.proc.poll() is not None:
            pytest.fail(f"server exited:\n{s.log()}")
        try:
            if requests.get(f"{s.base_url}/health/live", timeout=1).status_code == 200:
                break
        except requests.RequestException:
            time.sleep(0.3)
    else:
        pytest.fail("server never became live")
    try:
        yield s
    finally:
        s.stop()


class TestMetricsEndpoint:
    def test_the_route_exists(self, server):
        r = requests.get(f"{server.base_url}/api/v1/_config/metrics",
                         headers={"Authorization": f"Bearer {TOKEN}"}, timeout=10)
        assert r.status_code == 200, (
            "the endpoint is in the OpenAPI document; it must not 404"
        )

    def test_it_is_bearer_gated_like_its_siblings(self, server):
        r = requests.get(f"{server.base_url}/api/v1/_config/metrics", timeout=10)
        assert r.status_code == 401, "metrics report internal state and must be gated"

    def test_it_reports_tracing_counters(self, server):
        requests.get(f"{server.base_url}/open", timeout=10)

        r = requests.get(f"{server.base_url}/api/v1/_config/metrics",
                         headers={"Authorization": f"Bearer {TOKEN}"}, timeout=10)
        body = r.json()

        assert body["tracing"]["enabled"] is True
        assert body["tracing"]["spans_exported"] >= 1, (
            "serving a request must be visible as exported spans"
        )
        assert "spans_dropped" in body["tracing"], (
            "a drop counter is how an unreachable collector becomes observable "
            "rather than silent data loss"
        )

    def test_it_reports_arrow_and_endpoint_counters(self, server):
        r = requests.get(f"{server.base_url}/api/v1/_config/metrics",
                         headers={"Authorization": f"Bearer {TOKEN}"}, timeout=10)
        body = r.json()
        assert "arrow" in body and "total_requests" in body["arrow"]
        assert body["endpoints"]["count"] >= 1
