"""Cache scheduler, populate-template checks and maintenance (#211, cluster B).

Each test starts its own server over its own DuckLake catalog. Covered:

* a bare-SELECT populate template is refused (it used to "succeed" and build nothing);
* `cache.schedule` fires without `ducklake.scheduler.enabled: true`;
* `POST .../cache/refresh?full=true` rebuilds an append cache instead of re-appending;
* a failed `warmup: always` over an existing table serves it `stale`, not 503;
* the one-shot `flapi cache refresh` reports duration/rows, names the lock clearly,
  and runs compaction (which proves the DuckLake maintenance functions exist);
* retention deletes the expired snapshots' files;
* SIGTERM does not wait for a running warm-up query.
"""
import glob
import json
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

TOKEN = "cache-scheduler-token"
AUTH = {"Authorization": f"Bearer {TOKEN}"}


def config(extra_ducklake="", extra=""):
    return (
        "project-name: cache-scheduler-test\n"
        "project-description: scheduler and maintenance\n"
        "template:\n  path: ./sqls\n"
        "connections:\n  src:\n    properties:\n      path: ./source.csv\n"
        "ducklake:\n  enabled: true\n  alias: cache\n"
        "  metadata-path: ./cache.ducklake\n  data-path: ./cache_data\n"
        + extra_ducklake + "rate_limit:\n  enabled: false\n" + extra
    )


SERVE = "SELECT * FROM {{{cache.catalog}}}.{{{cache.schema}}}.{{{cache.table}}} ORDER BY id\n"

FULL_CACHE = """\
CREATE OR REPLACE TABLE {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS
SELECT * FROM read_csv('{{{conn.path}}}')
"""

APPEND_CACHE = """\
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
"""


def endpoint(path, table, template, cache_extra=""):
    return (
        f"url-path: /{path}\nmethod: GET\ntemplate-source: serve.sql\nconnection: [src]\n"
        f"cache:\n  enabled: true\n  table: {table}\n  schema: main\n"
        f"  template-file: {path}_cache.sql\n{cache_extra}"
    ), template


def make_root(cfg, endpoints):
    root = tempfile.mkdtemp(prefix="flapi_sched_")
    os.makedirs(os.path.join(root, "sqls"))
    with open(os.path.join(root, "flapi.yaml"), "w") as f:
        f.write(cfg)
    with open(os.path.join(root, "sqls", "serve.sql"), "w") as f:
        f.write(SERVE)
    for path, (yaml_body, sql) in endpoints.items():
        with open(os.path.join(root, "sqls", f"{path}.yaml"), "w") as f:
            f.write(yaml_body)
        with open(os.path.join(root, "sqls", f"{path}_cache.sql"), "w") as f:
            f.write(sql)
    write_source(root, [(1, "a", 1), (2, "b", 1), (3, "c", 1)])
    return root


def write_source(root, rows):
    with open(os.path.join(root, "source.csv"), "w") as f:
        f.write("id,name,version\n")
        for r in rows:
            f.write(",".join(str(x) for x in r) + "\n")


def start(root, log_name="server.log", wait_live=True):
    port = free_port()
    log_path = os.path.join(root, log_name)
    log = open(log_path, "w")
    proc = subprocess.Popen(
        [flapi_binary(), "-c", "flapi.yaml", "-p", str(port), "--log-level", "info",
         "--config-service", "--config-service-token", TOKEN],
        cwd=root, stdout=log, stderr=subprocess.STDOUT, preexec_fn=os.setsid,
        env={**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"},
    )
    s = SimpleNamespace(base=f"http://127.0.0.1:{port}", root=root, log_path=log_path, proc=proc, log=log)
    if wait_live:
        deadline = time.time() + 60
        while time.time() < deadline:
            if proc.poll() is not None:
                pytest.fail("server exited:\n" + read_log(s))
            try:
                if requests.get(f"{s.base}/health/live", timeout=1).status_code == 200:
                    break
            except requests.RequestException:
                time.sleep(0.2)
        else:
            stop(s)
            pytest.fail("server never became live")
    return s


def stop(s, timeout=15):
    try:
        os.killpg(s.proc.pid, signal.SIGTERM)
        s.proc.wait(timeout=timeout)
    except ProcessLookupError:
        pass
    except subprocess.TimeoutExpired:
        os.killpg(s.proc.pid, signal.SIGKILL)
        s.proc.wait(timeout=5)
    finally:
        s.log.close()


def read_log(s):
    with open(s.log_path) as f:
        return f.read()


def wait_ready(s, timeout=30):
    deadline = time.time() + timeout
    while time.time() < deadline:
        r = requests.get(f"{s.base}/health", timeout=2)
        if r.json().get("status") in ("ready", "degraded"):
            return r
        time.sleep(0.2)
    pytest.fail("warm-up never finished:\n" + read_log(s)[-3000:])


def post_refresh(s, path, **params):
    return requests.post(f"{s.base}/api/v1/_config/endpoints/-{path}/cache/refresh",
                         headers=AUTH, params=params, timeout=60)


def audit(s, path):
    r = requests.get(f"{s.base}/api/v1/_config/endpoints/-{path}/cache/audit", headers=AUTH, timeout=30)
    assert r.status_code == 200, r.text
    return r.json()


def cache_state(s, key):
    r = requests.get(f"{s.base}/api/v1/_config/metrics", headers=AUTH, timeout=30)
    assert r.status_code == 200, r.text
    return {c["cache"]: c for c in r.json()["caches"]}[key]["state"]


def one_shot(root, *args):
    return subprocess.run(
        [flapi_binary(), "cache", "refresh", "-c", "flapi.yaml", *args],
        cwd=root, capture_output=True, text=True, timeout=120,
        env={**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"},
    )


class TestPopulateTemplate:
    def test_a_bare_select_is_refused_and_builds_nothing(self):
        root = make_root(config("  warmup: never\n"), {
            "sel": endpoint("sel", "sel_cache", "SELECT * FROM read_csv('{{{conn.path}}}')\n"),
        })
        s = start(root)
        try:
            wait_ready(s)   # the audit table is created by warm-up
            r = post_refresh(s, "sel")
            assert r.status_code == 400, r.text
            assert "cache template must write {{cache.catalog}}.{{cache.schema}}.{{cache.table}}" in r.text
            assert "CREATE OR REPLACE TABLE ... AS SELECT" in r.text
            newest = audit(s, "sel")[0]
            assert newest["status"] == "error", newest
            # Still not built: 503, not a 500 on a missing table.
            assert requests.get(f"{s.base}/sel", timeout=10).status_code == 503
        finally:
            stop(s)


    def test_without_template_file_the_endpoint_select_is_materialised(self):
        # No template-file: the serving template is the populate SQL. A bare
        # SELECT there used to build nothing, then (with the bare-SELECT
        # refusal) failed the cache; it is materialised as a full refresh.
        body = ("url-path: /raw\nmethod: GET\ntemplate-source: raw_cache.sql\nconnection: [src]\n"
                "cache:\n  enabled: true\n  table: raw_cache\n  schema: main\n"
                "  primary-key: [id]\n  cursor:\n    column: id\n    type: integer\n")
        root = make_root(config("  warmup: never\n"), {
            "raw": (body, "SELECT * FROM read_csv('{{{conn.path}}}') ORDER BY id\n"),
        })
        s = start(root)
        try:
            wait_ready(s)
            assert "cache on /raw has no template-file" in read_log(s)
            r = post_refresh(s, "raw")
            assert r.status_code == 200, r.text
            assert r.headers["Content-Type"].startswith("application/json")
            result = r.json()
            assert result["status"] == "refreshed", result
            assert result["mode"] == "full", "materialised whatever cursor/primary-key imply"
            # `full` is what the refresh did (#211 G1): a materialised full refresh loads
            # the whole source, though ?full= was not asked for.
            assert result["full"] is True
            assert result["rows_affected"] == 3
            assert result["duration_ms"] >= 0
            assert result["snapshot_id"]
            assert result["endpoint"] == "/raw"
            served = requests.get(f"{s.base}/raw", timeout=10)
            assert served.status_code == 200, served.text
            assert audit(s, "raw")[0]["sync_type"] == "full"
        finally:
            stop(s)

    def test_refresh_errors_are_json(self):
        root = make_root(config("  warmup: never\n"), {
            "sel": endpoint("sel", "sel_cache", "SELECT * FROM read_csv('{{{conn.path}}}')\n"),
        })
        s = start(root)
        try:
            wait_ready(s)
            r = post_refresh(s, "sel")
            assert r.status_code == 400
            assert r.json()["error"] == "Cache refresh failed"
            assert "bare SELECT" in r.json()["reason"]
            missing = post_refresh(s, "nope")
            assert missing.status_code == 404
            # The method-aware resolver names the path it could not find.
            assert missing.json()["error"].startswith("Endpoint not found")
        finally:
            stop(s)


class TestScheduler:
    def test_a_schedule_fires_without_the_scheduler_flag(self):
        # No `scheduler:` block at all. The schedule used to never fire.
        root = make_root(config("  warmup: never\n"), {
            "full": endpoint("full", "full_cache", FULL_CACHE, "  schedule: 2s\n"),
        })
        s = start(root)
        try:
            deadline = time.time() + 20
            while time.time() < deadline:
                if requests.get(f"{s.base}/full", timeout=5).status_code == 200:
                    break
                time.sleep(0.5)
            else:
                pytest.fail("scheduled refresh never built the cache:\n" + read_log(s)[-3000:])
            log = read_log(s)
            assert "Cache scheduler on: 1 scheduled cache(s), checked every 2 s" in log
            assert "Running scheduled cache refresh for endpoint: /full" in log
        finally:
            stop(s)

    def test_an_explicit_opt_out_is_honoured_and_logged(self):
        root = make_root(config("  warmup: never\n  scheduler:\n    enabled: false\n"), {
            "full": endpoint("full", "full_cache", FULL_CACHE, "  schedule: 1s\n"),
        })
        s = start(root)
        try:
            time.sleep(3)
            log = read_log(s)
            assert "will NOT be refreshed on schedule" in log
            assert "Running scheduled cache refresh" not in log
        finally:
            stop(s)


class TestFullRebuild:
    def test_full_true_rebuilds_an_append_cache(self):
        root = make_root(config("  warmup: never\n"), {
            "append": endpoint("append", "append_cache", APPEND_CACHE,
                               "  cursor:\n    column: id\n    type: integer\n"),
        })
        s = start(root)
        try:
            wait_ready(s)
            assert post_refresh(s, "append").status_code == 200
            write_source(root, [(1, "a", 1), (2, "b", 1), (3, "c", 1), (4, "d", 1)])
            assert post_refresh(s, "append").status_code == 200
            assert audit(s, "append")[0]["rows_affected"] == 1

            r = post_refresh(s, "append", full="true")
            assert r.status_code == 200, r.text
            # Rebuilt, not re-appended: four rows, no duplicates.
            rows = requests.get(f"{s.base}/append", timeout=10).json()["data"]
            assert sorted(x["id"] for x in rows) == [1, 2, 3, 4]
            assert audit(s, "append")[0]["rows_affected"] == 4
        finally:
            stop(s)


class TestWarmupOverExistingTable:
    def test_failed_always_warmup_serves_the_previous_table_stale(self):
        root = make_root(config("  warmup: always\n"), {
            "full": endpoint("full", "full_cache", FULL_CACHE),
        })
        s = start(root)
        try:
            wait_ready(s)
            assert requests.get(f"{s.base}/full", timeout=10).status_code == 200
        finally:
            stop(s)

        os.remove(os.path.join(root, "source.csv"))   # the source is down at restart
        s = start(root, "server2.log")
        try:
            wait_ready(s)
            served = requests.get(f"{s.base}/full", timeout=10)
            assert served.status_code == 200, served.text
            assert len(served.json()["data"]) == 3
            assert cache_state(s, "cache.main.full_cache") == "stale"
        finally:
            stop(s)


class TestOneShot:
    def test_summary_reports_duration_and_rows_and_compaction_runs(self):
        root = make_root(config("  warmup: never\n  compaction:\n    enabled: true\n    schedule: '@hourly'\n"), {
            "full": endpoint("full", "full_cache", FULL_CACHE),
        })
        r = one_shot(root, "--endpoint", "/full")
        assert r.returncode == 0, r.stdout + r.stderr
        item = json.loads(r.stdout.strip().splitlines()[-1])["refreshed"][0]
        assert item["status"] == "ok"
        assert item["rows_affected"] == 3
        assert item["duration_ms"] >= 0

        s = start(root)
        try:
            # Compaction ran from the one-shot (it was never run before, so it was due),
            # and every step - merge, rewrite, cleanup - exists in our DuckLake.
            events = requests.get(f"{s.base}/api/v1/_config/cache/audit", headers=AUTH, timeout=30).json()
            compaction = [e for e in events if e["sync_type"] == "compaction"]
            assert compaction and compaction[0]["status"] == "success", events

            # With the server holding the catalog, the one-shot says what to do.
            locked = one_shot(root, "--endpoint", "/full")
            assert locked.returncode == 1
            assert ("a running flapi holds the DuckLake catalog lock; refresh through the server: "
                    "flapii cache refresh /full") in locked.stdout, locked.stdout + locked.stderr
        finally:
            stop(s)


class TestRetentionReclaimsFiles:
    def test_expired_snapshots_files_are_deleted(self):
        # Global retention (it used to be parsed and never read), no inlining so
        # every refresh writes a parquet file.
        root = make_root(config("  warmup: never\n  data-inlining-row-limit: 0\n"
                                "  retention:\n    keep-last-snapshots: 1\n"), {
            "full": endpoint("full", "full_cache", FULL_CACHE),
        })
        s = start(root)
        try:
            wait_ready(s)
            for _ in range(5):
                assert post_refresh(s, "full").status_code == 200
            files = glob.glob(os.path.join(root, "cache_data", "**", "*.parquet"), recursive=True)
            files = [f for f in files if "full_cache" in f]
            assert len(files) <= 2, files
        finally:
            stop(s)


class TestShutdown:
    def test_sigterm_interrupts_a_running_warmup(self):
        slow = """\
CREATE OR REPLACE TABLE {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS
SELECT 1 AS id, SUM(i % 13) AS checksum FROM range(20000000000) t(i);
"""
        root = make_root(config("  warmup: always\n"), {"slow": endpoint("slow", "slow_cache", slow)})
        s = start(root)
        try:
            time.sleep(2)   # warm-up query is running
            assert requests.get(f"{s.base}/health", timeout=2).json()["status"] == "starting"
            t0 = time.time()
            os.killpg(s.proc.pid, signal.SIGTERM)
            s.proc.wait(timeout=30)
            assert time.time() - t0 < 10, "SIGTERM waited for the warm-up query"
            assert "interrupting 1 running cache refresh" in read_log(s)
        finally:
            if s.proc.poll() is None:
                os.killpg(s.proc.pid, signal.SIGKILL)
            s.log.close()
