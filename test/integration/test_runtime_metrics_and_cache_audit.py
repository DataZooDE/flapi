"""Runtime metrics and measured cache refreshes (#211).

Two things an operator tuning flAPI needs and did not have:

* cache.audit.sync_events wrote CURRENT_TIMESTAMP for both start and end and
  never filled duration_ms / rows_affected / snapshot_id, so "how long does my
  refresh take, and how many rows did it move" had no answer;
* GET /api/v1/_config/metrics had tracing and Arrow counters but nothing per
  endpoint - no request counts, error rates or latency - and nothing per cache.

Exercised against a real server for all three cache modes, with the source
changing between refreshes so the incremental paths are the ones measured.
"""
import os
import signal
import subprocess
import tempfile
import time
from types import SimpleNamespace

import pytest
import requests

from otel_helpers import flapi_binary, free_port

pytestmark = pytest.mark.standalone_server

TOKEN = "runtime-metrics-token"
AUTH = {"Authorization": f"Bearer {TOKEN}"}

CONFIG = """\
project-name: runtime-metrics-test
project-description: metrics and cache audit
template:
  path: ./sqls
connections:
  src:
    properties:
      path: ./source.csv
      fail_at: '1'
ducklake:
  enabled: true
  alias: cache
  metadata-path: ./cache.ducklake
  data-path: ./cache_data
  warmup: never
rate_limit:
  enabled: false
"""

SERVE = """SELECT * FROM {{{cache.catalog}}}.{{{cache.schema}}}.{{{cache.table}}} ORDER BY id"""

ENDPOINTS = {
    # Full refresh: no cursor.
    "full.yaml": """\
url-path: /full
method: GET
template-source: serve.sql
connection: [src]
cache:
  enabled: true
  table: full_cache
  schema: main
  template-file: full_cache.sql
""",
    "full_cache.sql": """\
CREATE OR REPLACE TABLE {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS
SELECT * FROM read_csv('{{{conn.path}}}')
""",
    # Append: cursor only. First run (no watermark) creates the table empty.
    "append.yaml": """\
url-path: /append
method: GET
template-source: serve.sql
connection: [src]
cache:
  enabled: true
  table: append_cache
  schema: main
  cursor:
    column: id
    type: integer
  template-file: append_cache.sql
""",
    "append_cache.sql": """\
{{^cache.previousSnapshotTimestamp}}
CREATE TABLE IF NOT EXISTS {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS
SELECT * FROM read_csv('{{{conn.path}}}') WHERE false;
{{/cache.previousSnapshotTimestamp}}
INSERT INTO {{cache.catalog}}.{{cache.schema}}.{{cache.table}}
SELECT * FROM read_csv('{{{conn.path}}}')
WHERE 1=1
{{#cache.previousSnapshotTimestamp}}
  AND id > {{cache.previousSnapshotTimestamp}}
{{/cache.previousSnapshotTimestamp}};
""",
    # Merge: cursor + primary key.
    "merge.yaml": """\
url-path: /merge
method: GET
template-source: serve.sql
connection: [src]
cache:
  enabled: true
  table: merge_cache
  schema: main
  primary-key: [id]
  cursor:
    column: version
    type: integer
  template-file: merge_cache.sql
""",
    "merge_cache.sql": """\
{{^cache.previousSnapshotTimestamp}}
CREATE TABLE IF NOT EXISTS {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS
SELECT * FROM read_csv('{{{conn.path}}}') WHERE false;
{{/cache.previousSnapshotTimestamp}}
MERGE INTO {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS t
USING (
  SELECT * FROM read_csv('{{{conn.path}}}')
  WHERE 1=1
  {{#cache.previousSnapshotTimestamp}}
    AND version > {{cache.previousSnapshotTimestamp}}
  {{/cache.previousSnapshotTimestamp}}
) AS s
ON t.id = s.id
WHEN MATCHED THEN UPDATE SET name = s.name, version = s.version
WHEN NOT MATCHED THEN INSERT *;
""",
    # Soft deletes the DuckLake way: DuckLake's MERGE takes one UPDATE/DELETE
    # action, so deletes are a DELETE statement before a single-action MERGE.
    "soft.yaml": """\
url-path: /soft
method: GET
template-source: serve.sql
connection: [src]
cache:
  enabled: true
  table: soft_cache
  schema: main
  primary-key: [id]
  cursor:
    column: version
    type: integer
  template-file: soft_cache.sql
""",
    "soft_cache.sql": """\
{{^cache.previousSnapshotTimestamp}}
CREATE TABLE IF NOT EXISTS {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS
SELECT id, name, version FROM read_csv('{{{conn.path}}}') WHERE false;
{{/cache.previousSnapshotTimestamp}}
DELETE FROM {{cache.catalog}}.{{cache.schema}}.{{cache.table}}
WHERE id IN (SELECT id FROM read_csv('{{{conn.path}}}') WHERE name = 'DELETED');
MERGE INTO {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS t
USING (
  SELECT id, name, version FROM read_csv('{{{conn.path}}}')
  WHERE name <> 'DELETED'
  {{#cache.previousSnapshotTimestamp}}
    AND version > {{cache.previousSnapshotTimestamp}}
  {{/cache.previousSnapshotTimestamp}}
) AS s
ON t.id = s.id
WHEN MATCHED THEN UPDATE SET name = s.name, version = s.version
WHEN NOT MATCHED THEN INSERT *;
""",
    # A template that fails AFTER writing: nothing it wrote may survive.
    "broken.yaml": """\
url-path: /broken
method: GET
template-source: serve.sql
connection: [src]
cache:
  enabled: true
  table: broken_cache
  schema: main
  template-file: broken_cache.sql
""",
    "broken_cache.sql": """\
CREATE OR REPLACE TABLE {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS
SELECT * FROM read_csv('{{{conn.path}}}');
SELECT error('boom') FROM read_csv('{{{conn.path}}}') WHERE id >= (SELECT {{{conn.fail_at}}});
""",
    "serve.sql": SERVE,
}


def write_source(root, rows):
    with open(os.path.join(root, "source.csv"), "w") as f:
        f.write("id,name,version\n")
        for r in rows:
            f.write(",".join(str(x) for x in r) + "\n")


@pytest.fixture
def server():
    root = tempfile.mkdtemp(prefix="flapi_metrics_")
    os.makedirs(os.path.join(root, "sqls"))
    with open(os.path.join(root, "flapi.yaml"), "w") as f:
        f.write(CONFIG)
    for name, body in ENDPOINTS.items():
        with open(os.path.join(root, "sqls", name), "w") as f:
            f.write(body)
    write_source(root, [(1, "a", 1), (2, "b", 1), (3, "c", 1)])

    port = free_port()
    log_path = os.path.join(root, "server.log")
    log = open(log_path, "w")
    proc = subprocess.Popen(
        [flapi_binary(), "-c", "flapi.yaml", "-p", str(port), "--log-level", "warning",
         "--config-service", "--config-service-token", TOKEN],
        cwd=root, stdout=log, stderr=subprocess.STDOUT, preexec_fn=os.setsid,
        env={**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"},
    )
    base = f"http://127.0.0.1:{port}"
    deadline = time.time() + 60
    while time.time() < deadline:
        if proc.poll() is not None:
            pytest.fail("server exited:\n" + open(log_path).read())
        try:
            if requests.get(f"{base}/health/live", timeout=1).status_code == 200:
                break
        except requests.RequestException:
            time.sleep(0.3)
    else:
        pytest.fail("server never became live")

    s = SimpleNamespace(base=base, root=root, log_path=log_path)
    try:
        yield s
    finally:
        os.killpg(proc.pid, signal.SIGTERM)
        try:
            proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            os.killpg(proc.pid, signal.SIGKILL)
        log.close()


def refresh(s, path):
    r = requests.post(f"{s.base}/api/v1/_config/endpoints/-{path}/cache/refresh", headers=AUTH, timeout=60)
    assert r.status_code == 200, f"refresh {path}: {r.status_code} {r.text}\n{open(s.log_path).read()[-2000:]}"


def audit(s, path):
    r = requests.get(f"{s.base}/api/v1/_config/endpoints/-{path}/cache/audit", headers=AUTH, timeout=30)
    assert r.status_code == 200, r.text
    # newest first
    return r.json()


def metrics(s, **params):
    r = requests.get(f"{s.base}/api/v1/_config/metrics", headers=AUTH, params=params, timeout=30)
    assert r.status_code == 200, r.text
    return r.json()


class TestMeasuredCacheRefresh:
    @pytest.mark.parametrize("path,first,second", [
        # full re-reads everything: 3 rows, then 4
        ("full", 3, 4),
        # append: 3 rows, then only the one new id
        ("append", 3, 1),
        # merge: 3 rows, then one update (id 2 -> version 2) plus one insert
        ("merge", 3, 2),
    ])
    def test_audit_records_duration_rows_and_snapshot(self, server, path, first, second):
        refresh(server, path)
        write_source(server.root, [(1, "a", 1), (2, "b2", 2), (3, "c", 1), (4, "d", 2)])
        refresh(server, path)

        events = [e for e in audit(server, path) if e["sync_type"] != "garbage_collection"]
        assert len(events) >= 2, events
        newest, older = events[0], events[1]
        for e in (newest, older):
            assert e["status"] == "success", e
            assert e["duration_ms"] is not None and e["duration_ms"] >= 0, e
            assert e["snapshot_id"] not in (None, ""), e
            assert e["sync_started_at"] <= e["sync_completed_at"], e
        assert older["rows_affected"] == first, older
        assert newest["rows_affected"] == second, newest
        assert newest["sync_type"] == {"full": "full", "append": "append", "merge": "merge"}[path]

        served = requests.get(f"{server.base}/{path}", timeout=30)
        assert served.status_code == 200, served.text
        names = {row["id"]: row["name"] for row in served.json()["data"]}
        assert names[4] == "d"
        if path != "append":
            assert names[2] == "b2", "full and merge pick up the update"

    def test_metrics_report_each_cache(self, server):
        refresh(server, "merge")
        body = metrics(server)
        caches = {c["cache"]: c for c in body["caches"]}
        merge = caches["cache.main.merge_cache"]
        assert merge["state"] == "ready"
        assert merge["endpoint"] == "/merge"
        assert merge["refreshes"] == 1 and merge["failures"] == 0
        assert merge["last"]["mode"] == "merge"
        assert merge["last"]["rows_affected"] == 3
        assert merge["last"]["duration_ms"] >= 0
        assert merge["data_age_seconds"] >= 0
        # never refreshed with warmup: never - listed, not invented
        assert caches["cache.main.full_cache"]["state"] == "unbuilt"
        assert "last" not in caches["cache.main.full_cache"]

    def test_a_failed_refresh_is_counted_and_timed(self, server):
        refresh(server, "full")
        os.remove(os.path.join(server.root, "source.csv"))
        r = requests.post(f"{server.base}/api/v1/_config/endpoints/-full/cache/refresh", headers=AUTH, timeout=60)
        assert r.status_code >= 400
        newest = audit(server, "full")[0]
        assert newest["status"] == "error"
        assert newest["duration_ms"] is not None
        assert newest["rows_affected"] is None
        cache = {c["cache"]: c for c in metrics(server)["caches"]}["cache.main.full_cache"]
        assert cache["failures"] == 1 and cache["refreshes"] == 2
        assert cache["state"] == "stale", "the last good snapshot is still served"


class TestRequestMetrics:
    def test_per_endpoint_counts_status_classes_and_latency(self, server):
        refresh(server, "full")
        for _ in range(5):
            assert requests.get(f"{server.base}/full", timeout=30).status_code == 200
        requests.get(f"{server.base}/does-not-exist", timeout=30)

        body = metrics(server)["requests"]
        series = {(s.get("method"), s.get("route")): s for s in body["series"]}
        full = series[("GET", "/full")]
        assert full["requests"] == 5
        assert full["status"]["2xx"] == 5
        lat = full["latency_ms"]
        assert 0 <= lat["p50"] <= lat["p95"] <= lat["p99"]
        assert lat["max"] >= lat["avg"] >= 0
        assert series[("GET", "<unmatched>")]["status"]["4xx"] >= 1
        assert body["total"] >= 6

    def test_filter_narrows_to_one_endpoint(self, server):
        refresh(server, "full")
        refresh(server, "merge")
        requests.get(f"{server.base}/full", timeout=30)
        requests.get(f"{server.base}/merge", timeout=30)
        body = metrics(server, endpoint="/merge")
        assert {s["route"] for s in body["requests"]["series"]} == {"/merge"}
        assert [c["endpoint"] for c in body["caches"]] == ["/merge"]

    def test_rejections_are_counted(self, server):
        requests.get(f"{server.base}/api/v1/_config/metrics", timeout=10)  # no token -> 401
        body = metrics(server)
        # The config-service 401 is a handler response, not middleware; it is
        # still a request the server rejected and is counted.
        assert body["requests"]["rejections"]["unauthorized"] >= 1


class TestMetricsMcpTool:
    def test_flapi_get_metrics_returns_the_same_document(self, server):
        refresh(server, "full")
        requests.get(f"{server.base}/full", timeout=30)
        payload = {"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                   "params": {"name": "flapi_get_metrics", "arguments": {"endpoint": "/full"}}}
        r = requests.post(f"{server.base}/mcp/jsonrpc", json=payload, headers=AUTH, timeout=30)
        assert r.status_code == 200, r.text
        result = r.json()["result"]
        assert not result.get("isError"), result
        import json
        doc = json.loads(result["content"][0]["text"])
        assert [s["route"] for s in doc["requests"]["series"]] == ["/full"]
        assert doc["caches"][0]["endpoint"] == "/full"


class TestRefreshIsAtomic:
    def test_a_failed_first_build_leaves_no_table(self, server):
        r = requests.post(f"{server.base}/api/v1/_config/endpoints/-broken/cache/refresh", headers=AUTH, timeout=60)
        assert r.status_code >= 400
        # The CREATE that ran before the failure was rolled back: the endpoint
        # must not serve an empty (or any) table as if it were built.
        served = requests.get(f"{server.base}/broken", timeout=30)
        assert served.status_code == 503, served.text
        state = {c["cache"]: c for c in metrics(server)["caches"]}["cache.main.broken_cache"]["state"]
        assert state in ("failed", "unbuilt"), state

    def test_deletes_then_single_action_merge(self, server):
        refresh(server, "soft")
        # update id 2, soft-delete id 3, insert id 4
        write_source(server.root, [(1, "a", 1), (2, "b2", 2), (3, "DELETED", 2), (4, "d", 2)])
        refresh(server, "soft")
        rows = {r["id"]: r["name"] for r in requests.get(f"{server.base}/soft", timeout=30).json()["data"]}
        assert rows == {1: "a", 2: "b2", 4: "d"}
        newest = audit(server, "soft")[0]
        # 1 deleted + 1 updated + 1 inserted, summed over the script's statements
        assert newest["rows_affected"] == 3, newest
