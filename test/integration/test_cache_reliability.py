"""Cache scheduling and refresh failures must not take the server or a working
endpoint down, and must not leak errors (#193).

Found by the crew review and reproduced against a real binary:
 * `cache.schedule: 5x` was accepted at load and then std::terminate()d the
   process ~2 s later (the heartbeat worker threw outside its try block);
 * a failed refresh of an already-populated cache turned the endpoint into HTTP 503
   although the last snapshot was fine, and the raw DuckDB error - with file
   paths - was returned to anonymous callers and by /health.
"""

import os
import shutil
import subprocess
import tempfile
import time

import pytest
import requests

from otel_helpers import flapi_binary, free_port

TOKEN = "cache-rel-token-123"


def _w(base, rel, text):
    path = os.path.join(base, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(text)


class _Server:
    def __init__(self, schedule="1h", worker_interval=3600, with_source=True):
        self.tmp = tempfile.mkdtemp(prefix="flapi_cacherel_")
        self.port = free_port()
        self.base = f"http://127.0.0.1:{self.port}"
        self.src = os.path.join(self.tmp, "data", "src.csv")
        if with_source:
            _w(self.tmp, "data/src.csv", "id,v\n1,a\n2,b\n")
        _w(self.tmp, "flapi.yaml",
           "project-name: p\nproject-description: d\ntemplate:\n  path: ./sqls\n"
           "connections:\n  inmem:\n    properties:\n      database: ':memory:'\n"
           f"      csv: '{self.src}'\n"
           f"heartbeat:\n  enabled: true\n  worker-interval: {worker_interval}\n"
           "ducklake:\n  enabled: true\n  alias: cache\n  metadata-path: ./cache.ducklake\n"
           "  data-path: ./cache\n  scheduler:\n    enabled: true\n")
        _w(self.tmp, "sqls/c.yaml",
           "url-path: /c\nmethod: GET\ntemplate-source: c.sql\nconnection: [inmem]\n"
           f"cache:\n  enabled: true\n  table: c_cache\n  schedule: {schedule}\n"
           "  template-file: c_populate.sql\n")
        # Requests read the cache table; the populate template reads the source.
        _w(self.tmp, "sqls/c.sql", "SELECT * FROM {{{cache.catalog}}}.{{{cache.schema}}}.{{{cache.table}}}\n")
        _w(self.tmp, "sqls/c_populate.sql",
           "CREATE OR REPLACE TABLE {{{cache.catalog}}}.{{{cache.schema}}}.{{{cache.table}}} AS "
           "SELECT * FROM read_csv('{{{conn.csv}}}')\n")
        self.log = os.path.join(self.tmp, "server.log")
        self.proc = subprocess.Popen(
            [flapi_binary(), "-c", os.path.join(self.tmp, "flapi.yaml"), "-p", str(self.port),
             "--config-service", "--config-service-token", TOKEN, "--log-level", "info"],
            stdout=open(self.log, "w"), stderr=subprocess.STDOUT, cwd=self.tmp,
            env={**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"})

    def wait_up(self, seconds=60, ready=True):
        """Listening - and, with `ready`, the caches warmed (warm-up runs after the
        listener is up, so /health/live alone races it)."""
        live = False
        for _ in range(int(seconds / 0.2)):
            if self.proc.poll() is not None:
                return False
            try:
                if not live:
                    live = requests.get(f"{self.base}/health/live", timeout=2).status_code == 200
                if live and not ready:
                    return True
                if live:
                    h = requests.get(f"{self.base}/health", timeout=2).json()
                    # total == 0 is "warm-up has not registered the cache yet", not "ready".
                    if h.get("status") == "ready" and h.get("caches", {}).get("total", 0) >= 1:
                        return True
            except requests.RequestException:
                pass
            time.sleep(0.2)
        return live and not ready

    def refresh(self):
        return requests.post(f"{self.base}/api/v1/_config/endpoints/-c/cache/refresh",
                             headers={"Authorization": f"Bearer {TOKEN}"}, timeout=30)

    def output(self):
        return open(self.log).read()

    def stop(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            self.proc.wait(timeout=30)
        shutil.rmtree(self.tmp, ignore_errors=True)


class TestSchedulesAreValidatedUpFront:

    @pytest.mark.parametrize("schedule", ["5x", "0s", "0", "abc", "30000d", "-5m", "5 m"])
    def test_a_bad_schedule_stops_startup_with_a_clear_message(self, schedule):
        s = _Server(schedule=f"'{schedule}'")
        try:
            up = s.wait_up(20)
            time.sleep(1)
            assert not up, f"schedule {schedule!r} was accepted"
            out = s.output()
            assert "schedule" in out.lower() and schedule.strip("'") in out, out[-800:]
            assert "core dumped" not in out and s.proc.returncode != -6, "the process aborted"
        finally:
            s.stop()

    @pytest.mark.parametrize("schedule", ["30s", "5m", "6h", "1d"])
    def test_valid_schedules_still_work(self, schedule):
        s = _Server(schedule=schedule)
        try:
            assert s.wait_up(60), s.output()[-800:]
        finally:
            s.stop()

    def test_the_cache_api_rejects_a_bad_schedule(self):
        s = _Server()
        try:
            assert s.wait_up(60), s.output()[-800:]
            r = requests.put(f"{s.base}/api/v1/_config/endpoints/-c/cache",
                             headers={"Authorization": f"Bearer {TOKEN}", "Content-Type": "application/json"},
                             json={"schedule": "5x"}, timeout=10)
            assert r.status_code == 400, r.text
        finally:
            s.stop()


class TestAFailedRefreshKeepsServingTheLastSnapshot:

    def test_the_endpoint_keeps_answering_and_nothing_leaks(self):
        s = _Server()
        try:
            assert s.wait_up(60), s.output()[-800:]
            # Settle: at startup the scheduler's first refresh and the warm-up both touch the
            # readiness state, so deleting the source in that window fails the WARM-UP, not a
            # refresh of a built cache. Only a stable "ready" is a precondition.
            time.sleep(2.5)
            assert requests.get(f"{s.base}/health", timeout=5).json()["status"] == "ready"
            assert requests.get(f"{s.base}/c", timeout=10).status_code == 200
            os.remove(s.src)
            # The refresh itself fails. (The scheduler's first scan may be refreshing at the
            # same moment, in which case ours is skipped as a duplicate: try again.)
            for _ in range(10):
                if s.refresh().status_code != 200:
                    break
                time.sleep(0.5)
            else:
                pytest.fail("a refresh with no source never failed")

            r = requests.get(f"{s.base}/c", timeout=10)
            assert r.status_code == 200, (f"a failed refresh took the endpoint offline: {r.status_code} {r.text[:200]}\n"
                                          + "\n".join(l[:220] for l in s.output().splitlines() if "ache" in l)[-2500:])
            assert len(r.json()["data"]) == 2

            for text in (r.text, requests.get(f"{s.base}/health", timeout=10).text):
                assert s.src not in text and "read_csv" not in text and "IO Error" not in text, (
                    "a DuckDB error with a file path reached an unauthenticated caller: " + text[:300])
            health = requests.get(f"{s.base}/health", timeout=10)
            assert "degraded" in health.text or "stale" in health.text, health.text

            # And it recovers by itself once the source is back.
            _w(s.tmp, "data/src.csv", "id,v\n1,a\n2,b\n3,c\n")
            assert s.refresh().status_code == 200
            assert len(requests.get(f"{s.base}/c", timeout=10).json()["data"]) == 3
            assert "degraded" not in requests.get(f"{s.base}/health", timeout=10).text
        finally:
            s.stop()

    def test_a_cache_that_never_built_is_503_without_the_error_text(self):
        s = _Server(with_source=False)
        try:
            assert s.wait_up(60, ready=False), s.output()[-800:]
            time.sleep(1)
            r = requests.get(f"{s.base}/c", timeout=10)
            assert r.status_code == 503, r.text
            body = r.text
            assert s.src not in body and "read_csv" not in body and "IO Error" not in body, body[:300]
            assert s.src not in requests.get(f"{s.base}/health", timeout=10).text
        finally:
            s.stop()


class TestAFailingScheduledRefreshDoesNotRetryEveryScan:

    def test_the_worker_backs_off_instead_of_hammering_a_dead_source(self):
        s = _Server(schedule="1h", worker_interval=1, with_source=False)
        try:
            assert s.wait_up(60, ready=False), s.output()[-800:]
            time.sleep(6)
            attempts = s.output().count("Failed scheduled cache refresh")
            assert attempts <= 2, f"{attempts} failed refreshes in ~6s with a 1h schedule: retry storm"
        finally:
            s.stop()
