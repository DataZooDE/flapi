"""SQL / database layer hardening (#198 quoting + log leak, #212, #216, #217 DECIMAL).

Every test drives the real binary over real HTTP:

* `ducklake.alias: my-lake` and `schema: my-schema` (hyphens) boot, populate,
  audit and serve - identifiers flAPI builds itself are quoted;
* a debug-level server does not print a connection password;
* DuckDB's session TimeZone is UTC unless the config says otherwise, whatever
  the host `TZ` is (#212);
* a write to a SQLite connection issued during a slow cache populate over the
  same connection waits for the populate (#216);
* DECIMALs wider than a double holds come out exactly (#217).
"""
import os
import shutil
import signal
import sqlite3
import subprocess
import tempfile
import threading
import time
from datetime import datetime, timezone

import pytest
import requests

from otel_helpers import flapi_binary, free_port

pytestmark = pytest.mark.standalone_server

TOKEN = "sql-layer-token"
AUTH = {"Authorization": f"Bearer {TOKEN}"}
BASE_ENV = {**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"}


class Server:
    def __init__(self, root, config, files, env=None, log_level="info", args=()):
        self.root = root
        os.makedirs(os.path.join(root, "sqls"), exist_ok=True)
        with open(os.path.join(root, "flapi.yaml"), "w") as f:
            f.write(config)
        for name, body in files.items():
            with open(os.path.join(root, "sqls", name), "w") as f:
                f.write(body)
        self.port = free_port()
        self.base = f"http://127.0.0.1:{self.port}"
        self.log_path = os.path.join(root, "server.log")
        self.log = open(self.log_path, "w")
        self.proc = subprocess.Popen(
            [flapi_binary(), "-c", "flapi.yaml", "-p", str(self.port), "--log-level", log_level,
             "--config-service", "--config-service-token", TOKEN, *args],
            cwd=root, stdout=self.log, stderr=subprocess.STDOUT, preexec_fn=os.setsid,
            env={**BASE_ENV, **(env or {})},
        )
        deadline = time.time() + 90
        while time.time() < deadline:
            if self.proc.poll() is not None:
                pytest.fail("server exited:\n" + self.output()[-4000:])
            try:
                if requests.get(f"{self.base}/health/live", timeout=1).status_code == 200:
                    return
            except requests.RequestException:
                time.sleep(0.3)
        pytest.fail("server never became live:\n" + self.output()[-4000:])

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


@pytest.fixture
def root():
    d = tempfile.mkdtemp(prefix="flapi_sqllayer_")
    yield d
    shutil.rmtree(d, ignore_errors=True)


@pytest.fixture
def start(root):
    started = []

    def go(config, files, **kw):
        s = Server(root, config, files, **kw)
        started.append(s)
        return s

    yield go
    for s in started:
        s.stop()


# ---------------------------------------------------------------- #198 quoting

QUOTING_CONFIG = """\
project-name: quoting
project-description: hyphenated identifiers
template:
  path: ./sqls
connections:
  src:
    properties:
      path: ./source.csv
ducklake:
  enabled: true
  alias: my-lake
  metadata-path: ./cache.ducklake
  data-path: ./cache_data
  warmup: never
rate_limit:
  enabled: false
"""

QUOTING_FILES = {
    "items.yaml": """\
url-path: /items
method: GET
template-source: serve.sql
connection: [src]
cache:
  enabled: true
  table: items_cache
  schema: my-schema
  template-file: populate.sql
""",
    "serve.sql": 'SELECT * FROM "{{{cache.catalog}}}"."{{{cache.schema}}}"."{{{cache.table}}}" ORDER BY id',
    "populate.sql": ('CREATE OR REPLACE TABLE "{{{cache.catalog}}}"."{{{cache.schema}}}"."{{{cache.table}}}" AS '
                     "SELECT * FROM read_csv('{{{conn.path}}}')"),
}


def test_hyphenated_alias_and_schema_boot_refresh_and_serve(root, start):
    with open(os.path.join(root, "source.csv"), "w") as f:
        f.write("id,name\n1,a\n2,b\n")
    s = start(QUOTING_CONFIG, QUOTING_FILES)
    log = s.output()
    assert "Failed to initialize audit tables" not in log, log[-3000:]
    r = requests.post(f"{s.base}/api/v1/_config/endpoints/-items/cache/refresh", headers=AUTH, timeout=60)
    assert r.status_code == 200, f"{r.status_code} {r.text}\n{s.output()[-3000:]}"
    assert "Failed to create cache schema" not in s.output()
    r = requests.get(f"{s.base}/items", timeout=10)
    assert r.status_code == 200 and len(r.json()["data"]) == 2, r.text
    audit = requests.get(f"{s.base}/api/v1/_config/endpoints/-items/cache/audit", headers=AUTH, timeout=30)
    assert audit.status_code == 200 and "success" in audit.text, audit.text


# ------------------------------------------------------------- #198 log secret

def test_debug_log_does_not_print_a_connection_password(root, start):
    secret = "SECRET123-probe"
    config = f"""\
project-name: leak
project-description: debug log hygiene
template:
  path: ./sqls
connections:
  src:
    properties:
      path: ./source.csv
      password: {secret}
rate_limit:
  enabled: false
"""
    files = {
        "q.yaml": "url-path: /q\nmethod: GET\ntemplate-source: q.sql\nconnection: [src]\n",
        "q.sql": "SELECT * FROM read_csv('{{{conn.path}}}')",
    }
    with open(os.path.join(root, "source.csv"), "w") as f:
        f.write("id\n1\n")
    s = start(config, files, log_level="debug")
    assert requests.get(f"{s.base}/q", timeout=10).status_code == 200
    out = s.output()
    assert secret not in out, "debug logging printed a connection password:\n" + \
        "\n".join(l for l in out.splitlines() if secret in l)[:1500]


# ------------------------------------------------------------------ #212 zone

TZ_CONFIG = """\
project-name: tz
project-description: session time zone
template:
  path: ./sqls
connections:
  mem:
    properties:
      x: y
{duckdb}
rate_limit:
  enabled: false
"""

TZ_FILES = {
    "now.yaml": "url-path: /now\nmethod: GET\ntemplate-source: now.sql\nconnection: [mem]\n",
    # now() is TIMESTAMPTZ: the cast to TIMESTAMP goes through ICU and the session zone.
    "now.sql": "SELECT now()::TIMESTAMP::VARCHAR AS local_ts, current_setting('TimeZone') AS zone",
}


def _fetch_now(s):
    # The first query autoloads ICU (current_setting('TimeZone') needs it) and
    # picks up the host zone; the SECOND is the one that shows the drift.
    requests.get(f"{s.base}/now", timeout=20)
    r = requests.get(f"{s.base}/now", timeout=20)
    assert r.status_code == 200, r.text
    return r.json()["data"][0]


def test_timezone_defaults_to_utc_regardless_of_host_tz(root, start):
    s = start(TZ_CONFIG.format(duckdb=""), TZ_FILES, env={"TZ": "Asia/Tokyo"})
    row = _fetch_now(s)
    got = datetime.fromisoformat(row["local_ts"].split(".")[0]).replace(tzinfo=timezone.utc)
    drift = abs((datetime.now(timezone.utc) - got).total_seconds())
    assert drift < 120, f"now()::TIMESTAMP drifted {drift / 3600:.1f}h from UTC: {row}"
    assert row["zone"] == "UTC", row
    assert "DuckDB TimeZone: UTC" in s.output(), s.output()[-2000:]  # effective zone logged at startup


@pytest.mark.parametrize("key", ["TimeZone", "timezone"])
def test_user_timezone_is_honoured_case_insensitively(root, start, key):
    s = start(TZ_CONFIG.format(duckdb=f"duckdb:\n  {key}: Europe/Berlin\n"), TZ_FILES, env={"TZ": "Asia/Tokyo"})
    row = _fetch_now(s)
    assert row["zone"] == "Europe/Berlin", row
    got = datetime.fromisoformat(row["local_ts"].split(".")[0]).replace(tzinfo=timezone.utc)
    offset_h = (got - datetime.now(timezone.utc)).total_seconds() / 3600
    assert 0.9 < offset_h < 2.1, f"expected Berlin wall clock (+1/+2h), got {offset_h:+.2f}h: {row}"


# ------------------------------------------------------------------- #216 lock

def test_sqlite_write_waits_for_a_slow_populate_on_the_same_connection(root, start):
    db_path = os.path.join(root, "shop.sqlite")
    marker = os.path.join(root, "populate_started.csv")
    marker_end = os.path.join(root, "populate_done.csv")
    con = sqlite3.connect(db_path)
    con.execute("CREATE TABLE products (id INTEGER PRIMARY KEY, name TEXT)")
    con.executemany("INSERT INTO products (name) VALUES (?)", [(f"p{i}",) for i in range(20)])
    con.commit()
    con.close()

    config = f"""\
project-name: lock
project-description: populate takes the source lock
template:
  path: ./sqls
connections:
  shop:
    init: |
      INSTALL sqlite;
      LOAD sqlite;
      ATTACH IF NOT EXISTS '{db_path}' AS shop (TYPE sqlite);
ducklake:
  enabled: true
  alias: lake
  metadata-path: ./cache.ducklake
  data-path: ./cache_data
  warmup: never
rate_limit:
  enabled: false
"""
    files = {
        "cached.yaml": ("url-path: /cached\nmethod: GET\ntemplate-source: cached.sql\nconnection: [shop]\n"
                        "cache:\n  enabled: true\n  table: products_cache\n  template-file: populate.sql\n"),
        "cached.sql": "SELECT * FROM {{{cache.catalog}}}.{{{cache.schema}}}.{{{cache.table}}} ORDER BY id",
        # Reads the SQLite table, then burns time in DuckDB while still "populating".
        # The first statement drops a marker file, so the test knows the populate
        # (and the lock it holds) is running instead of guessing with a sleep.
        "populate.sql": ("COPY (SELECT 1 AS x) TO '" + marker + "' (FORMAT csv); "
                         "CREATE OR REPLACE TABLE {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS "
                         "SELECT p.id, p.name, s.h FROM shop.products p, "
                         "(SELECT sum(hash(range)) AS h FROM range(__N__)) s; "
                         "COPY (SELECT 1 AS x) TO '" + marker_end + "' (FORMAT csv);"),
        "write.yaml": ("url-path: /products\nmethod: POST\n"
                       "operation:\n  type: write\n  returns-data: false\n  transaction: true\n"
                       "request:\n  - field-name: name\n    field-in: body\n    required: true\n"
                       "template-source: write.sql\nconnection: [shop]\n"),
        "write.sql": "INSERT INTO shop.products (name) VALUES ('{{{ params.name }}}');",
    }
    # Calibrate the populate to take a few seconds on this machine.
    files["populate.sql"] = files["populate.sql"].replace("__N__", "1500000000")
    s = start(config, files)

    refresh = {}

    def do_refresh():
        t0 = time.time()
        r = requests.post(f"{s.base}/api/v1/_config/endpoints/-cached/cache/refresh", headers=AUTH, timeout=300)
        refresh.update(status=r.status_code, text=r.text, end=time.time(), start=t0)

    t = threading.Thread(target=do_refresh)
    t.start()
    deadline = time.time() + 30
    while not os.path.exists(marker) and time.time() < deadline:
        time.sleep(0.02)
    assert os.path.exists(marker), "the populate never started\n" + s.output()[-2000:]
    w = requests.post(f"{s.base}/products", json={"name": "during-populate"}, timeout=300)
    write_end = time.time()
    t.join(timeout=300)

    assert refresh["status"] == 200, refresh
    duration = refresh["end"] - refresh["start"]
    assert duration > 2.5, f"populate too fast ({duration:.1f}s) to prove anything; raise the range"
    assert w.status_code < 500, f"write got {w.status_code}: {w.text}\n{s.output()[-2000:]}"
    # The populate's last statement writes `marker_end` while it still holds the
    # lock; the HTTP response of the refresh arrives later, so it is the wrong clock.
    populate_done = os.path.getmtime(marker_end)
    assert write_end >= populate_done - 0.05, (
        f"the write completed {populate_done - write_end:.1f}s BEFORE the populate it overlapped")


# ------------------------------------------------------------------ #217 DECIMAL

def test_wide_decimals_are_serialised_exactly(root, start):
    config = """\
project-name: dec
project-description: decimals
template:
  path: ./sqls
connections:
  mem:
    properties:
      x: y
rate_limit:
  enabled: false
"""
    files = {
        "d.yaml": "url-path: /d\nmethod: GET\ntemplate-source: d.sql\nconnection: [mem]\n",
        "d.sql": ("SELECT 12345678901234567890.123456789::DECIMAL(38,9) AS wide, "
                  "1.1234567::DECIMAL(10,7) AS scale7, 7790.62::DECIMAL(10,2) AS narrow"),
    }
    s = start(config, files)
    text = requests.get(f"{s.base}/d", timeout=10).text
    # Exact digits, as a JSON number or - the documented fallback - as a string.
    assert ('"wide":12345678901234567890.123456789' in text
            or '"wide":"12345678901234567890.123456789"' in text), text
    assert ('"scale7":1.1234567' in text or '"scale7":"1.1234567"' in text), text
    assert '"narrow":7790.62' in text, text
