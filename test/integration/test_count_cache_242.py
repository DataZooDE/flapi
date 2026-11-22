"""total_count of DuckLake-cached endpoints is reused between catalog changes (#242).

A full page of a cached endpoint ran `SELECT COUNT(*) FROM (<page query>)` on every request, which
costs about as much as the page itself. The count is now kept, keyed by the rendered SQL and the
bound values, and tagged with the DuckLake catalog's newest snapshot id - read with one small query
on the metadata database - so it is served only while the catalog is exactly as it was counted.

Checked against real servers, each answer against the exact total; which queries ran is read from
the trace (one DB span per DuckDB query, named by its role):

* a repeat request does not count again; a refresh, a full rebuild, a write endpoint that changes
  the table, and a refresh by ANOTHER process (one-shot `flapi cache refresh`, a writer beside a
  read-only replica on a shared catalog) are all seen by the next request;
* bound values and time-travel versions are part of the key;
* a query that reads anything but DuckLake tables, or calls a time- or session-dependent function,
  always counts.
"""
import os
import shutil
import tempfile

import pytest
import requests

from otel_helpers import flapi_binary, spans_from_file
from test_cache_catalog_f1 import Server, base_config, one_shot, write_source

pytestmark = pytest.mark.standalone_server

POPULATE = ("CREATE OR REPLACE TABLE {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS\n"
            "SELECT * FROM read_csv('{{{conn.path}}}')\n")
TABLE = "{{{cache.catalog}}}.{{{cache.schema}}}.{{{cache.table}}}"


def cached(path, template, extra=""):
    return (f"url-path: {path}\nmethod: GET\ntemplate-source: {template}\nconnection: [src]\n{extra}"
            "cache:\n  enabled: true\n  table: items\n  schema: main\n  template-file: items_cache.sql\n")


MIN = ("request:\n  - field-name: min\n    field-in: query\n    required: true\n"
       "    validators:\n      - type: int\n        min: 0\n")
VERSION = ("request:\n  - field-name: version\n    field-in: query\n    required: true\n"
           "    validators:\n      - type: int\n        min: 0\n")

FILES = {
    "items_cache.sql": POPULATE,
    "all.yaml": cached("/all", "all.sql"),
    "all.sql": f"SELECT * FROM {TABLE} ORDER BY id\n",
    "atleast.yaml": cached("/atleast", "atleast.sql", MIN),
    "atleast.sql": f"SELECT * FROM {TABLE} WHERE id >= {{{{ params.min }}}} ORDER BY id\n",
    "tt.yaml": cached("/tt", "tt.sql", VERSION),
    "tt.sql": f"SELECT * FROM {TABLE} AT (VERSION => {{{{ params.version }}}}) ORDER BY id\n",
    "lower.yaml": cached("/lower", "lower.sql"),
    "lower.sql": f"SELECT id, lower(name) AS name FROM {TABLE} WHERE name LIKE 'n%' ORDER BY id\n",
    # Never eligible: a time-dependent function, a source outside DuckLake.
    "recent.yaml": cached("/recent", "recent.sql"),
    "recent.sql": f"SELECT * FROM {TABLE} WHERE now() > TIMESTAMP '2000-01-01' ORDER BY id\n",
    "joined.yaml": cached("/joined", "joined.sql"),
    "joined.sql": (f"SELECT c.* FROM {TABLE} c JOIN read_csv('{{{{{{conn.path}}}}}}') s USING (id) "
                   "ORDER BY c.id\n"),
    # A write endpoint that changes the cached table outside any refresh.
    "add.yaml": ("url-path: /add\nmethod: POST\ntemplate-source: add.sql\nconnection: [src]\n"
                 "operation:\n  type: write\n  returns-data: false\n  transaction: false\n"),
    "add.sql": "INSERT INTO cache.main.items SELECT 100000 + range, 'added', 1 FROM range(7)\n",
}


def tracing(root):
    return (f"tracing:\n  enabled: true\n  exporter: otlp_file\n  capture: metadata\n"
            f"  file:\n    path: {os.path.join(root, 'traces.jsonl')}\n"
            "  flush:\n    mode: on_response\n  sample:\n    type: always_on\n")


def write_project(root, ducklake_extra="", metadata="./cache.ducklake"):
    os.makedirs(os.path.join(root, "sqls"), exist_ok=True)
    with open(os.path.join(root, "flapi.yaml"), "w") as f:
        f.write(base_config(ducklake_extra, metadata) + tracing(root))
    for name, body in FILES.items():
        with open(os.path.join(root, "sqls", name), "w") as f:
            f.write(body)


def rows(n, start=0):
    return [(i, f"n{i}", 1) for i in range(start, start + n)]


@pytest.fixture
def root():
    d = tempfile.mkdtemp(prefix="flapi_count242_")
    yield d
    shutil.rmtree(d, ignore_errors=True)


@pytest.fixture
def servers():
    started = []

    def start(root, extra_args=()):
        s = Server(root, extra_args)
        started.append(s)
        return s.wait_live()

    yield start
    for s in started:
        s.stop()


def db_spans_of_last_request(root, route, before):
    spans = spans_from_file(os.path.join(root, "traces.jsonl"))
    parents = [s for s in spans if s.is_server and s.attributes.get("http.route") == route
               and s.span_id not in before]
    assert parents, f"no server span for {route}"
    frontier, out = {parents[-1].span_id}, []
    while frontier:
        kids = [s for s in spans if s.parent_span_id in frontier]
        out += [s.name for s in kids if s.attributes.get("db.system.name") == "duckdb"]
        frontier = {s.span_id for s in kids}
    return out


def get(root, s, route, query=""):
    """(body, names of the DuckDB queries the request ran)."""
    import time
    before = {x.span_id for x in spans_from_file(os.path.join(root, "traces.jsonl"))}
    r = requests.get(f"{s.base}{route}{query}", timeout=30)
    assert r.status_code == 200, f"{route}{query}: {r.status_code} {r.text}\n{s.output()[-2000:]}"
    deadline = time.time() + 5
    while True:
        try:
            return r.json(), db_spans_of_last_request(root, route, before)
        except AssertionError:
            if time.time() > deadline:
                raise
            time.sleep(0.05)


def counted(names):
    """How many COUNT queries ran. Builds before #242 name no query on the plain path
    ("duckdb.query" for the page and the count alike), hence the page is discounted."""
    queries = [n for n in names if n not in ("count cache generation", "count cache check")]
    return len(queries) - 1


def assert_total(body, expected):
    assert body["total_count"] == expected, body.get("total_count")


def test_a_repeat_request_reuses_the_count_until_a_refresh(root, servers):
    write_project(root)
    write_source(root, rows(250))
    s = servers(root)
    s.refresh("all")
    body, names = get(root, s, "/all")
    assert_total(body, 250)
    assert counted(names) == 1, names
    for _ in range(3):
        body, names = get(root, s, "/all")
        assert_total(body, 250)
        assert len(body["data"]) == 100
        assert counted(names) == 0, names            # served from the count cache
    # Same rows, same next link, byte for byte, whether counted or cached.
    first = requests.get(f"{s.base}/all?offset=100", timeout=30).text
    assert requests.get(f"{s.base}/all?offset=100", timeout=30).text == first

    write_source(root, rows(330))
    s.refresh("all")
    body, names = get(root, s, "/all")
    assert_total(body, 330)
    assert counted(names) == 1, names

    write_source(root, rows(120))
    s.refresh("all", full=True)                      # DROP + CREATE
    body, names = get(root, s, "/all")
    assert_total(body, 120)
    body, names = get(root, s, "/all")
    assert_total(body, 120)
    assert counted(names) == 0, names


def test_bound_values_and_time_travel_are_part_of_the_key(root, servers):
    write_project(root)
    write_source(root, rows(250))
    s = servers(root)
    v1 = s.refresh("all")["snapshot_id"]
    for minimum, expected in [(0, 250), (50, 200), (0, 250), (100, 150), (50, 200)]:
        body, _ = get(root, s, "/atleast", f"?min={minimum}")
        assert_total(body, expected)
    body, names = get(root, s, "/atleast", "?min=100")
    assert counted(names) == 0 and body["total_count"] == 150, names

    write_source(root, rows(400))
    v2 = s.refresh("all")["snapshot_id"]
    for version, expected in [(v1, 250), (v2, 400), (v1, 250), (v2, 400)]:
        body, _ = get(root, s, "/tt", f"?version={version}")
        assert_total(body, expected)
    body, names = get(root, s, "/tt", f"?version={v1}")
    assert counted(names) == 0, names
    body, _ = get(root, s, "/atleast", "?min=100")
    assert_total(body, 300)


def test_only_queries_over_ducklake_tables_with_stable_functions_reuse_the_count(root, servers):
    write_project(root)
    write_source(root, rows(250))
    s = servers(root)
    s.refresh("all")
    for _ in range(2):
        body, names = get(root, s, "/lower")
        assert_total(body, 250)
    assert counted(names) == 0, names
    for route in ("/recent", "/joined"):
        for _ in range(3):
            body, names = get(root, s, route)
            assert_total(body, 250)
            assert counted(names) == 1, (route, names)


def test_a_write_endpoint_that_changes_the_table_is_seen(root, servers):
    write_project(root)
    write_source(root, rows(250))
    s = servers(root)
    s.refresh("all")
    get(root, s, "/all")
    body, names = get(root, s, "/all")
    assert counted(names) == 0
    r = requests.post(f"{s.base}/add", timeout=30)
    assert r.status_code in (200, 201), r.text
    body, names = get(root, s, "/all")
    assert_total(body, 257)


def test_a_refresh_by_another_process_is_seen(root, servers):
    # A SQLite catalog can be shared: a one-shot refresh runs beside the server,
    # and a read-only replica serves what a separate writer builds.
    write_project(root, metadata="sqlite:./cache.sqlite")
    write_source(root, rows(250))
    writer = servers(root)
    writer.refresh("all")
    body, _ = get(root, writer, "/all")
    assert_total(body, 250)
    body, names = get(root, writer, "/all")
    assert counted(names) == 0, names

    replica_root = os.path.join(root, "replica")
    os.makedirs(replica_root)
    for name in ("cache_data", "source.csv"):
        os.symlink(os.path.join(root, name), os.path.join(replica_root, name))
    write_project(replica_root, "  read-only: true",
                  metadata=f"sqlite:{os.path.join(root, 'cache.sqlite')}")
    with open(os.path.join(replica_root, "flapi.yaml")) as f:
        text = f.read()
    with open(os.path.join(replica_root, "flapi.yaml"), "w") as f:
        f.write(text.replace("data-path: ./cache_data", f"data-path: {os.path.join(root, 'cache_data')}"))
    replica = servers(replica_root)
    body, _ = get(replica_root, replica, "/all")
    assert_total(body, 250)
    get(replica_root, replica, "/all")

    write_source(root, rows(310))
    writer.refresh("all")                            # the replica's process never hears of it
    body, _ = get(replica_root, replica, "/all")
    assert_total(body, 310)

    write_source(root, rows(275))
    done = one_shot(flapi_binary(), root, "--endpoint", "/all")
    assert done.returncode == 0, done.stdout + done.stderr
    for server, where in ((writer, root), (replica, replica_root)):
        body, _ = get(where, server, "/all")
        assert_total(body, 275)
