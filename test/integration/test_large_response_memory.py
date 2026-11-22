"""A large response is written row by row, not held as a JSON tree plus copies (#241).

Every row used to become a crow::json object that was deep-copied three times, and the text was
copied twice more: a 9 MB response peaked at ~640 MB. The rows now go straight into the response
text. The body must stay byte-for-byte what it was, for JSON, paginated JSON, CSV and MCP.
"""
import json
import shutil
import tempfile

import pytest
import requests

from test_duckdb_upgrade import Run
from otel_helpers import flapi_binary

pytestmark = pytest.mark.standalone_server

ROWS = 200_000
SQL = ("SELECT range AS id, range * 1.5 AS d, 'name_' || range AS s, {'k': range % 7} AS st, "
       "[range, range + 1] AS l FROM range(%d) ORDER BY id\n")


def write(root):
    import os
    os.makedirs(os.path.join(root, "sqls"))
    with open(os.path.join(root, "flapi.yaml"), "w") as f:
        f.write("project-name: big\ntemplate:\n  path: ./sqls\nconnections:\n  c:\n    properties: {}\n"
                "rate_limit:\n  enabled: false\n")
    for name, extra in (("big", "with-pagination: false\n"), ("paged", "")):
        with open(os.path.join(root, "sqls", f"{name}.yaml"), "w") as f:
            f.write(f"url-path: /{name}\nmethod: GET\ntemplate-source: q.sql\nconnection: [c]\n{extra}"
                    f"mcp-tool:\n  name: {name}_tool\n  description: rows\n")
    with open(os.path.join(root, "sqls", "q.sql"), "w") as f:
        f.write(SQL.replace("%d", str(ROWS)))


@pytest.fixture
def server():
    root = tempfile.mkdtemp(prefix="flapi_big_")
    write(root)
    run = Run(flapi_binary(), root)
    yield run
    run.stop()
    shutil.rmtree(root, ignore_errors=True)


def hwm_kb(pid):
    with open(f"/proc/{pid}/status") as f:
        return next(int(line.split()[1]) for line in f if line.startswith("VmHWM:"))


@pytest.mark.skipif(not __import__("os").path.exists("/proc/self/status"), reason="needs /proc")
def test_a_large_response_does_not_cost_many_times_its_size(server):
    requests.get(f"{server.base}/paged?limit=1", timeout=30)   # warm up: extensions, first plan
    before = hwm_kb(server.proc.pid)
    r = requests.get(f"{server.base}/big", timeout=120)
    assert r.status_code == 200
    body_kb = len(r.content) / 1024
    grown_kb = hwm_kb(server.proc.pid) - before
    # The tree-plus-copies path grew ~55x the body; streaming stays near ~13x (DuckDB's own
    # result buffers included). 30x leaves room for allocator noise.
    assert grown_kb < 30 * body_kb, f"peak grew {grown_kb / 1024:.0f} MB for a {body_kb / 1024:.1f} MB body"


def test_json_csv_and_mcp_bodies_agree(server):
    data = requests.get(f"{server.base}/big", timeout=120).json()
    assert len(data) == ROWS
    assert data[0] == {"id": 0, "d": 0.0, "s": "name_0", "st": {"k": 0}, "l": [0, 1]}, data[0]
    assert list(data[1].keys()) == ["id", "d", "s", "st", "l"]           # SELECT order kept
    assert data[3]["d"] == 4.5

    page = requests.get(f"{server.base}/paged?offset=199990&limit=100", timeout=60).json()
    assert page["data"] == data[199990:] and page["total_count"] == ROWS and page["next"] == ""

    csv = requests.get(f"{server.base}/paged?offset=0&limit=3&format=csv", timeout=60).text.splitlines()
    assert csv[0] == "id,d,s,st,l" and len(csv) == 4, csv
    assert csv[1] == '0,0.0,"name_0",{"k":0},[0,1]', csv   # values as JSON, as before

    rpc = requests.post(f"{server.base}/mcp/jsonrpc", json={
        "jsonrpc": "2.0", "id": 1, "method": "tools/call",
        "params": {"name": "paged_tool", "arguments": {}}}, timeout=60).json()
    text = rpc["result"]["content"][0]["text"]
    rows = json.loads(text)
    assert rows == data[:len(rows)] and len(rows) > 0, text[:200]
