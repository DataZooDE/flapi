"""Cache retention, replicas, packaging and catalog safety (#211, cluster F1).

Each test starts its own server (or one-shot) on a throwaway DuckLake catalog:

* retention for the DEFAULT schema `cache` with CTAS full refreshes, and for the
  previous incarnation of a cache after a `--full` rebuild, actually expires
  snapshots and deletes files (it matched nothing before, so disk grew forever);
* GC answers what it did as JSON, `no_policy` when there is no retention;
* an obsolete table left in an old schema can be dropped; the refresh answer
  never reports another table's snapshot id;
* a read-only replica refuses every cache-mutating route (409) and reports the
  effective warm-up;
* time travel to an unknown snapshot is a 404, not a 500;
* a packed binary's `cache refresh` reads its bundled config, and `embed://`
  works from the source tree; pack leaves the catalog out;
* catalog directories are created, lock conflicts are explained, credentials
  in a metadata-path DSN are never printed.
"""
import glob
import json
import os
import shutil
import signal
import subprocess
import tempfile
import time
from types import SimpleNamespace

import pytest
import requests

from otel_helpers import flapi_binary, free_port

pytestmark = pytest.mark.standalone_server

TOKEN = "f1-token"
AUTH = {"Authorization": f"Bearer {TOKEN}"}
ENV = {**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"}

SERVE = "SELECT * FROM {{{cache.catalog}}}.{{{cache.schema}}}.{{{cache.table}}} ORDER BY id"


def base_config(ducklake_extra="", metadata="./cache.ducklake"):
    return f"""\
project-name: f1
project-description: cluster F1
template:
  path: ./sqls
connections:
  src:
    properties:
      path: ./source.csv
ducklake:
  enabled: true
  alias: cache
  metadata-path: {metadata}
  data-path: ./cache_data
  data-inlining-row-limit: 0
  warmup: never
{ducklake_extra}
rate_limit:
  enabled: false
"""


FULL_ENDPOINT = """\
url-path: /full
method: GET
template-source: serve.sql
connection: [src]
cache:
  enabled: true
  table: full_cache
  template-file: full_cache.sql
  retention:
    keep-last-snapshots: 2
"""
FULL_POPULATE = """\
CREATE OR REPLACE TABLE {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS
SELECT * FROM read_csv('embed://source.csv')
"""

MERGE_ENDPOINT = """\
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
  retention:
    keep-last-snapshots: 2
"""
MERGE_POPULATE = """\
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
"""

TIME_TRAVEL_ENDPOINT = """\
url-path: /tt
method: GET
template-source: tt.sql
connection: [src]
request:
  - field-name: version
    field-in: query
    required: true
    validators:
      - type: int
        min: 0
cache:
  enabled: true
  table: full_cache
  template-file: full_cache.sql
"""
TIME_TRAVEL_SQL = (
    "SELECT * FROM {{{cache.catalog}}}.{{{cache.schema}}}.{{{cache.table}}} "
    "AT (VERSION => {{ params.version }}) ORDER BY id"
)


def write_tree(root, config, files):
    os.makedirs(os.path.join(root, "sqls"), exist_ok=True)
    with open(os.path.join(root, "flapi.yaml"), "w") as f:
        f.write(config)
    for name, body in files.items():
        with open(os.path.join(root, "sqls", name), "w") as f:
            f.write(body)


def write_source(root, rows):
    with open(os.path.join(root, "source.csv"), "w") as f:
        f.write("id,name,version\n")
        for r in rows:
            f.write(",".join(str(x) for x in r) + "\n")


def parquet_files(root, *parts):
    return sorted(glob.glob(os.path.join(root, "cache_data", *parts, "**", "*.parquet"), recursive=True))


class Server:
    def __init__(self, root, extra_args=()):
        self.root = root
        self.port = free_port()
        self.base = f"http://127.0.0.1:{self.port}"
        self.log_path = os.path.join(root, f"server-{self.port}.log")
        self.log = open(self.log_path, "w")
        self.proc = subprocess.Popen(
            [flapi_binary(), "-c", "flapi.yaml", "-p", str(self.port), "--log-level", "info",
             "--config-service", "--config-service-token", TOKEN, *extra_args],
            cwd=root, stdout=self.log, stderr=subprocess.STDOUT, preexec_fn=os.setsid, env=ENV,
        )

    def wait_live(self):
        deadline = time.time() + 60
        while time.time() < deadline:
            if self.proc.poll() is not None:
                pytest.fail("server exited:\n" + self.output())
            try:
                if requests.get(f"{self.base}/health/live", timeout=1).status_code == 200:
                    return self
            except requests.RequestException:
                time.sleep(0.3)
        pytest.fail("server never became live:\n" + self.output())

    def output(self):
        self.log.flush()
        with open(self.log_path) as f:
            return f.read()

    def stop(self):
        if self.proc.poll() is None:
            os.killpg(self.proc.pid, signal.SIGTERM)
            try:
                self.proc.wait(timeout=20)
            except subprocess.TimeoutExpired:
                os.killpg(self.proc.pid, signal.SIGKILL)
                self.proc.wait(timeout=10)
        self.log.close()

    def refresh(self, slug, full=False):
        url = f"{self.base}/api/v1/_config/endpoints/-{slug}/cache/refresh" + ("?full=true" if full else "")
        r = requests.post(url, headers=AUTH, timeout=60)
        assert r.status_code == 200, f"refresh {slug}: {r.status_code} {r.text}\n{self.output()[-3000:]}"
        return r.json()

    def gc(self, slug):
        return requests.post(f"{self.base}/api/v1/_config/endpoints/-{slug}/cache/gc", headers=AUTH, timeout=60)


@pytest.fixture
def root():
    d = tempfile.mkdtemp(prefix="flapi_f1_")
    yield d
    shutil.rmtree(d, ignore_errors=True)


@pytest.fixture
def servers():
    started = []

    def start(root, extra_args=(), wait=True):
        s = Server(root, extra_args)
        started.append(s)
        return s.wait_live() if wait else s

    yield start
    for s in started:
        s.stop()


def one_shot(binary, cwd, *args, timeout=120):
    return subprocess.run([binary, "cache", "refresh", *args], cwd=cwd, env=ENV,
                          capture_output=True, text=True, timeout=timeout)


class TestRetention:
    def test_default_schema_full_refresh_keeps_only_the_policy(self, root, servers):
        # The default schema `cache` is a keyword: DuckLake records the create as
        # '"cache".full_cache', which retention never matched. Five refreshes kept
        # five snapshots and five files, forever.
        write_tree(root, base_config(), {"full.yaml": FULL_ENDPOINT, "full_cache.sql": FULL_POPULATE,
                                         "serve.sql": SERVE})
        write_source(root, [(1, "a", 1), (2, "b", 1)])
        s = servers(root)
        answers = [s.refresh("full") for _ in range(5)]
        assert len(parquet_files(root, "cache", "full_cache")) == 2, s.output()[-3000:]
        # Every refresh answers the snapshot IT committed, each a new one.
        ids = [a.get("snapshot_id") for a in answers]
        assert all(ids) and len(set(ids)) == 5, answers
        r = requests.get(f"{s.base}/full", timeout=10)
        assert r.status_code == 200 and len(r.json()["data"]) == 2

        # GC with nothing left to expire says so, as JSON.
        g = s.gc("full")
        assert g.status_code == 200, g.text
        body = g.json()
        assert body["status"] == "nothing_to_expire"
        assert {"expired_snapshots", "files_deleted", "bytes_freed", "message"} <= set(body)

    def test_full_rebuild_does_not_leak_the_previous_incarnation(self, root, servers):
        # `--full` on a merge cache is DROP + CREATE (new table id); the old
        # incarnation's snapshots were never expirable, so each rebuild leaked
        # the previous base files.
        write_tree(root, base_config(), {"merge.yaml": MERGE_ENDPOINT, "merge_cache.sql": MERGE_POPULATE,
                                         "serve.sql": SERVE})
        rows = [(i, "n", 1) for i in range(1, 51)]
        write_source(root, rows)
        s = servers(root)
        s.refresh("merge")
        version = 1
        for _ in range(4):
            version += 1
            rows.append((len(rows) + 1, "n", version))
            write_source(root, rows)
            s.refresh("merge")
            s.refresh("merge", full=True)
        files = parquet_files(root, "main", "merge_cache")
        # The live table is one rebuilt base file; at most the previous
        # incarnation's last file is still reachable through the kept snapshots.
        assert len(files) <= 3, (files, s.output()[-3000:])
        r = requests.get(f"{s.base}/merge", timeout=10)
        assert r.status_code == 200 and len(r.json()["data"]) == len(rows)

    def test_gc_without_a_policy_answers_no_policy(self, root, servers):
        endpoint = FULL_ENDPOINT.replace("  retention:\n    keep-last-snapshots: 2\n", "")
        write_tree(root, base_config(), {"full.yaml": endpoint, "full_cache.sql": FULL_POPULATE,
                                         "serve.sql": SERVE})
        write_source(root, [(1, "a", 1)])
        s = servers(root)
        s.refresh("full")
        g = s.gc("full")
        assert g.status_code == 200, g.text
        body = g.json()
        assert body["status"] == "no_policy"
        assert body["expired_snapshots"] == 0
        assert "no retention policy configured" in body["message"]


class TestObsoleteTable:
    def test_schema_change_leaves_a_table_that_can_be_dropped(self, root, servers):
        endpoint = FULL_ENDPOINT.replace("  table: full_cache\n", "  table: full_cache\n  schema: old\n")
        write_tree(root, base_config(), {"full.yaml": endpoint, "full_cache.sql": FULL_POPULATE,
                                         "serve.sql": SERVE})
        write_source(root, [(1, "a", 1), (2, "b", 1)])
        s = servers(root)
        s.refresh("full")
        s.refresh("full")
        s.stop()

        # The operator moves the cache to a new schema.
        with open(os.path.join(root, "sqls", "full.yaml"), "w") as f:
            f.write(endpoint.replace("schema: old", "schema: new"))
        s = servers(root)
        answers = [s.refresh("full") for _ in range(4)]
        # Retention works for the new table although old.full_cache exists.
        assert len(parquet_files(root, "new", "full_cache")) == 2, s.output()[-3000:]
        ids = [int(a["snapshot_id"]) for a in answers]
        assert ids == sorted(ids) and len(set(ids)) == 4, answers

        drop = f"{s.base}/api/v1/_config/cache/drop-table"
        in_use = requests.post(drop, headers=AUTH, json={"schema": "new", "table": "full_cache"}, timeout=30)
        assert in_use.status_code == 409 and in_use.json()["error"] == "cache_table_in_use"
        missing = requests.post(drop, headers=AUTH, json={"schema": "old", "table": "nope"}, timeout=30)
        assert missing.status_code == 404
        assert requests.post(drop, json={"schema": "old", "table": "full_cache"}, timeout=30).status_code == 401

        r = requests.post(drop, headers=AUTH, json={"schema": "old", "table": "full_cache"}, timeout=30)
        assert r.status_code == 200, r.text
        assert r.json()["status"] == "dropped"
        # The old table's last file is still visible (time travel) in the new
        # table's retained snapshots, taken while it was live; it is deleted as
        # soon as the new table's retention expires those.
        s.refresh("full")
        s.refresh("full")
        assert parquet_files(root, "old", "full_cache") == [], s.output()[-3000:]
        assert len(parquet_files(root, "new", "full_cache")) == 2
        assert requests.get(f"{s.base}/full", timeout=10).status_code == 200


class TestTimeTravel:
    def test_unknown_snapshot_is_404(self, root, servers):
        write_tree(root, base_config(), {"tt.yaml": TIME_TRAVEL_ENDPOINT, "tt.sql": TIME_TRAVEL_SQL,
                                         "full_cache.sql": FULL_POPULATE})
        write_source(root, [(1, "a", 1)])
        s = servers(root)
        answer = s.refresh("tt")
        ok = requests.get(f"{s.base}/tt", params={"version": answer["snapshot_id"]}, timeout=10)
        assert ok.status_code == 200, ok.text
        r = requests.get(f"{s.base}/tt", params={"version": 999999}, timeout=10)
        assert r.status_code == 404, r.text
        assert r.json()["error"] == "snapshot_not_found"


class TestReadOnlyReplica:
    def test_replica_refuses_cache_mutations_and_reports_effective_warmup(self, root, servers):
        write_tree(root, base_config(), {"full.yaml": FULL_ENDPOINT, "full_cache.sql": FULL_POPULATE,
                                         "serve.sql": SERVE})
        write_source(root, [(1, "a", 1)])
        w = servers(root)
        w.refresh("full")
        w.stop()

        with open(os.path.join(root, "flapi.yaml"), "w") as f:
            f.write(base_config("  read-only: true").replace("warmup: never", "warmup: always"))
        r = servers(root)
        project = requests.get(f"{r.base}/api/v1/_config/project", headers=AUTH, timeout=10).json()
        assert project["ducklake"]["warmup"] == "always"
        assert project["ducklake"]["warmup_effective"] == "never"

        template_url = f"{r.base}/api/v1/_config/endpoints/-full/cache/template"
        before = open(os.path.join(root, "sqls", "full_cache.sql")).read()
        put = requests.put(template_url, headers=AUTH, json={"template": "SELECT 2"}, timeout=10)
        assert put.status_code == 409 and put.json()["error"] == "read_only_cache"
        assert open(os.path.join(root, "sqls", "full_cache.sql")).read() == before

        gc = r.gc("full")
        refresh = requests.post(f"{r.base}/api/v1/_config/endpoints/-full/cache/refresh", headers=AUTH, timeout=10)
        assert gc.status_code == 409 and refresh.status_code == 409
        # Operation-specific advice, not "refresh it from the writer" for a GC.
        assert gc.json()["message"] != refresh.json()["message"]
        assert "refresh it" not in gc.json()["message"]
        drop = requests.post(f"{r.base}/api/v1/_config/cache/drop-table", headers=AUTH,
                             json={"schema": "cache", "table": "x"}, timeout=10)
        assert drop.status_code == 409
        assert requests.get(f"{r.base}/full", timeout=10).status_code == 200


class TestReplicaNeverBuilt:
    UNBUILT_ENDPOINT = FULL_ENDPOINT.replace("/full", "/unbuilt").replace("full_cache", "unbuilt_cache")

    def test_never_built_table_is_cache_not_built_from_the_first_request(self, root, servers):
        # #217: a replica answered 503 cache_warming for ~2 s after the port
        # opened, THEN cache_not_built. It must say cache_not_built at once.
        write_tree(root, base_config(), {"full.yaml": FULL_ENDPOINT, "full_cache.sql": FULL_POPULATE,
                                         "unbuilt.yaml": self.UNBUILT_ENDPOINT, "serve.sql": SERVE})
        write_source(root, [(1, "a", 1)])
        w = servers(root)
        w.refresh("full")
        w.stop()

        with open(os.path.join(root, "flapi.yaml"), "w") as f:
            f.write(base_config("  read-only: true"))
        for _ in range(3):
            r = Server(root)
            try:
                answers = []
                deadline = time.time() + 20
                session = requests.Session()
                while time.time() < deadline and r.proc.poll() is None:
                    try:
                        resp = session.get(f"{r.base}/unbuilt", timeout=2)
                    except requests.RequestException:
                        continue
                    answers.append((resp.status_code, resp.json().get("error")))
                    if len(answers) >= 60:
                        break
                assert len(answers) >= 60, r.output()[-3000:]
                assert set(answers) == {(503, "cache_not_built")}, sorted(set(answers))
                health = session.get(f"{r.base}/health", timeout=5)
                assert health.status_code == 503
                assert health.json()["status"] != "starting", health.text
            finally:
                r.stop()


class TestCatalogSafety:
    def test_missing_catalog_directories_are_created(self, root, servers):
        write_tree(root, base_config(metadata="./state/deep/cache.ducklake"),
                   {"full.yaml": FULL_ENDPOINT, "full_cache.sql": FULL_POPULATE, "serve.sql": SERVE})
        write_source(root, [(1, "a", 1)])
        s = servers(root)
        s.refresh("full")
        assert os.path.isfile(os.path.join(root, "state", "deep", "cache.ducklake"))

    def test_lock_conflicts_are_explained(self, root, servers):
        write_tree(root, base_config(), {"full.yaml": FULL_ENDPOINT, "full_cache.sql": FULL_POPULATE,
                                         "serve.sql": SERVE})
        write_source(root, [(1, "a", 1)])
        servers(root)   # the writer holds the file catalog

        done = one_shot(flapi_binary(), root, "--all")
        assert done.returncode == 1
        assert "stop the replicas" in done.stdout, done.stdout
        assert "Failed to detach" not in done.stderr, done.stderr

        with open(os.path.join(root, "replica.yaml"), "w") as f:
            f.write(base_config("  read-only: true"))
        port = free_port()
        replica = subprocess.run([flapi_binary(), "-c", "replica.yaml", "-p", str(port)], cwd=root, env=ENV,
                                 capture_output=True, text=True, timeout=60)
        assert replica.returncode == 1
        out = replica.stdout + replica.stderr
        assert "stop the other flapi" in out and "Postgres/SQLite/MySQL" in out, out

    def test_metadata_dsn_password_is_never_printed(self, root):
        config = base_config(metadata="'postgres:dbname=lake host=127.0.0.1 port=1 user=x password=hunter2'")
        write_tree(root, config, {"full.yaml": FULL_ENDPOINT, "full_cache.sql": FULL_POPULATE, "serve.sql": SERVE})
        write_source(root, [(1, "a", 1)])
        port = free_port()
        run = subprocess.run([flapi_binary(), "-c", "flapi.yaml", "-p", str(port), "--log-level", "debug"],
                             cwd=root, env=ENV, capture_output=True, text=True, timeout=120)
        out = run.stdout + run.stderr
        assert run.returncode == 1, out[-2000:]
        assert "Failed to attach DuckLake catalog" in out, out[-2000:]
        assert "hunter2" not in out


class TestMetricsAndAudit:
    def test_metrics_name_the_endpoint_before_any_refresh(self, root, servers):
        write_tree(root, base_config(), {"full.yaml": FULL_ENDPOINT, "full_cache.sql": FULL_POPULATE,
                                         "serve.sql": SERVE})
        write_source(root, [(1, "a", 1)])
        s = servers(root)
        m = requests.get(f"{s.base}/api/v1/_config/metrics", headers=AUTH, params={"endpoint": "/full"},
                         timeout=10).json()
        assert [c["endpoint"] for c in m["caches"]] == ["/full"], m["caches"]
        everything = requests.get(f"{s.base}/api/v1/_config/metrics", headers=AUTH, timeout=10).json()
        assert all(c.get("endpoint") for c in everything["caches"]), everything["caches"]

    def test_compaction_history_is_filterable(self, root, servers):
        write_tree(root, base_config("  compaction:\n    enabled: true\n    schedule: 1d\n    delete-threshold: 0.3"),
                   {"full.yaml": FULL_ENDPOINT, "full_cache.sql": FULL_POPULATE, "serve.sql": SERVE})
        write_source(root, [(1, "a", 1)])
        done = one_shot(flapi_binary(), root, "--all")   # compaction never ran: due
        assert done.returncode == 0, done.stdout + done.stderr
        s = servers(root)
        rows = requests.get(f"{s.base}/api/v1/_config/cache/audit", headers=AUTH,
                            params={"sync_type": "compaction"}, timeout=10).json()
        assert len(rows) == 1 and rows[0]["sync_type"] == "compaction" and rows[0]["status"] == "success", rows
        assert "DuckLake compaction every 1d" in s.output()


class TestPackaging:
    def test_embed_paths_work_from_the_source_tree(self, root):
        write_tree(root, base_config(), {"full.yaml": FULL_ENDPOINT, "full_cache.sql": FULL_POPULATE,
                                         "serve.sql": SERVE})
        write_source(root, [(1, "a", 1), (2, "b", 1)])
        done = one_shot(flapi_binary(), root, "--endpoint", "/full")
        assert done.returncode == 0, done.stdout + done.stderr
        assert json.loads(done.stdout.strip().splitlines()[-1])["refreshed"][0]["rows_affected"] == 2

    def test_packed_binary_refreshes_its_bundled_config(self, root):
        write_tree(root, base_config(), {"full.yaml": FULL_ENDPOINT, "full_cache.sql": FULL_POPULATE,
                                         "serve.sql": SERVE})
        write_source(root, [(1, "a", 1), (2, "b", 1)])
        # A catalog in the tree: pack must leave it out.
        assert one_shot(flapi_binary(), root, "--all").returncode == 0
        assert os.path.exists(os.path.join(root, "cache.ducklake"))

        out_dir = tempfile.mkdtemp(prefix="flapi_f1_pack_")
        try:
            packed = os.path.join(out_dir, "flapi-packed")
            pack = subprocess.run([flapi_binary(), "pack", "--in", root, "--out", packed], env=ENV,
                                  capture_output=True, text=True, timeout=300)
            assert pack.returncode == 0, pack.stdout + pack.stderr
            info = subprocess.run([packed, "info"], env=ENV, capture_output=True, text=True, timeout=60)
            assert "cache.ducklake" not in info.stdout and "cache_data/" not in info.stdout, info.stdout

            run_dir = os.path.join(out_dir, "run")
            os.makedirs(run_dir)
            # From an empty directory: the config comes from the bundle; the
            # catalog lands beside the working directory.
            done = one_shot(packed, run_dir, "--all")
            assert done.returncode == 0, done.stdout + done.stderr
            assert json.loads(done.stdout.strip().splitlines()[-1])["refreshed"][0]["rows_affected"] == 2
        finally:
            shutil.rmtree(out_dir, ignore_errors=True)
