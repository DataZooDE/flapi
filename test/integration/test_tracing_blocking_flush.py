"""Blocking span export for request-billed, scale-to-zero platforms.

On Cloud Run with "CPU allocated only during request processing", the instance
is throttled the moment the response is sent - so BatchSpanProcessor's export
thread may never be scheduled and spans are lost. `flush.mode: on_response` is
refused for `otlp_http` by default, because a synchronous collector round-trip
on a Crow worker means a hanging collector depletes the pool.

This adds an explicit opt-in for that topology: `flush.blocking_timeout_ms`
caps how much latency the operator is willing to trade for telemetry.

The contract under test is exact, and it is the whole point:

    by the time the HTTP response returns, the spans are at the collector.

Note what these tests deliberately do NOT do: wait. A `wait_for_spans` loop
would pass under ordinary batch export too, and would prove nothing at all.

Every test drives the real binary over real HTTP or JSON-RPC against a real
OTLP/HTTP collector. Nothing here is mocked.
"""
import os
import subprocess
import tempfile
import time

import pytest
import requests

from otel_helpers import flapi_binary, free_port, traced_server
from otlp_collector import OtlpCollector, collector_tracing_block

pytestmark = pytest.mark.standalone_server

ENDPOINTS = {
    "typed.yaml": (
        "url-path: /typed\nmethod: GET\n"
        "template-source: typed.sql\nconnection: [inmem]\n"
        "mcp-tool:\n  name: typed_tool\n  description: Look up by id\n"
        "request:\n"
        "  - field-name: id\n    field-in: query\n    required: true\n"
        "    description: The id to look up\n"
        "    validators:\n      - type: int\n        min: 1\n"
    ),
    "typed.sql": "SELECT {{ params.id }} AS id\n",
}


class TestBlockingFlush:
    def test_spans_are_at_the_collector_when_the_response_returns(self):
        # The contract. Under batch export this fails: flush.timeout_ms is 30s
        # in the fixture, so nothing can have been exported yet by ordinary means.
        with OtlpCollector() as collector:
            block = collector_tracing_block(collector.endpoint, blocking_timeout_ms=500)
            for server in traced_server(endpoints=ENDPOINTS, tracing_block=block):
                r = requests.get(f"{server.base_url}/typed?id=1", timeout=15)
                assert r.status_code == 200, r.text
                trace_id = r.headers.get("X-Trace-Id")
                assert trace_id, "a traced response must carry X-Trace-Id"

                # NO WAITING. Read once, immediately.
                got = collector.spans_for_trace(trace_id)
                assert got, (
                    "the response returned before its spans reached the "
                    "collector - on a throttled instance those spans are lost"
                )
                assert any(s.is_server for s in got)
                assert collector.bad_paths() == [], (
                    f"spans were posted to the wrong path: {collector.bad_paths()}"
                )

    def test_a_request_exports_in_one_round_trip(self):
        # A request produces ~4 spans (server + render + two duckdb, the second
        # being the pagination count query). SimpleSpanProcessor would make that
        # four HTTP round trips; BatchSpanProcessor plus one force-flush makes
        # it one. On a per-request latency budget that difference is the design.
        #
        # Measured as a DELTA: the first request also sweeps out whatever
        # startup work left buffered, so an absolute count would be testing the
        # fixture rather than the property.
        with OtlpCollector() as collector:
            block = collector_tracing_block(collector.endpoint, blocking_timeout_ms=500)
            for server in traced_server(endpoints=ENDPOINTS, tracing_block=block):
                requests.get(f"{server.base_url}/typed?id=1", timeout=15)   # drain startup
                before_exports = collector.export_count()
                before_spans = len(collector.spans())

                requests.get(f"{server.base_url}/typed?id=2", timeout=15)
                new_exports = collector.export_count() - before_exports
                new_spans = len(collector.spans()) - before_spans

                assert new_spans >= 3, f"expected a span tree, got {new_spans}"
                assert new_exports == 1, (
                    f"{new_spans} spans left in {new_exports} round trips; one "
                    f"force-flush per request should batch them into a single export"
                )

    def test_an_mcp_tool_call_flushes_the_same_way(self):
        # Agent traffic is the workload most likely to be running on a
        # request-billed instance, so it gets the same guarantee.
        with OtlpCollector() as collector:
            block = collector_tracing_block(collector.endpoint, blocking_timeout_ms=500)
            for server in traced_server(endpoints=ENDPOINTS, tracing_block=block):
                r = requests.post(
                    f"{server.base_url}/mcp/jsonrpc",
                    json={"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                          "params": {"name": "typed_tool", "arguments": {"id": 1}}},
                    timeout=15)
                assert r.status_code == 200, r.text
                trace_id = r.headers.get("X-Trace-Id")
                assert trace_id

                got = collector.spans_for_trace(trace_id)
                assert got, "an MCP tool call's spans had not left when it returned"
                assert any(s.attributes.get("gen_ai.tool.name") == "typed_tool"
                           for s in got)


class TestBlockingFlushSafety:
    def test_a_hanging_collector_bounds_the_request_at_the_budget(self):
        # The reason this is opt-in and capped. A third party flAPI does not
        # control must never be able to hold a worker indefinitely.
        #
        # Bounded on BOTH sides. An upper bound alone passed with the feature
        # deleted - of course a request is fast when it never flushes. The lower
        # bound is what proves the flush actually happened and actually waited.
        budget_ms = 300
        with OtlpCollector(mode="hang") as collector:
            block = collector_tracing_block(collector.endpoint,
                                            blocking_timeout_ms=budget_ms)
            for server in traced_server(endpoints=ENDPOINTS, tracing_block=block):
                started = time.monotonic()
                r = requests.get(f"{server.base_url}/typed?id=1", timeout=15)
                elapsed_ms = (time.monotonic() - started) * 1000

                assert r.status_code == 200, "instrumentation must never fail a request"
                assert collector.saw_a_hanging_request(), (
                    "the collector was never contacted - nothing was flushed, so "
                    "this test would pass with the feature removed"
                )
                assert elapsed_ms >= budget_ms * 0.8, (
                    f"request returned in {elapsed_ms:.0f}ms against a HANGING "
                    f"collector with a {budget_ms}ms budget - it cannot have waited"
                )
                assert elapsed_ms < budget_ms * 3, (
                    f"request took {elapsed_ms:.0f}ms with a {budget_ms}ms budget "
                    f"- the timeout is not bounding it"
                )

    def test_a_refusing_collector_is_invisible_to_the_caller(self):
        # As above: assert the collector was actually reached, or this passes
        # with the feature deleted.
        with OtlpCollector(mode="refuse") as collector:
            block = collector_tracing_block(collector.endpoint, blocking_timeout_ms=300)
            for server in traced_server(endpoints=ENDPOINTS, tracing_block=block):
                r = requests.get(f"{server.base_url}/typed?id=1", timeout=15)
                assert r.status_code == 200
                assert r.json() is not None
                assert "[OTLP TRACE HTTP Exporter] ERROR" in server.log(), (
                    "the exporter never reported a failure, so nothing was "
                    "flushed - the test would pass without the feature"
                )

    def test_an_excluded_route_does_not_pay_the_budget(self):
        # finish() is reached for excluded routes too - exclusion skips span
        # creation, not the context. An ungated flush made /health/live wait the
        # full budget against a wedged collector, which on a platform with a
        # tight liveness timeout restarts a healthy instance.
        budget_ms = 400
        with OtlpCollector(mode="hang") as collector:
            block = collector_tracing_block(collector.endpoint,
                                            blocking_timeout_ms=budget_ms)
            for server in traced_server(endpoints=ENDPOINTS, tracing_block=block):
                started = time.monotonic()
                r = requests.get(f"{server.base_url}/health/live", timeout=15)
                elapsed_ms = (time.monotonic() - started) * 1000

                assert r.status_code == 200
                assert elapsed_ms < budget_ms * 0.5, (
                    f"an excluded health probe took {elapsed_ms:.0f}ms against a "
                    f"wedged collector - probes must not pay the flush budget"
                )

    def test_without_the_opt_in_it_still_falls_back_to_batch(self):
        # Pins the DEFAULT. on_response + otlp_http without an explicit latency
        # budget must keep refusing, or the safe behaviour drifts away silently.
        with OtlpCollector() as collector:
            block = collector_tracing_block(collector.endpoint, blocking_timeout_ms=None)
            for server in traced_server(endpoints=ENDPOINTS, tracing_block=block):
                r = requests.get(f"{server.base_url}/typed?id=1", timeout=15)
                assert r.status_code == 200

                # flush.timeout_ms is 30s in the fixture, so a batch export
                # cannot have happened yet.
                assert collector.spans_for_trace(r.headers.get("X-Trace-Id", "x")) == [], (
                    "on_response was honoured for otlp_http without an explicit "
                    "blocking_timeout_ms - the latency trade must be opted into"
                )
                assert "falling back to batch export" in server.log()


class TestBlockingFlushConfig:
    def _start_with(self, blocking_value: str, timeout_value: str = "2000",
                    wait: float = 60):
        """Start the real binary with a given blocking_timeout_ms and see what it does."""
        tmp = tempfile.mkdtemp(prefix="flapi_cfgchk_")
        sqls = os.path.join(tmp, "sqls")
        os.makedirs(sqls, exist_ok=True)
        with open(os.path.join(sqls, "open.yaml"), "w") as f:
            f.write("url-path: /open\nmethod: GET\n"
                    "template-source: open.sql\nconnection: [inmem]\n")
        with open(os.path.join(sqls, "open.sql"), "w") as f:
            f.write("SELECT 1 AS n\n")
        port = free_port()
        with open(os.path.join(tmp, "flapi.yaml"), "w") as f:
            f.write(
                "project-name: cfgchk\n"
                "project-description: blocking flush config check\n"
                f"http-port: {port}\n"
                "template:\n  path: ./sqls\n"
                "connections:\n  inmem:\n    properties:\n      database: ':memory:'\n"
                "tracing:\n"
                "  enabled: true\n"
                "  exporter: otlp_http\n"
                "  flush:\n"
                "    mode: on_response\n"
                f"    timeout_ms: {timeout_value}\n"
                f"    blocking_timeout_ms: {blocking_value}\n")
        # A REJECTED config exits immediately; an accepted one runs until the
        # timeout, which is how the boundary cases below assert acceptance.
        try:
            return subprocess.run(
                [flapi_binary(), "-c", os.path.join(tmp, "flapi.yaml"), "-p", str(port),
                 "--log-level", "warning"],
                capture_output=True, text=True, timeout=wait,
                env={**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"})
        except subprocess.TimeoutExpired as exc:
            return exc

    @pytest.mark.parametrize("value", ["0", "-1", "1001", "30000"])
    def test_an_out_of_range_budget_is_refused_at_startup(self, value):
        # This value lands directly in every caller's latency. An operator must
        # not be able to set 30s and discover it in production, so it fails loudly
        # at boot rather than being silently clamped.
        proc = self._start_with(value)
        assert proc.returncode != 0, (
            f"blocking_timeout_ms={value} was accepted; the server started"
        )
        combined = proc.stdout + proc.stderr
        assert "blocking_timeout_ms must be between 1 and 1000" in combined, combined[-800:]

    @pytest.mark.parametrize("value", ["0", "-1", "60001"])
    def test_an_out_of_range_flush_timeout_is_refused(self, value):
        # flush.timeout_ms is now the SIGTERM force-flush budget as well as the
        # batch interval. 0 makes ForceFlush wait indefinitely inside a signal
        # handler and spins the batch worker; a huge value overruns the
        # platform's shutdown grace and earns a SIGKILL - strictly worse than
        # the hardcoded 2s it replaced.
        proc = self._start_with("300", timeout_value=value)
        assert proc.returncode != 0, f"flush.timeout_ms={value} was accepted"
        combined = proc.stdout + proc.stderr
        assert "timeout_ms must be between 1 and 60000" in combined, combined[-600:]

    @pytest.mark.parametrize("value", ["1", "1000"])
    def test_the_accepted_boundaries_start_cleanly(self, value):
        # The other half of a range check. Without this, narrowing the accepted
        # range by mistake would go unnoticed - every rejection test would still
        # pass.
        result = self._start_with(value, wait=8)

        def _text(v):
            if v is None:
                return ""
            return v.decode(errors="replace") if isinstance(v, bytes) else v

        combined = _text(result.stdout) + _text(result.stderr)
        assert "blocking_timeout_ms must be between" not in combined, (
            f"boundary value {value} was rejected: {combined[-400:]}"
        )
