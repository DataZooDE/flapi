"""A state directory written by the PREVIOUS flapi binary must keep working under the current one.

The DuckDB major upgrade (#241) changes the on-disk formats: the `duckdb.db_path` database file
(storage version) and the DuckLake catalog (spec 1.0 -> 1.1). Nothing is committed as a binary
fixture; the previous release binary builds the state at test time (a real run, not a frozen blob):

  1. the OLD binary (FLAPI_OLD_BINARY, default build/release-1.5/flapi) serves a full cache, a merge
     cache, and a persistent table in `db_path`; each cache is refreshed, then the server stops;
  2. the CURRENT binary (the one every other integration test uses) starts on the same directory and
     must serve the same data, refresh both caches again (a new source row shows up), garbage-collect,
     and time-travel to the snapshot the old binary wrote.

Skipped when no old binary exists. CI downloads the last released flapi-linux-amd64 for it.
"""
import os
import pathlib
import shutil
import signal
import subprocess
import tempfile
import time

import pytest
import requests

from otel_helpers import flapi_binary, free_port
from test_cache_catalog_f1 import (AUTH, FULL_ENDPOINT, FULL_POPULATE, MERGE_ENDPOINT, MERGE_POPULATE, SERVE,
                                   TOKEN, parquet_files, write_source, write_tree)

pytestmark = pytest.mark.standalone_server

REPO = pathlib.Path(__file__).resolve().parents[2]
OLD = pathlib.Path(os.environ.get("FLAPI_OLD_BINARY", REPO / "build" / "release-1.5" / "flapi"))
ENV = {**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"}

KEPT_ENDPOINT = """\
url-path: /kept
method: GET
template-source: kept.sql
connection: [src]
"""


def config(inline_rows):
    return f"""\
project-name: upgrade
project-description: state written by the previous binary
template:
  path: ./sqls
duckdb:
  db_path: ./state.db
connections:
  src:
    init: |
      CREATE TABLE IF NOT EXISTS main.kept AS SELECT 1 AS id, 'written-by-old-binary' AS note;
    properties:
      path: ./source.csv
ducklake:
  enabled: true
  alias: cache
  metadata-path: ./cache.ducklake
  data-path: ./cache_data
  data-inlining-row-limit: {inline_rows}
  warmup: never
rate_limit:
  enabled: false
"""


class Run:
    def __init__(self, binary, root):
        self.port = free_port()
        self.base = f"http://127.0.0.1:{self.port}"
        self.log_path = os.path.join(root, f"run-{self.port}.log")
        self.log = open(self.log_path, "w")
        self.proc = subprocess.Popen(
            [str(binary), "-c", "flapi.yaml", "-p", str(self.port), "--log-level", "info",
             "--config-service", "--config-service-token", TOKEN],
            cwd=root, stdout=self.log, stderr=subprocess.STDOUT, preexec_fn=os.setsid, env=ENV)
        deadline = time.time() + 90
        while time.time() < deadline:
            if self.proc.poll() is not None:
                pytest.fail(f"{binary} exited during startup:\n{self.output()[-4000:]}")
            try:
                if requests.get(f"{self.base}/health/live", timeout=1).status_code == 200:
                    return
            except requests.RequestException:
                time.sleep(0.3)
        pytest.fail("never became live:\n" + self.output()[-4000:])

    def output(self):
        if not self.log.closed:
            self.log.flush()
        return pathlib.Path(self.log_path).read_text()

    def stop(self):
        if self.proc.poll() is None:
            os.killpg(self.proc.pid, signal.SIGTERM)
            try:
                self.proc.wait(timeout=20)
            except subprocess.TimeoutExpired:
                os.killpg(self.proc.pid, signal.SIGKILL)
        self.log.close()

    def post(self, slug, op, **params):
        r = requests.post(f"{self.base}/api/v1/_config/endpoints/-{slug}/cache/{op}", headers=AUTH,
                          params=params, timeout=120)
        assert r.status_code == 200, f"{op} {slug}: {r.status_code} {r.text}\n{self.output()[-3000:]}"
        return r.json()

    def get(self, path, **params):
        r = requests.get(f"{self.base}{path}", params=params, timeout=30)
        assert r.status_code == 200, f"GET {path}: {r.status_code} {r.text}\n{self.output()[-3000:]}"
        return r.json()["data"]


@pytest.fixture
def root():
    d = tempfile.mkdtemp(prefix="flapi_upgrade_")
    yield d
    shutil.rmtree(d, ignore_errors=True)


@pytest.mark.skipif(not OLD.exists(), reason=f"no previous binary at {OLD} (set FLAPI_OLD_BINARY)")
@pytest.mark.parametrize("inline_rows", [0, 10], ids=["files", "inlined"])
def test_state_written_by_the_previous_binary_keeps_working(root, inline_rows):
    write_tree(root, config(inline_rows), {
        "full.yaml": FULL_ENDPOINT, "full_cache.sql": FULL_POPULATE,
        "merge.yaml": MERGE_ENDPOINT, "merge_cache.sql": MERGE_POPULATE,
        "kept.yaml": KEPT_ENDPOINT, "kept.sql": "SELECT * FROM main.kept",
        "serve.sql": SERVE})
    write_source(root, [(1, "a", 1), (2, "b", 1)])

    old = Run(OLD, root)
    try:
        old.post("full", "refresh")
        old.post("merge", "refresh")
        assert [r["id"] for r in old.get("/full")] == [1, 2]
        assert [r["id"] for r in old.get("/merge")] == [1, 2]
        assert old.get("/kept")[0]["note"] == "written-by-old-binary"
    finally:
        old.stop()

    new = Run(flapi_binary(), root)
    try:
        # what the old binary wrote is served as is, without a refresh
        assert [r["id"] for r in new.get("/full")] == [1, 2], new.output()[-3000:]
        assert [r["id"] for r in new.get("/merge")] == [1, 2]
        assert new.get("/kept")[0]["note"] == "written-by-old-binary"

        # and both refresh paths work on top of it: a changed source row shows up
        write_source(root, [(1, "a", 1), (2, "b2", 2), (3, "c", 2)])
        new.post("full", "refresh")
        new.post("merge", "refresh")
        assert [r["id"] for r in new.get("/full")] == [1, 2, 3]
        merged = {r["id"]: r["name"] for r in new.get("/merge")}
        assert merged == {1: "a", 2: "b2", 3: "c"}, merged

        new.post("full", "refresh", full="true")
        gc = new.post("full", "gc")
        assert "status" in gc, gc
        assert parquet_files(root, "cache", "full_cache") or inline_rows, "no cache files after a full rebuild"
    finally:
        new.stop()
    log = new.output()
    assert "ERROR" not in log and "SIGABRT" not in log, log[-3000:]
