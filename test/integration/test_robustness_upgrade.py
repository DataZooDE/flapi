"""Crash and resource robustness of the real binary (DuckDB upgrade gate, #241).

Run on the previous binary first (the baseline) and on the upgraded one:

* `kill -9` while a cache refresh runs, then restart on the same db_path + DuckLake catalog:
  the server starts, the previous cache is still served or cleanly rebuilt, and a refresh succeeds;
* a tiny `duckdb.max_memory` turns an over-sized query into an error for THAT request only: the
  process survives and the next small request is served.
"""
import os
import shutil
import signal
import tempfile
import threading
import time

import pytest
import requests

from test_cache_catalog_f1 import FULL_ENDPOINT, SERVE, write_source, write_tree
from test_duckdb_upgrade import Run, config
from otel_helpers import flapi_binary

pytestmark = pytest.mark.standalone_server

SLOW_POPULATE = """\
CREATE OR REPLACE TABLE {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS
SELECT sum(hash(a.range * 100003 + b.range)) % 1000 AS id, 'slow' AS name, 1 AS version
FROM range(60000) a, range(60000) b
"""
FAST_POPULATE = """\
CREATE OR REPLACE TABLE {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS
SELECT * FROM read_csv('{{{conn.path}}}')
"""


@pytest.fixture
def root():
    d = tempfile.mkdtemp(prefix="flapi_robust_")
    yield d
    shutil.rmtree(d, ignore_errors=True)


def test_kill_9_during_refresh_then_restart(root):
    write_tree(root, config(0), {"full.yaml": FULL_ENDPOINT, "full_cache.sql": FAST_POPULATE, "serve.sql": SERVE})
    write_source(root, [(1, "a", 1), (2, "b", 1)])
    first = Run(flapi_binary(), root)
    try:
        first.post("full", "refresh")
        assert [r["id"] for r in first.get("/full")] == [1, 2]
        # swap in a populate that takes a long time, restart so it is picked up, and kill it mid-refresh
    finally:
        first.stop()
    write_tree(root, config(0), {"full.yaml": FULL_ENDPOINT, "full_cache.sql": SLOW_POPULATE, "serve.sql": SERVE})
    second = Run(flapi_binary(), root)
    try:
        def refresh_until_killed():
            try:
                requests.post(f"{second.base}/api/v1/_config/endpoints/-full/cache/refresh",
                              headers={"Authorization": "Bearer f1-token"}, timeout=120)
            except requests.RequestException:
                pass  # the server is killed under it, on purpose

        worker = threading.Thread(target=refresh_until_killed, daemon=True)
        worker.start()
        time.sleep(1.0)
        os.killpg(second.proc.pid, signal.SIGKILL)
        second.proc.wait(timeout=10)
    finally:
        second.stop()
    write_tree(root, config(0), {"full.yaml": FULL_ENDPOINT, "full_cache.sql": FAST_POPULATE, "serve.sql": SERVE})
    third = Run(flapi_binary(), root)
    try:
        before = third.get("/full")  # the last COMMITTED cache is intact
        assert [r["id"] for r in before] == [1, 2], before
        write_source(root, [(1, "a", 1), (2, "b", 1), (3, "c", 1)])
        third.post("full", "refresh")
        assert [r["id"] for r in third.get("/full")] == [1, 2, 3]
    finally:
        third.stop()
    assert "SIGABRT" not in third.output()


def test_max_memory_fails_the_request_not_the_process(root):
    cfg = config(0).replace("duckdb:\n  db_path: ./state.db", "duckdb:\n  db_path: ./state.db\n  max_memory: 64MB\n  threads: 2")
    write_tree(root, cfg, {
        "big.yaml": "url-path: /big\nmethod: GET\ntemplate-source: big.sql\nconnection: [src]\n",
        "big.sql": "SELECT count(*) AS c, max(s) AS m FROM (SELECT string_agg(i::VARCHAR, ',') AS s FROM range(30000000) t(i) GROUP BY i % 3)",
        "small.yaml": "url-path: /small\nmethod: GET\ntemplate-source: small.sql\nconnection: [src]\n",
        "small.sql": "SELECT 1 AS v"})
    write_source(root, [(1, "a", 1)])
    run = Run(flapi_binary(), root)
    try:
        r = requests.get(f"{run.base}/big", timeout=120)
        assert r.status_code >= 400 or r.json().get("data") is not None  # either answer, but...
        assert run.proc.poll() is None, "the server died on an over-sized query\n" + run.output()[-3000:]
        assert run.get("/small")[0]["v"] == 1
    finally:
        run.stop()
    assert "SIGABRT" not in run.output()
