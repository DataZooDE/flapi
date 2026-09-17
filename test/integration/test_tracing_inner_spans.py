"""Inner spans: render / bind / query / serialize (epic issue 11).

This is the part no proxy or service mesh can provide. A gateway can time an HTTP
request; only flAPI can say how much of it was template rendering, how much was
parameter binding, how much was DuckDB, and how much was serialization - and
whether the endpoint was cache-backed.

It is also the part that needed NO signature changes anywhere in the pipeline.
OpenTelemetry keeps a thread-local active-span stack and SpanScope holds the
corresponding Scope, so a child started deep inside DatabaseManager attaches to
whatever the middleware put on top. Threading a context parameter through
DatabaseManager, SQLTemplateProcessor and QueryExecutor would have touched every
caller and every existing test for no behavioural gain.
"""
import pytest
import requests

from otel_helpers import children_of, find_span, traced_server

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
    # /items has a typed validator but its SQL never references the param, so the
    # rewriter produces an empty binding plan and the request falls back to the
    # UNPREPARED path. This second endpoint references it, which is what actually
    # routes a request through executeWithBindings -> executePrepared.
    "typed.yaml": (
        "url-path: /typed\nmethod: GET\n"
        "template-source: typed.sql\nconnection: [inmem]\n"
        "mcp-tool:\n  name: typed_lookup\n  description: Look up by id\n"
        "request:\n"
        "  - field-name: id\n    field-in: query\n    required: true\n"
        "    description: The id to look up\n"
        "    validators:\n      - type: int\n        min: 1\n"
    ),
    "typed.sql": "SELECT {{ params.id }} AS id\n",
}


@pytest.fixture
def server():
    yield from traced_server(endpoints=ENDPOINTS)


def _descendants(spans, parent):
    """Every span transitively under `parent`, so an assertion cannot be
    satisfied by an unrelated background query elsewhere in the file."""
    out, frontier = [], {parent.span_id}
    while frontier:
        kids = [s for s in spans if s.parent_span_id in frontier]
        out.extend(kids)
        frontier = {s.span_id for s in kids}
    return out


class TestInnerSpans:
    def test_the_server_span_has_pipeline_children(self, server):
        r = requests.get(f"{server.base_url}/items", timeout=10)
        assert r.status_code == 200

        parent = server.wait_for_server_span()
        spans = server.spans()
        names = {c.name for c in children_of(spans, parent)}

        assert "flapi.render_template" in names, (
            f"template rendering must be attributable; children were {names}"
        )
        assert any(n.startswith("flapi.") or n.upper().startswith("SELECT") for n in names), \
            f"expected flAPI pipeline spans, got {names}"

    def test_children_share_the_trace_and_parent_correctly(self, server):
        requests.get(f"{server.base_url}/items", timeout=10)

        parent = server.wait_for_server_span()
        spans = server.spans()
        kids = children_of(spans, parent)

        assert kids, "the server span must have children"
        for child in kids:
            assert child.trace_id == parent.trace_id
            assert child.parent_span_id == parent.span_id

    def test_the_render_span_reports_a_config_relative_template_path(self, server):
        requests.get(f"{server.base_url}/items", timeout=10)

        span = find_span(server.spans(), name="flapi.render_template")
        assert span is not None
        path = span.attributes.get("flapi.template.path")
        assert path is not None
        # Absolute paths are excluded at every tier: they disclose the server's
        # filesystem layout and are of no use to a trace consumer.
        assert not str(path).startswith("/"), f"template path must be config-relative, got {path!r}"

    def test_the_database_span_uses_db_semconv(self, server):
        requests.get(f"{server.base_url}/items", timeout=10)

        spans = server.spans()
        db_spans = [s for s in spans if s.attributes.get("db.system.name") == "duckdb"]
        assert db_spans, "the DuckDB execution boundary must be a CLIENT span"

        span = db_spans[0]
        assert span.kind == 3, "a database call is a CLIENT span"
        assert "db.operation.name" in span.attributes

    def test_a_typed_endpoint_also_produces_a_database_span(self, server):
        # The regression this exists for: startDbSpan was called only from
        # QueryExecutor::execute, the UNPREPARED path. Any endpoint whose
        # template references a typed request field is rewritten onto
        # executeWithBindings -> executePrepared, which emitted no span at all -
        # so most real endpoints showed template rendering and a server span, but
        # no database child. The time was inside the parent's duration and simply
        # unattributed, which is the one thing an inner span exists to prevent.
        r = requests.get(f"{server.base_url}/typed?id=7", timeout=10)
        assert r.status_code == 200, r.text

        parent = server.wait_for_server_span(route="/typed")
        spans = server.spans()
        # Scoped to THIS request's tree on purpose. Background cache warmup also
        # runs DuckDB queries through the unprepared path, so a search over every
        # span in the file would pass without the request having produced one.
        db_spans = [s for s in _descendants(spans, parent)
                    if s.attributes.get("db.system.name") == "duckdb"]
        assert db_spans, (
            "an endpoint on the PREPARED path produced no DuckDB span; "
            "the database boundary is invisible for every typed endpoint"
        )
        span = db_spans[0]
        assert span.kind == 3, "a database call is a CLIENT span"
        assert span.attributes.get("db.operation.name") == "SELECT"

    def test_both_query_paths_agree_on_the_operation_name(self, server):
        # Two execution paths must not drift into reporting the same statement
        # differently - that is how a dashboard ends up double-counting.
        requests.get(f"{server.base_url}/items", timeout=10)
        unprepared = server.wait_for_server_span(route="/items")
        requests.get(f"{server.base_url}/typed?id=7", timeout=10)
        prepared = server.wait_for_server_span(route="/typed")

        spans = server.spans()

        def verb_under(parent):
            return {s.attributes.get("db.operation.name")
                    for s in _descendants(spans, parent)
                    if s.attributes.get("db.system.name") == "duckdb"}

        assert verb_under(unprepared) == {"SELECT"}
        assert verb_under(prepared) == {"SELECT"}, (
            "the two execution paths report the same statement differently, "
            "which is how a dashboard ends up double-counting"
        )

    def test_the_cache_attribute_claims_only_what_flapi_knows(self, server):
        # TELEMETRY.md already documents that flAPI has no per-request cache
        # hit/miss signal. The attribute is therefore named "backed", and a span
        # must never imply a signal flAPI does not have.
        requests.get(f"{server.base_url}/items", timeout=10)
        blob = server.raw_traces()
        assert "flapi.cache.hit" not in blob, (
            "flAPI cannot report a per-request cache hit; the attribute is "
            "flapi.cache.backed and must stay honest"
        )

    def test_inner_spans_carry_no_values(self, server):
        # The no-leak invariant applies to children too, and the render and bind
        # spans are the most tempting places to leak a parameter value.
        sentinel = "S3NT1NEL_INNER"
        requests.get(f"{server.base_url}/items?limit=5&note={sentinel}", timeout=10)
        assert sentinel not in server.raw_traces()
