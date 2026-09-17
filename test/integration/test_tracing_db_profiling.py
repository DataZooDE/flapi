"""DuckDB execution profiling as span attributes (Option A).

DuckDB can report what EXPLAIN ANALYZE shows - latency, blocked thread time,
bytes read, rows scanned - through its C API rather than as text. Attaching a
summary of that to the existing DuckDB span answers "where did the query time
go" without the operator-tree span explosion.

Two things make this dangerous rather than merely useful, and both are pinned
here:

1. DuckDB's QUERY_NAME metric is the SQL text, and EXTRA_INFO for a filter is the
   rendered predicate - which on the prepared path carries bound parameter
   values. flAPI requests a fixed allowlist and reads back only those keys. It
   must never iterate the metric map and export what it finds, or it re-leaks
   the moment DuckDB adds a metric.
2. Profiling settings are connection-scoped and flAPI opens a connection per
   query, so this costs a round trip per query. It is off by default and gated
   on the span actually recording.
"""
import pytest
import requests

from otel_helpers import traced_server

pytestmark = pytest.mark.standalone_server

SENTINEL = 40477

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


def _server(db_profiling=None):
    extra = f"  db_profiling: {db_profiling}\n" if db_profiling else ""
    return traced_server(endpoints=ENDPOINTS, tracing_extra=extra)


def _db_spans(server, parent):
    spans = server.spans()
    frontier, out = {parent.span_id}, []
    while frontier:
        kids = [s for s in spans if s.parent_span_id in frontier]
        out += [s for s in kids if s.attributes.get("db.system.name") == "duckdb"]
        frontier = {s.span_id for s in kids}
    return out


class TestDbProfiling:
    def test_it_is_off_by_default(self):
        # Costing a round trip per query is not something an operator should
        # acquire by upgrading.
        for server in _server():
            requests.get(f"{server.base_url}/typed?id=1", timeout=10)
            parent = server.wait_for_server_span(route="/typed")
            db = _db_spans(server, parent)
            assert db, "precondition: the prepared path must produce a DB span"
            leaked = [k for k in db[0].attributes if k.startswith("flapi.db.")]
            assert not leaked, f"profiling attributes present without opt-in: {leaked}"

    def test_summary_reports_query_latency(self):
        for server in _server("summary"):
            r = requests.get(f"{server.base_url}/typed?id=1", timeout=10)
            assert r.status_code == 200, r.text
            parent = server.wait_for_server_span(route="/typed")
            db = _db_spans(server, parent)
            assert db, "no DB span to carry profiling attributes"

            attrs = db[0].attributes
            assert "flapi.db.latency_ms" in attrs, (
                f"summary profiling exported nothing; got "
                f"{[k for k in attrs if k.startswith('flapi.db.')]}"
            )
            assert attrs["flapi.db.latency_ms"] >= 0

    def test_an_absent_metric_is_omitted_not_reported_as_zero(self):
        # A fabricated measurement is worse than a missing one: zero latency
        # reads as "instant" on every dashboard.
        for server in _server("summary"):
            requests.get(f"{server.base_url}/typed?id=1", timeout=10)
            parent = server.wait_for_server_span(route="/typed")
            attrs = _db_spans(server, parent)[0].attributes
            for key, value in attrs.items():
                if key.startswith("flapi.db."):
                    assert value is not None

    def test_profiling_never_exports_the_sql_or_a_parameter_value(self):
        # The whole-file assertion, deliberately: it is stronger than checking
        # named attributes and survives DuckDB adding metrics we did not expect.
        for server in _server("detailed"):
            r = requests.get(f"{server.base_url}/typed?id={SENTINEL}", timeout=10)
            assert r.status_code == 200, r.text
            server.wait_for_server_span(route="/typed")

            blob = server.raw_traces()
            assert str(SENTINEL) not in blob, (
                "a bound parameter value reached the trace via profiling "
                "metrics - EXTRA_INFO carries rendered filter predicates"
            )
            assert "SELECT" not in blob.replace('"SELECT"', ""), (
                "the SQL text reached the trace; QUERY_NAME must never be "
                "requested"
            )

    def test_an_mcp_tool_call_is_profiled_the_same_way(self):
        # Agents reach DuckDB through the same prepared path, over real
        # JSON-RPC. No mocks.
        for server in _server("summary"):
            r = requests.post(
                f"{server.base_url}/mcp/jsonrpc",
                json={"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                      "params": {"name": "typed_tool", "arguments": {"id": 1}}},
                timeout=15)
            assert r.status_code == 200, r.text

            parent = server.wait_for_server_span()
            db = _db_spans(server, parent)
            assert db, "an MCP tools/call produced no DB span"
            assert "flapi.db.latency_ms" in db[0].attributes
