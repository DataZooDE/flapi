"""total_count without a second query where the page already tells it (#241).

A paginated GET used to run the page query AND `SELECT COUNT(*) FROM (<q>)` on every request.
Now the COUNT runs only when the page cannot tell the total (a full page, or an empty page past
the end); MCP tool calls, which never return a total, never count; and `total-count: false`
endpoints read one row past the page to know `next` and report no total at all.

Every answer is compared against the exact total, and the number of DuckDB queries per request is
read from the trace (one DB span per query).
"""
import pytest
import requests

from otel_helpers import traced_server
from test_tracing_db_profiling import _db_spans

pytestmark = pytest.mark.standalone_server

ROWS = 250
TYPED = ("request:\n  - field-name: min\n    field-in: query\n    required: false\n"
         "    validators:\n      - type: int\n        min: 0\n")
ENDPOINTS = {
    # prepared path (a typed parameter is bound)
    "rows.yaml": "url-path: /rows\nmethod: GET\ntemplate-source: rows.sql\nconnection: [inmem]\n"
                 "mcp-tool:\n  name: rows_tool\n  description: rows\n" + TYPED,
    "rows.sql": f"SELECT range AS id FROM range({ROWS}) WHERE 1=1"
                "{{#params.min}} AND range >= {{ params.min }}{{/params.min}} ORDER BY id\n",
    # plain path (nothing bound)
    "plain.yaml": "url-path: /plain\nmethod: GET\ntemplate-source: plain.sql\nconnection: [inmem]\n",
    "plain.sql": f"SELECT range AS id FROM range({ROWS}) ORDER BY id\n",
    "nototal.yaml": "url-path: /nototal\nmethod: GET\ntemplate-source: plain.sql\nconnection: [inmem]\n"
                    "total-count: false\n",
}


@pytest.fixture(scope="module")
def server():
    yield from traced_server(endpoints=ENDPOINTS)


def queries_for(server, route, before):
    parent = [s for s in server.spans() if s.is_server and s.attributes.get("http.route") == route
              and s.span_id not in before][-1]
    return len(_db_spans(server, parent))


def get(server, route, query):
    before = {s.span_id for s in server.spans()}
    r = requests.get(f"{server.base_url}{route}{query}", timeout=10)
    assert r.status_code == 200, r.text
    server.wait_for_spans(lambda ss: any(s.is_server and s.span_id not in before
                                         and s.attributes.get("http.route") == route for s in ss),
                          5, "no server span")
    return r, queries_for(server, route, before)


# (query string, expected rows, expected next offset or None, queries run)
EXACT_CASES = [
    ("", 100, 100, 2),                       # full first page: COUNT needed
    ("?offset=100", 100, 200, 2),            # full middle page
    ("?offset=200", 50, None, 1),            # short last page: total = 200 + 50
    ("?offset=200&limit=50", 50, None, 2),   # exactly full last page: cannot tell, COUNT
    ("?offset=300", 0, None, 2),             # past the end: empty page says nothing, COUNT
    ("?limit=1000", ROWS, None, 1),          # everything in one short page
    ("?offset=0&limit=0", 0, None, 2),       # limit 0 tells nothing
]


@pytest.mark.parametrize("route", ["/rows", "/plain"])
@pytest.mark.parametrize("query,rows,next_offset,queries", EXACT_CASES,
                         ids=[c[0] or "first" for c in EXACT_CASES])
def test_total_count_is_exact_and_counted_only_when_needed(server, route, query, rows, next_offset, queries):
    if route == "/rows":
        query = (query + "&" if query else "?") + "min=0"   # bind a typed parameter: the prepared path
    r, ran = get(server, route, query)
    body = r.json()
    assert len(body["data"]) == rows, body
    assert body["total_count"] == ROWS, body
    assert r.headers["X-Total-Count"] == str(ROWS)
    if next_offset is None:
        assert body["next"] == "", body
    else:
        assert f"offset={next_offset}" in body["next"], body
    assert ran == queries, f"{route}{query}: {ran} DuckDB queries, expected {queries}"


@pytest.mark.parametrize("query,rows,has_next", [
    ("", 100, True), ("?offset=200", 50, False), ("?offset=200&limit=50", 50, False),
    ("?offset=150&limit=50", 50, True), ("?offset=300", 0, False)])
def test_total_count_false_never_counts_and_reports_no_total(server, query, rows, has_next):
    r, ran = get(server, "/nototal", query)
    body = r.json()
    assert len(body["data"]) == rows, body
    assert "total_count" not in body and "X-Total-Count" not in r.headers, (body, dict(r.headers))
    assert bool(body["next"]) == has_next, body
    assert ran == 1, f"{ran} DuckDB queries for a total-count: false endpoint"


def test_mcp_tool_calls_do_not_count(server):
    before = {s.span_id for s in server.spans()}
    r = requests.post(f"{server.base_url}/mcp/jsonrpc", json={
        "jsonrpc": "2.0", "id": 1, "method": "tools/call",
        "params": {"name": "rows_tool", "arguments": {"min": 0}}}, timeout=15)
    assert r.status_code == 200 and "error" not in r.json(), r.text
    span = server.wait_for_spans(lambda ss: any(s.is_server and s.span_id not in before
                                                and s.name == "tools/call rows_tool" for s in ss), 5, "no MCP span")
    parent = [s for s in span if s.is_server and s.name == "tools/call rows_tool" and s.span_id not in before][-1]
    assert len(_db_spans(server, parent)) == 1
