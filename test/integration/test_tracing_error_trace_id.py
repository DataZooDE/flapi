"""Trace ids in error responses (epic issue 16).

The operator-facing payoff of the whole epic: a user hitting an error can quote
one id, and support can go straight to the trace and the audit line instead of
reconstructing the request from two unjoined logs.

X-Request-Id is on every response already. This adds the trace id to the response
where one exists, so the id a user sees is the id in the backend.
"""
import pytest
import requests

from otel_helpers import traced_server

pytestmark = pytest.mark.standalone_server

ENDPOINTS = {
    "items.yaml": (
        "url-path: /items\nmethod: GET\n"
        "template-source: items.sql\nconnection: [inmem]\n"
        "request:\n"
        "  - field-name: limit\n    field-in: query\n    required: false\n"
        "    validators:\n      - type: int\n        min: 1\n"
    ),
    "items.sql": "SELECT 1 AS n\n",
    "guarded.yaml": (
        "url-path: /guarded\nmethod: GET\n"
        "template-source: items.sql\nconnection: [inmem]\n"
        "auth:\n  enabled: true\n  type: basic\n  users:\n"
        "    - username: alice\n      password: correct-horse\n      roles: [reader]\n"
    ),
}


@pytest.fixture
def server():
    yield from traced_server(endpoints=ENDPOINTS)


class TestErrorTraceIds:
    def test_every_response_carries_a_request_id(self, server):
        for path, expected in (("/items", 200), ("/guarded", 401), ("/nope", 404)):
            r = requests.get(f"{server.base_url}{path}", timeout=10)
            assert r.status_code == expected
            assert r.headers.get("X-Request-Id", "").startswith("req-"), (
                f"{path} returned no usable request id"
            )

    def test_a_traced_response_carries_the_trace_id(self, server):
        r = requests.get(f"{server.base_url}/items", timeout=10)
        assert r.status_code == 200

        trace_id = r.headers.get("X-Trace-Id")
        assert trace_id, "a traced response must expose its trace id"
        assert len(trace_id) == 32, f"expected a 32-char W3C trace id, got {trace_id!r}"

    def test_the_header_trace_id_matches_the_exported_span(self, server):
        # The id a user quotes must be the id in the backend, or it is worse than
        # useless - it sends support looking for something that does not exist.
        r = requests.get(f"{server.base_url}/items", timeout=10)
        header_trace = r.headers["X-Trace-Id"]

        span = server.wait_for_server_span()
        assert span.trace_id == header_trace

    def test_an_error_response_carries_the_trace_id(self, server):
        r = requests.get(f"{server.base_url}/guarded", timeout=10)
        assert r.status_code == 401
        assert r.headers.get("X-Trace-Id"), (
            "an error is exactly when a user needs the id to quote"
        )

    def test_an_inbound_trace_is_echoed_back(self, server):
        tp = "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01"
        r = requests.get(f"{server.base_url}/items",
                         headers={"traceparent": tp}, timeout=10)
        assert r.headers.get("X-Trace-Id") == "4bf92f3577b34da6a3ce929d0e0e4736", (
            "the echoed id must be the CALLER's trace, so both sides can join on it"
        )
