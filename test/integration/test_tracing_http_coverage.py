"""The HTTP SERVER span, across the whole route inventory (epic issue 7).

This is the §6.1.1 regression suite, and it exists because the obvious place to
put the span - the request handler - would trace the happy path and almost
nothing else. Crow middlewares reject before the handler runs, and every static
route is registered separately and never reaches handleDynamicRequest at all.

Net effect of a handler-level span: no 401, no 403, no 429, no health check, no
config-service call, no 404 - i.e. the two questions operators actually ask
("are we being rate-limited?", "why is auth rejecting us?") would be
unanswerable. A handler-level implementation fails these tests.
"""
import uuid

import pytest
import requests

from otel_helpers import TracedServer, children_of, find_span, find_spans, traced_server

pytestmark = pytest.mark.standalone_server


ENDPOINTS = {
    "open.yaml": "url-path: /open\nmethod: GET\ntemplate-source: open.sql\nconnection: [inmem]\n",
    "open.sql": "SELECT 1 AS n\n",
    "guarded.yaml": (
        "url-path: /guarded\nmethod: GET\ntemplate-source: open.sql\nconnection: [inmem]\n"
        "auth:\n  enabled: true\n  type: basic\n  users:\n"
        "    - username: alice\n      password: correct-horse\n      roles: [reader]\n"
    ),
}


@pytest.fixture
def server():
    yield from traced_server(endpoints=ENDPOINTS)


class TestSpanCoverage:
    def test_a_successful_request_produces_one_server_span(self, server):
        r = requests.get(f"{server.base_url}/open", timeout=10)
        assert r.status_code == 200

        server.wait_for_server_span()
        servers = [s for s in server.spans() if s.is_server]
        assert len(servers) == 1, f"expected exactly one SERVER span, got {servers}"

        span = servers[0]
        assert span.attributes["http.request.method"] == "GET"
        assert span.attributes["http.response.status_code"] == 200
        assert span.attributes["http.route"] == "/open"
        assert span.name == "GET /open"

    def test_a_401_rejected_in_middleware_is_traced(self, server):
        # THE regression test. AuthMiddleware completes the response inside
        # before_handle, so a handler-level span never exists for this request.
        r = requests.get(f"{server.base_url}/guarded", timeout=10)
        assert r.status_code == 401

        spans = find_spans(server.spans(), attrs={"http.response.status_code": 401})
        assert len(spans) == 1, (
            f"a 401 rejected in AuthMiddleware must produce exactly one span, got {spans}"
        )
        assert spans[0].attributes.get("error.type") is not None

    def test_a_404_is_traced(self, server):
        r = requests.get(f"{server.base_url}/definitely-not-a-route", timeout=10)
        assert r.status_code == 404

        spans = find_spans(server.spans(), attrs={"http.response.status_code": 404})
        assert spans, "an unmatched route must still be traced"

    def test_a_health_probe_is_excluded_by_default(self, server):
        # Probes are the highest-volume route in a Kubernetes deployment and are
        # of near-zero diagnostic value once green.
        for _ in range(5):
            requests.get(f"{server.base_url}/health/live", timeout=5)

        routes = [s.attributes.get("http.route") for s in server.spans()]
        assert "/health/live" not in routes, f"probes must not be traced by default: {routes}"

    def test_successful_auth_is_traced_with_the_auth_kind(self, server):
        r = requests.get(f"{server.base_url}/guarded",
                         auth=("alice", "correct-horse"), timeout=10)
        assert r.status_code == 200

        span = find_span(server.spans(), attrs={"http.route": "/guarded",
                                                "http.response.status_code": 200})
        assert span is not None
        assert span.attributes.get("flapi.auth.kind") == "basic"


class TestCardinality:
    def test_unmatched_paths_collapse_to_one_route(self, server):
        # BR-23. Unmatched paths are attacker-controlled: a scanner hitting a
        # thousand random URLs would otherwise mint a thousand span names and
        # metric series - a cardinality blow-up and a cost-amplification vector
        # against whoever pays for ingest.
        for _ in range(50):
            requests.get(f"{server.base_url}/{uuid.uuid4().hex}", timeout=5)

        routes = {
            s.attributes.get("http.route")
            for s in server.spans()
            if s.attributes.get("http.response.status_code") == 404
        }
        assert routes == {"<unmatched>"}, (
            f"every unmatched path must collapse to one route label, got {routes}"
        )

    def test_a_filled_path_never_appears_in_a_span(self, server):
        # NFR-5 and the no-leak contract: a filled path on a data API is a filter
        # over customer data.
        marker = "S3CRET" + uuid.uuid4().hex
        requests.get(f"{server.base_url}/{marker}", timeout=5)
        requests.get(f"{server.base_url}/open?token={marker}", timeout=5)

        assert marker not in server.raw_traces(), (
            "a filled path or query string must never reach an exported span"
        )


class TestParenting:
    TRACEPARENT = "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01"

    def test_an_inbound_traceparent_parents_the_server_span(self, server):
        # BR-1 acceptance: the whole reason the feature exists.
        r = requests.get(f"{server.base_url}/open",
                         headers={"traceparent": self.TRACEPARENT}, timeout=10)
        assert r.status_code == 200

        span = find_span(server.spans(), attrs={"http.route": "/open"})
        assert span is not None
        assert span.trace_id == "4bf92f3577b34da6a3ce929d0e0e4736"
        assert span.parent_span_id == "00f067aa0ba902b7"

    def test_without_a_traceparent_the_span_is_a_root(self, server):
        requests.get(f"{server.base_url}/open", timeout=10)
        span = find_span(server.spans(), attrs={"http.route": "/open"})
        assert span is not None
        assert span.parent_span_id in ("", "0000000000000000")

    def test_a_malformed_traceparent_starts_a_new_root(self, server):
        r = requests.get(f"{server.base_url}/open",
                         headers={"traceparent": "total-garbage"}, timeout=10)
        assert r.status_code == 200, "a malformed traceparent must not fail the request"

        span = find_span(server.spans(), attrs={"http.route": "/open"})
        assert span is not None
        assert span.parent_span_id in ("", "0000000000000000")


class TestNoLeak:
    def test_metadata_tier_exports_no_values(self, server):
        # The security gate. Whole-file substring search is strictly stronger than
        # inspecting a parsed attribute list: it also covers span names, event
        # names, status messages and resource attributes nobody enumerated.
        sentinels = {
            "param": "S3NT1NEL_PARAM",
            "auth": "S3NT1NEL_TOKEN",
        }
        requests.get(f"{server.base_url}/open?q={sentinels['param']}", timeout=10)
        requests.get(f"{server.base_url}/guarded",
                     headers={"Authorization": f"Bearer {sentinels['auth']}"}, timeout=10)

        blob = server.raw_traces()
        for where, value in sentinels.items():
            assert value not in blob, f"{where} value leaked into an exported span"
