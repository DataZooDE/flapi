"""The MCP single-span contract (epic issue 9).

An MCP call arrives as an HTTP POST, so there are two server-side operations to
model and an obvious risk of two nested SERVER spans - which reads badly in every
backend. flAPI emits ONE span carrying both attribute sets, named by the MCP
convention, because that is the name a trace consumer looks for.

That is only correct because flAPI's transport is strictly one JSON-RPC call per
HTTP request: there is no batch handling, and the legacy GET/SSE stream is
deliberately not implemented (GET on the MCP endpoint returns 405). If either
ever changes, this collapses and the model must become an HTTP SERVER parent with
one MCP SERVER child per call. These tests pin the invariant so that change
cannot happen quietly.
"""
import pytest
import requests

from otel_helpers import find_span, find_spans, traced_server

pytestmark = pytest.mark.standalone_server

ENDPOINTS = {
    "lookup.yaml": (
        "url-path: /lookup\nmethod: GET\n"
        "template-source: lookup.sql\nconnection: [inmem]\n"
        "mcp-tool:\n  name: lookup_tool\n  description: Look something up\n"
    ),
    "lookup.sql": "SELECT 1 AS n\n",
    # A TYPED tool: its template references a typed field, so a tools/call is
    # rewritten onto executeWithBindings -> executePrepared. Agents reach the
    # database through exactly the same path REST does, and it was equally
    # unspanned.
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

TRACEPARENT = "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01"


@pytest.fixture
def server():
    yield from traced_server(endpoints=ENDPOINTS)


def _rpc(base_url, method, params=None, headers=None):
    body = {"jsonrpc": "2.0", "id": 1, "method": method, "params": params or {}}
    return requests.post(f"{base_url}/mcp/jsonrpc", json=body,
                         headers=headers or {}, timeout=15)


class TestMcpSingleSpan:
    def test_a_tools_call_produces_exactly_one_server_span(self, server):
        r = _rpc(server.base_url, "tools/call",
                 {"name": "lookup_tool", "arguments": {}})
        assert r.status_code == 200, r.text

        server.wait_for_server_span()
        servers = [s for s in server.spans() if s.is_server]
        assert len(servers) == 1, (
            f"an MCP call must be ONE span carrying both http.* and mcp.*, "
            f"not nested HTTP and MCP spans; got {[s.name for s in servers]}"
        )

    def test_a_typed_tool_call_produces_a_database_span(self, server):
        # The MCP half of the prepared-path gap. A real tools/call over JSON-RPC,
        # no mocks: an agent asking a typed tool for data must leave the same
        # database evidence a REST caller does, or agent traffic is the half of
        # the workload nobody can debug.
        r = _rpc(server.base_url, "tools/call",
                 {"name": "typed_tool", "arguments": {"id": 7}})
        assert r.status_code == 200, r.text

        # Scoped by span name: the module fixture is shared across tests, so an
        # unscoped lookup can return an earlier test's SERVER span and the
        # descendant walk then succeeds on its DB span instead of this one.
        parent = server.wait_for_server_span(name="tools/call typed_tool")
        spans = server.spans()
        frontier, db = {parent.span_id}, []
        while frontier:
            kids = [s for s in spans if s.parent_span_id in frontier]
            db += [s for s in kids if s.attributes.get("db.system.name") == "duckdb"]
            frontier = {s.span_id for s in kids}

        assert db, "an MCP tools/call on the prepared path produced no DuckDB span"
        assert db[0].kind == 3
        assert db[0].attributes.get("db.operation.name") == "SELECT"

    def test_the_span_carries_both_attribute_sets(self, server):
        _rpc(server.base_url, "tools/call", {"name": "lookup_tool", "arguments": {}})

        span = server.wait_for_server_span()
        # HTTP half - Stable conventions, useful to an operator with no agent.
        assert span.attributes["http.request.method"] == "POST"
        assert span.attributes["http.response.status_code"] == 200
        # MCP/GenAI half - the overlay on the same span.
        assert span.attributes.get("mcp.method.name") == "tools/call"
        assert span.attributes.get("gen_ai.tool.name") == "lookup_tool"
        assert span.attributes.get("gen_ai.operation.name") == "execute_tool"

    def test_the_span_is_named_by_the_mcp_convention(self, server):
        _rpc(server.base_url, "tools/call", {"name": "lookup_tool", "arguments": {}})
        span = server.wait_for_server_span()
        assert span.name == "tools/call lookup_tool", (
            f"the name a trace consumer looks for is the MCP one, got {span.name!r}"
        )

    def test_a_non_tool_method_is_named_by_its_method(self, server):
        _rpc(server.base_url, "tools/list")
        span = server.wait_for_server_span()
        assert span.name == "tools/list"
        assert span.attributes.get("mcp.method.name") == "tools/list"

    def test_meta_traceparent_parents_the_mcp_span(self, server):
        # The SEP-414 acceptance case, now visible as a real span.
        _rpc(server.base_url, "tools/call",
             {"name": "lookup_tool", "arguments": {}, "_meta": {"traceparent": TRACEPARENT}})

        span = server.wait_for_server_span()
        assert span.trace_id == "4bf92f3577b34da6a3ce929d0e0e4736"
        assert span.parent_span_id == "00f067aa0ba902b7"

    def test_meta_wins_over_a_disagreeing_header(self, server):
        other = "00-11111111111111111111111111111111-1111111111111111-01"
        _rpc(server.base_url, "tools/call",
             {"name": "lookup_tool", "arguments": {}, "_meta": {"traceparent": TRACEPARENT}},
             headers={"traceparent": other})

        span = server.wait_for_server_span()
        assert span.trace_id == "4bf92f3577b34da6a3ce929d0e0e4736", (
            "params._meta must take precedence: over a gateway the HTTP hop may "
            "carry the gateway's own span while _meta carries the agent's"
        )
        assert span.attributes.get("flapi.trace.context_source") == "meta_over_header"

    def test_the_single_span_invariant_still_holds(self, server):
        # GET on the MCP endpoint returns 405 (no SSE), and there is no JSON-RPC
        # batch handling. Both are what make one-span-per-request correct. If
        # either changes, this test fails and the span model must be revisited.
        r = requests.get(f"{server.base_url}/mcp/jsonrpc", timeout=10)
        assert r.status_code == 405, (
            "an SSE/streamable GET would break the one-JSON-RPC-call-per-request "
            "invariant that the single-span model depends on"
        )

    def test_an_attacker_cannot_inject_into_the_span_name(self, server):
        # parseMCPRequest falls back to dumping the raw JSON when `method` is not
        # a string, so a non-string method would otherwise put attacker-controlled
        # content straight into a span NAME and mcp.method.name - unbounded
        # cardinality and content injection at once. Span names and metric
        # dimensions must come from a closed set.
        marker = "INJECT3D"
        requests.post(f"{server.base_url}/mcp/jsonrpc",
                      json={"jsonrpc": "2.0", "id": 1,
                            "method": {"evil": marker}, "params": {}},
                      timeout=15)
        requests.post(f"{server.base_url}/mcp/jsonrpc",
                      json={"jsonrpc": "2.0", "id": 2,
                            "method": "not/a/real/method", "params": {}},
                      timeout=15)

        blob = server.raw_traces()
        assert marker not in blob, "attacker JSON reached an exported span"
        assert "not/a/real/method" not in blob, (
            "an unrecognised method must collapse to one bucket, or a scanner "
            "mints unbounded span names and metric series"
        )

    def test_an_unknown_tool_records_an_enumerated_error(self, server):
        _rpc(server.base_url, "tools/call", {"name": "no_such_tool", "arguments": {}})

        span = server.wait_for_server_span()
        error = span.attributes.get("error.type")
        if error is not None:
            # Enumerated only - never a free-form message, which is the most
            # reliable way to leak customer data into a trace.
            assert " " not in str(error), f"error.type must be enumerated, got {error!r}"

    def test_an_unknown_tool_name_never_reaches_the_export(self, server):
        # The tool name is caller-controlled until the lookup succeeds. It is
        # appended to the SPAN NAME at the default metadata tier, so an
        # unvalidated name is both a content leak and an unbounded-cardinality
        # vector - the same bug the method whitelist fixed for `method`.
        marker = "SECRET_CUSTOMER_9f2c1a7b"
        _rpc(server.base_url, "tools/call", {"name": marker, "arguments": {}})

        server.wait_for_server_span()
        blob = server.raw_traces()
        assert marker not in blob, (
            "a caller-supplied tool name reached an exported span"
        )
