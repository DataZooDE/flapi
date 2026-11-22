"""Per-request CPU work removed from the read path (#241), checked against a real server.

* Rows are written from the DuckDB vectors straight into the response text: the body must stay
  byte-for-byte what the crow::json::wvalue-per-row path wrote (doubles, escapes, NULLs,
  timestamps, nested values, duplicate column names).
* A body above 1 MiB used to take Crow's synchronous "stream" write, after which the connection
  was never read again: the next keep-alive request on it was reset.
* Endpoint reads reuse a per-thread connection with cached prepared SELECTs: no session state
  may leak between requests, and file data must never be served stale.
"""
import os
import shutil
import socket
import tempfile
import time

import pytest
import requests

from test_duckdb_upgrade import Run
from otel_helpers import flapi_binary

pytestmark = pytest.mark.standalone_server

SCALARS = (
    "SELECT i, i::TINYINT AS t8, (i * 576460752303423488)::BIGINT AS t64, "
    "(i::UBIGINT * 1152921504606846976::UBIGINT) AS u64, "
    "[3.0, 1.5e20, 1e21, 1e-5, 0.0001, -0.0, 0.1, 1e16, 1e300, 9.5e18][i + 1]::DOUBLE AS dbl, "
    "CASE WHEN i = 2 THEN NULL WHEN i = 3 THEN 'inf'::DOUBLE ELSE i / 7.0 END::DOUBLE AS d2, "
    "CASE WHEN i % 4 = 0 THEN NULL ELSE 'q\"u\\o' || chr(10) || chr(1) || chr(9) || 'é' || i END AS s, "
    "DATE '1999-12-31' + i::INT AS dt, "
    "CASE WHEN i % 3 = 2 THEN NULL ELSE TIMESTAMP '1969-12-31 23:59:59.123456' + to_microseconds(i) END AS ts, "
    "'1969-12-31 23:59:59.123456789'::TIMESTAMP_NS AS tsns, TIMESTAMPTZ '2024-05-06 07:08:09.5+00' AS tstz, "
    "CASE WHEN i = 1 THEN 'infinity'::TIMESTAMP ELSE TIMESTAMP '2000-01-01' END AS tsinf, "
    "(i % 2 = 0) AS b, 1.25::DECIMAL(10,2) AS dec, 0.1::FLOAT AS f, {'a': i, 'b': [i, NULL]} AS st, "
    "'{\"j\":1}'::JSON AS j FROM range(10) t(i)\n")

# The text the wvalue-per-row writer produced for row 0 and row 1 (taken from 18af94d).
ROW0 = ('{"i":0,"t8":0,"t64":0,"u64":0,"dbl":3,"d2":0,"s":null,"dt":"1999-12-31",'
        '"ts":"1969-12-31T23:59:59.123456","tsns":"1969-12-31T23:59:59.123456789",'
        '"tstz":"2024-05-06T07:08:09.500000Z","tsinf":"2000-01-01T00:00:00.000000","b":true,"dec":1.25,'
        '"f":0.1,"st":{"b":[0,null],"a":0},"j":{"j":1}}')


def write(root, extra_endpoints=()):
    os.makedirs(os.path.join(root, "sqls"))
    os.makedirs(os.path.join(root, "data"))
    with open(os.path.join(root, "flapi.yaml"), "w") as f:
        f.write("project-name: cpu241\ntemplate:\n  path: ./sqls\nconnections:\n  c:\n    properties:\n"
                "      path: ./data/t.csv\nrate_limit:\n  enabled: false\n")
    endpoints = {
        "scalars": ("with-pagination: false\n", SCALARS),
        "dup": ("with-pagination: false\n", "SELECT 1 AS a, 2 AS a, 'x' AS b\n"),
        "big": ("with-pagination: false\n", "SELECT range AS id, 'value_' || range AS s FROM range(150000)\n"),
        "csvfile": ("with-pagination: false\n", "SELECT * FROM read_csv('{{{ conn.path }}}') ORDER BY 1\n"),
        "setvar": ("with-pagination: false\n",
                   "SET VARIABLE leaked = 'yes';\nSELECT getvariable('leaked') AS v\n"),
        "getvar": ("with-pagination: false\n", "SELECT coalesce(getvariable('leaked'), 'no') AS v\n"),
        "tmpmake": ("with-pagination: false\n",
                    "CREATE OR REPLACE TEMP TABLE t241 AS SELECT 1 AS x;\nSELECT count(*) AS n FROM t241\n"),
        "tmpread": ("with-pagination: false\n",
                    "SELECT count(*) AS n FROM duckdb_tables() WHERE table_name = 't241'\n"),
    }
    endpoints.update(dict(extra_endpoints))
    for name, (extra, sql) in endpoints.items():
        with open(os.path.join(root, "sqls", f"{name}.yaml"), "w") as f:
            f.write(f"url-path: /{name}\nmethod: GET\ntemplate-source: {name}.sql\nconnection: [c]\n{extra}")
        with open(os.path.join(root, "sqls", f"{name}.sql"), "w") as f:
            f.write(sql)
    with open(os.path.join(root, "data", "t.csv"), "w") as f:
        f.write("a,b\n1,x\n2,y\n")


@pytest.fixture
def server():
    root = tempfile.mkdtemp(prefix="flapi_cpu241_")
    write(root)
    run = Run(flapi_binary(), root)
    run.root = root
    yield run
    run.stop()
    shutil.rmtree(root, ignore_errors=True)


def test_rows_keep_their_exact_text(server):
    body = requests.get(f"{server.base}/scalars", timeout=30).text
    assert body.startswith("[" + ROW0 + ","), body[:600]
    rows = requests.get(f"{server.base}/scalars", timeout=30).json()
    # Doubles: integral -> integer digits, fractions shortest round-trip, NaN/inf -> null.
    assert [r["dbl"] for r in rows][:8] == [3, 150000000000000000000, 1e21, 1e-05, 0.0001, 0, 0.1,
                                            10000000000000000]
    assert rows[3]["d2"] is None and rows[2]["d2"] is None
    assert rows[1]["s"] == 'q"u\\o\n\x01\té1'
    assert rows[1]["tsinf"] == "infinity"
    # Duplicate column names keep the old (map) shape.
    assert requests.get(f"{server.base}/dup", timeout=30).text == '[{"b":"x","a":2}]'


def test_csv_format_is_unchanged(server):
    csv = requests.get(f"{server.base}/scalars?format=csv", timeout=30).text.splitlines()
    assert csv[0].startswith("i,t8,t64,u64,dbl,d2,s,dt,ts"), csv[0]
    assert len(csv) == 11


def http_get_twice_on_one_socket(port, path):
    """Two requests on one TCP connection; returns the two status lines."""
    sock = socket.create_connection(("127.0.0.1", port), timeout=60)
    statuses = []
    try:
        for _ in range(2):
            sock.sendall(f"GET {path} HTTP/1.1\r\nHost: x\r\n\r\n".encode())
            buf = b""
            while b"\r\n\r\n" not in buf:
                chunk = sock.recv(65536)
                assert chunk, "connection closed before the response headers"
                buf += chunk
            head, rest = buf.split(b"\r\n\r\n", 1)
            length = next(int(line.split(b":")[1]) for line in head.split(b"\r\n")
                          if line.lower().startswith(b"content-length:"))
            while len(rest) < length:
                chunk = sock.recv(1 << 20)
                assert chunk, "connection closed mid-body"
                rest += chunk
            statuses.append(head.split(b"\r\n")[0].decode())
    finally:
        sock.close()
    return statuses


def test_a_large_body_keeps_the_connection_alive(server):
    assert len(requests.get(f"{server.base}/big", timeout=60).content) > 2 * 1024 * 1024
    assert http_get_twice_on_one_socket(server.port, "/big") == ["HTTP/1.1 200 OK"] * 2


def test_session_state_does_not_leak_between_requests(server):
    s = requests.Session()   # one connection: the same io thread, likely the same worker
    for _ in range(20):
        assert s.get(f"{server.base}/setvar", timeout=30).json() == [{"v": "yes"}]
        assert s.get(f"{server.base}/getvar", timeout=30).json() == [{"v": "no"}]
        assert s.get(f"{server.base}/tmpmake", timeout=30).json() == [{"n": 1}]
        assert s.get(f"{server.base}/tmpread", timeout=30).json() == [{"n": 0}]


def test_a_rewritten_source_file_is_never_served_stale(server):
    s = requests.Session()
    path = os.path.join(server.root, "data", "t.csv")
    for i in range(10):
        with open(path, "w") as f:
            f.write("a,b\n" + "".join(f"{k},v{i}\n" for k in range(i + 1)))
        rows = s.get(f"{server.base}/csvfile", timeout=30).json()
        assert len(rows) == i + 1 and rows[-1]["b"] == f"v{i}", rows
    with open(path, "w") as f:          # a schema change must rebind, not fail or serve old columns
        f.write("z\nhello\n")
    assert s.get(f"{server.base}/csvfile", timeout=30).json() == [{"z": "hello"}]
