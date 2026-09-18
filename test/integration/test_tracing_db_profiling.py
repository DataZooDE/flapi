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

# Decimal-only digits that cannot appear inside a hex trace/span id.
SENTINEL = 99899

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
    # A FILTERED endpoint. The sentinel test needs a WHERE clause: without a
    # predicate there is no EXTRA_INFO for DuckDB to render, so a leak test over
    # an unfiltered query would pass even if EXTRA_INFO were exported verbatim.
    "filtered.yaml": (
        "url-path: /filtered\nmethod: GET\n"
        "template-source: filtered.sql\nconnection: [inmem]\n"
        "request:\n"
        "  - field-name: id\n    field-in: query\n    required: true\n"
        "    validators:\n      - type: int\n        min: 1\n"
    ),
    "filtered.sql": "SELECT r.range AS v FROM range(200000) r WHERE r.range = {{ params.id }}\n",
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

    def test_each_tier_exports_exactly_its_documented_keys(self):
        # The previous version of this test asserted every value "is not None",
        # which OTLP guarantees anyway - it could never fail. The tier table in
        # the docs is only true if this holds.
        detailed_only = {"flapi.db.cpu_time_ms", "flapi.db.rows_scanned"}
        summary_keys = {"flapi.db.latency_ms", "flapi.db.blocked_thread_time_ms",
                        "flapi.db.result_set_bytes", "flapi.db.bytes_read"}

        for server in _server("summary"):
            requests.get(f"{server.base_url}/typed?id=1", timeout=10)
            parent = server.wait_for_server_span(route="/typed")
            got = {k for k in _db_spans(server, parent)[0].attributes
                   if k.startswith("flapi.db.")}
            assert got == summary_keys, f"summary exported {got}"

        for server in _server("detailed"):
            requests.get(f"{server.base_url}/typed?id=1", timeout=10)
            parent = server.wait_for_server_span(route="/typed")
            attrs = _db_spans(server, parent)[0].attributes
            got = {k for k in attrs if k.startswith("flapi.db.")}
            assert got == summary_keys | detailed_only, f"detailed exported {got}"
            for k in got:
                assert isinstance(attrs[k], (int, float)), f"{k} is not numeric"

    def test_profiling_never_exports_a_filter_predicate_or_its_value(self):
        # Driven through a FILTERED query on purpose: EXTRA_INFO is, per
        # operator, expression->GetName() - the rendered predicate. On the
        # prepared path that can carry the bound value. A query with no WHERE
        # clause has no predicate to leak, so testing with one would prove
        # nothing.
        for server in _server("detailed"):
            r = requests.get(f"{server.base_url}/filtered?id={SENTINEL}", timeout=15)
            assert r.status_code == 200, r.text
            server.wait_for_server_span(route="/filtered")

            assert str(SENTINEL) not in server.raw_traces(), (
                "a bound parameter value reached the trace via profiling metrics"
            )
            # And not via the log either: DuckDB will happily print its query
            # tree to stderr when profiling is on, which bypasses the capture
            # tiers entirely.
            assert str(SENTINEL) not in server.log(), (
                "a bound parameter value reached the server log"
            )

    def test_profiling_does_not_dump_a_query_tree_to_the_log(self):
        # custom_profiling_settings enables the profiler but leaves
        # emit_profiler_output at its default of true, so DuckDB renders and
        # prints a ~1 KB tree per query - a synchronous stderr write on the
        # request thread, and log volume proportional to QPS.
        for server in _server("summary"):
            for _ in range(5):
                requests.get(f"{server.base_url}/typed?id=1", timeout=10)
            server.wait_for_server_span(route="/typed")
            assert "Query Profiling Information" not in server.log(), (
                "DuckDB is printing its query profile to stderr on every "
                "profiled query"
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

            parent = server.wait_for_server_span(name="tools/call typed_tool")
            db = _db_spans(server, parent)
            assert db, "an MCP tools/call produced no DB span"
            assert "flapi.db.latency_ms" in db[0].attributes
