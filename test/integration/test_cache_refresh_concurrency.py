"""Concurrent cache refreshes over one DuckDB file (#211, exit gate G1).

Three caches whose populate templates each do

    DETACH DATABASE IF EXISTS src;
    ATTACH '<file>' AS src (READ_ONLY);

- the recipe the caching skill taught - used to run in parallel when two
refresh requests for DIFFERENT tables arrived together. DuckDB attach names
are instance-wide, so the second ATTACH hit

    INTERNAL Error: Database name src was already used by a different
    database for this meta transaction

which INVALIDATES the whole database: every later request answered
"FATAL Error: database has been invalidated" until the process restarted,
and /health kept saying 200.

Refreshes are now serialised per DuckLake catalog, so the same templates run
one after another and all succeed.
"""
import concurrent.futures
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

TOKEN = "refresh-concurrency-token"
AUTH = {"Authorization": f"Bearer {TOKEN}"}

CONFIG = """\
project-name: refresh-concurrency-test
project-description: concurrent refreshes sharing an ATTACH alias
template:
  path: ./sqls
connections:
  src:
    init: |
      ATTACH './source.duckdb' AS seed;
      CREATE OR REPLACE TABLE seed.orders AS SELECT range AS id, range % 7 AS v FROM range(20000);
      CREATE OR REPLACE TABLE seed.customers AS SELECT range AS id, 'c' || range AS name FROM range(5000);
      CREATE OR REPLACE TABLE seed.items AS SELECT range AS id, range * 2 AS price FROM range(10000);
      DETACH seed;
    properties:
      path: ./source.duckdb
ducklake:
  enabled: true
  alias: cache
  metadata-path: ./cache.ducklake
  data-path: ./cache_data
  warmup: never
rate_limit:
  enabled: false
"""

SERVE = "SELECT count(*) AS n FROM {{{cache.catalog}}}.{{{cache.schema}}}.{{{cache.table}}}"


def populate(source_table):
    # A deliberately slow-ish body (a join over range) so two refreshes overlap.
    return f"""\
DETACH DATABASE IF EXISTS src;
ATTACH '{{{{{{conn.path}}}}}}' AS src (READ_ONLY);
CREATE OR REPLACE TABLE {{{{cache.catalog}}}}.{{{{cache.schema}}}}.{{{{cache.table}}}} AS
SELECT s.* FROM src.{source_table} s JOIN (SELECT range AS r FROM range(200)) x ON s.id % 200 = x.r;
"""


TABLES = ["orders", "customers", "items"]


def endpoint_yaml(name):
    return f"""\
url-path: /{name}
method: GET
template-source: serve.sql
connection: [src]
cache:
  enabled: true
  table: {name}_cache
  schema: main
  template-file: {name}_cache.sql
"""


@pytest.fixture
def server():
    root = tempfile.mkdtemp(prefix="flapi_refresh_conc_")
    os.makedirs(os.path.join(root, "sqls"))
    with open(os.path.join(root, "flapi.yaml"), "w") as f:
        f.write(CONFIG)
    with open(os.path.join(root, "sqls", "serve.sql"), "w") as f:
        f.write(SERVE)
    for name in TABLES:
        with open(os.path.join(root, "sqls", f"{name}.yaml"), "w") as f:
            f.write(endpoint_yaml(name))
        with open(os.path.join(root, "sqls", f"{name}_cache.sql"), "w") as f:
            f.write(populate(name))

    port = free_port()
    log_path = os.path.join(root, "server.log")
    log = open(log_path, "w")
    proc = subprocess.Popen(
        [flapi_binary(), "-c", "flapi.yaml", "-p", str(port), "--log-level", "info",
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


def refresh(s, name):
    return requests.post(f"{s.base}/api/v1/_config/endpoints/-{name}/cache/refresh",
                         headers=AUTH, timeout=120)


def log_tail(s):
    return open(s.log_path).read()[-4000:]


class TestConcurrentRefreshSharedAlias:
    def test_concurrent_refreshes_of_different_tables_all_succeed(self, server):
        # Several rounds: one round can get lucky with the timing. Failures are
        # collected, not asserted one by one, so the report shows every form the
        # collision took (attach-name clash, invalidation).
        failures = []
        started = time.time()
        for _round in range(5):
            with concurrent.futures.ThreadPoolExecutor(max_workers=len(TABLES)) as pool:
                answers = list(pool.map(lambda n: refresh(server, n), TABLES))
            for name, r in zip(TABLES, answers):
                if r.status_code != 200:
                    failures.append(f"{name}: {r.status_code} {r.text[:300]}")
        elapsed = time.time() - started
        assert not failures, "\n".join(failures) + "\n" + log_tail(server)
        # Serialised, not slower than running them back to back would be by
        # much: 15 refreshes of a few hundred ms each.
        assert elapsed < 60, f"15 serialised refreshes took {elapsed:.1f}s"

        assert "invalidated" not in open(server.log_path).read(), log_tail(server)

        # The database is still usable: every cache serves, and health is ready.
        expected = {"orders": 20000, "customers": 5000, "items": 10000}
        for name in TABLES:
            r = requests.get(f"{server.base}/{name}", timeout=30)
            assert r.status_code == 200, f"{name}: {r.status_code} {r.text}\n{log_tail(server)}"
            assert r.json()["data"][0]["n"] == expected[name]
        health = requests.get(f"{server.base}/health", timeout=10)
        assert health.status_code == 200, health.text
        assert health.json()["status"] == "ready", health.text
        assert requests.get(f"{server.base}/health/live", timeout=10).status_code == 200

    def test_same_table_duplicates_still_coalesce(self, server):
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            answers = list(pool.map(lambda _n: refresh(server, "orders"), range(4)))
        codes = sorted(r.status_code for r in answers)
        # At least one ran; the duplicates that arrived while it ran were
        # coalesced: answered 409 + Retry-After, not run again (#218).
        assert set(codes) <= {200, 409}, [(r.status_code, r.text) for r in answers]
        bodies = [r.json() for r in answers if r.status_code == 200]
        assert any(b.get("status") == "refreshed" for b in bodies), [(r.status_code, r.text) for r in answers]
        # A skipped refresh is never a 200 (it used to be 200 {"status":"already_running"}).
        assert all(b.get("status") == "refreshed" for b in bodies), [(r.status_code, r.text) for r in answers]
        for r in answers:
            if r.status_code == 409:
                assert r.json()["status"] == "already_running", r.text
                assert int(r.headers["Retry-After"]) >= 1, dict(r.headers)
