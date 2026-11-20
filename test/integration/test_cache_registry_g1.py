"""Cache registry, cache routes and maintenance (#211, exit gate G1).

Each test starts its own server on a throwaway DuckLake catalog:

* the readiness registry follows endpoint reloads: a changed `cache.table` is
  503 cache_not_built (not 500), and reverting it clears the stale entry from
  /health (it stayed until restart);
* reload-file of a deleted endpoint YAML unloads what it declared;
* /cache/* routes and the one-shot pick the CACHED endpoint when a cached GET
  and an uncached POST share a path; `--method` narrows it;
* POST /api/v1/_config/cache/compact runs compaction now;
* the audit log, metrics and the refresh answer say whether a refresh loaded
  the whole source (`full`);
* time travel to a snapshot before the cache's schema existed is a 404;
* `validate_only` of a cache template executes its READ_ONLY ATTACH and leaves
  nothing attached;
* a read-only replica says the WRITER has not built a cache, not "warm-up
  skipped".
"""
import os
import subprocess
import time

import pytest
import requests

from otel_helpers import flapi_binary
from test_cache_catalog_f1 import (  # noqa: F401  (fixtures)
    AUTH, ENV, SERVE, base_config, root, servers, write_source, write_tree,
)

pytestmark = pytest.mark.standalone_server

POPULATE = """\
CREATE OR REPLACE TABLE {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS
SELECT * FROM read_csv('{{{conn.path}}}')
"""


def endpoint(table="full_cache", schema="main", path="/full", extra=""):
    return f"""\
url-path: {path}
method: GET
template-source: serve.sql
connection: [src]
cache:
  enabled: true
  table: {table}
  schema: {schema}
  template-file: full_cache.sql
{extra}"""


def health(s):
    r = requests.get(f"{s.base}/health", timeout=10)
    return r.status_code, r.json()


def reload_file(s, name):
    return requests.post(f"{s.base}/api/v1/_config/endpoints/reload-file", headers=AUTH,
                         json={"file": f"sqls/{name}"}, timeout=30)


class TestRegistryFollowsReloads:
    def test_changed_table_is_unbuilt_and_reverting_clears_the_stale_entry(self, root, servers):
        write_tree(root, base_config(), {"full.yaml": endpoint(), "full_cache.sql": POPULATE, "serve.sql": SERVE})
        write_source(root, [(1, "a", 1)])
        s = servers(root)
        s.refresh("full")
        assert health(s) == (200, health(s)[1]) and health(s)[1]["status"] == "ready"

        # Point the endpoint at a new table: not built yet, so 503 cache_not_built.
        with open(os.path.join(root, "sqls", "full.yaml"), "w") as f:
            f.write(endpoint(table="other_cache"))
        assert reload_file(s, "full.yaml").status_code == 200
        r = requests.get(f"{s.base}/full", timeout=10)
        assert r.status_code == 503, r.text
        assert r.json()["error"] == "cache_not_built"
        code, body = health(s)
        assert code == 200 and body["status"] == "degraded", body
        assert body["caches"]["total"] == 1, body   # the old table's entry is gone
        assert [c["table"] for c in body["unbuilt"]] == ["other_cache"]

        # A failed build of the new table: /health names it.
        with open(os.path.join(root, "sqls", "full_cache.sql"), "w") as f:
            f.write("CREATE OR REPLACE TABLE {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS "
                    "SELECT * FROM read_csv('./missing.csv')")
        bad = requests.post(f"{s.base}/api/v1/_config/endpoints/-full/cache/refresh", headers=AUTH, timeout=30)
        assert bad.status_code == 400, bad.text
        code, body = health(s)
        assert code == 503 and [c["table"] for c in body["failed"]] == ["other_cache"], body

        # Revert: the old table exists, so the endpoint serves again and /health
        # no longer carries the failed entry (it did, total:2, until restart).
        with open(os.path.join(root, "sqls", "full_cache.sql"), "w") as f:
            f.write(POPULATE)
        with open(os.path.join(root, "sqls", "full.yaml"), "w") as f:
            f.write(endpoint())
        assert reload_file(s, "full.yaml").status_code == 200
        code, body = health(s)
        assert code == 200 and body["status"] == "ready" and body["caches"]["total"] == 1, body
        assert requests.get(f"{s.base}/full", timeout=10).status_code == 200
        s.refresh("full")
        code, body = health(s)
        assert code == 200 and body["caches"]["total"] == 1, body

    def test_reload_of_a_deleted_file_unloads_its_endpoints(self, root, servers):
        write_tree(root, base_config(), {"full.yaml": endpoint(), "full_cache.sql": POPULATE, "serve.sql": SERVE})
        write_source(root, [(1, "a", 1)])
        s = servers(root)
        s.refresh("full")
        os.remove(os.path.join(root, "sqls", "full.yaml"))
        r = reload_file(s, "full.yaml")
        assert r.status_code == 200, r.text
        assert r.json()["status"] == "removed" and r.json()["removed"] == ["GET /full"], r.text
        assert requests.get(f"{s.base}/full", timeout=10).status_code == 404
        code, body = health(s)
        assert code == 200 and body["caches"]["total"] == 0, body
        # Nothing declared from it any more: a second reload is a 404.
        assert reload_file(s, "full.yaml").status_code == 404


ORDERS_GET = endpoint(table="orders_cache", path="/orders")
ORDERS_POST = """\
url-path: /orders
method: POST
template-source: serve_post.sql
connection: [src]
"""


class TestCachedEndpointIsChosen:
    def test_cache_routes_and_one_shot_pick_the_cached_endpoint(self, root, servers):
        write_tree(root, base_config(), {"orders_get.yaml": ORDERS_GET, "orders_post.yaml": ORDERS_POST,
                                         "full_cache.sql": POPULATE, "serve.sql": SERVE,
                                         "serve_post.sql": "SELECT 1 AS ok"})
        write_source(root, [(1, "a", 1)])
        one = subprocess.run([flapi_binary(), "cache", "refresh", "--endpoint", "/orders"], cwd=root, env=ENV,
                             capture_output=True, text=True, timeout=120)
        assert one.returncode == 0, one.stdout + one.stderr
        no_cache = subprocess.run([flapi_binary(), "cache", "refresh", "--endpoint", "/orders", "--method", "POST"],
                                  cwd=root, env=ENV, capture_output=True, text=True, timeout=120)
        assert no_cache.returncode == 2 and "has no cache" in no_cache.stdout, no_cache.stdout

        s = servers(root)
        assert s.refresh("orders")["status"] == "refreshed"
        for route in ("cache", "cache/audit"):
            r = requests.get(f"{s.base}/api/v1/_config/endpoints/-orders/{route}", headers=AUTH, timeout=10)
            assert r.status_code == 200, (route, r.text)
        assert s.gc("orders").status_code == 200
        # The non-cache routes still call it ambiguous.
        r = requests.get(f"{s.base}/api/v1/_config/endpoints/-orders", headers=AUTH, timeout=10)
        assert r.status_code == 400 and "Ambiguous" in r.text, r.text


class TestCompactNow:
    def test_compact_catalog_and_one_table(self, root, servers):
        write_tree(root, base_config(), {"full.yaml": endpoint(), "full_cache.sql": POPULATE, "serve.sql": SERVE})
        write_source(root, [(1, "a", 1)])
        s = servers(root)
        for _ in range(3):
            s.refresh("full")
        url = f"{s.base}/api/v1/_config/cache/compact"
        assert requests.post(url, timeout=10).status_code == 401
        r = requests.post(url, headers=AUTH, timeout=60)
        assert r.status_code == 200, r.text
        body = r.json()
        assert body["status"] == "compacted" and body["duration_ms"] >= 0, body
        r = requests.post(url, headers=AUTH, json={"endpoint": "/full"}, timeout=60)
        assert r.status_code == 200, r.text
        assert r.json()["table"] == "full_cache" and r.json()["schema"] == "main", r.text
        assert requests.post(url, headers=AUTH, json={"endpoint": "/nope"}, timeout=10).status_code == 404

        audit = requests.get(f"{s.base}/api/v1/_config/cache/audit", headers=AUTH,
                             params={"sync_type": "compaction"}, timeout=10).json()
        catalog_wide = [e for e in audit if e["cache_table"] == ""]
        assert catalog_wide and all(e["cache_schema"] == "" for e in catalog_wide), audit
        assert any(e["cache_table"] == "full_cache" and e["cache_schema"] == "main" for e in audit), audit
        assert requests.get(f"{s.base}/full", timeout=10).status_code == 200


MERGE = endpoint(table="merge_cache", path="/merge", extra="""\
  primary-key: [id]
  cursor:
    column: version
    type: integer
""").replace("full_cache.sql", "merge_cache.sql")
MERGE_POPULATE = """\
{{^cache.previousSnapshotTimestamp}}
CREATE TABLE IF NOT EXISTS {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS
SELECT * FROM read_csv('{{{conn.path}}}') WHERE false;
{{/cache.previousSnapshotTimestamp}}
MERGE INTO {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS t
USING (SELECT * FROM read_csv('{{{conn.path}}}')
  {{#cache.previousSnapshotTimestamp}}WHERE version > {{cache.previousSnapshotTimestamp}}{{/cache.previousSnapshotTimestamp}}
) AS s ON t.id = s.id
WHEN MATCHED THEN UPDATE SET name = s.name, version = s.version
WHEN NOT MATCHED THEN INSERT *;
"""


class TestFullFlag:
    def test_full_is_recorded_for_bootstrap_and_forced_rebuilds(self, root, servers):
        write_tree(root, base_config(), {"merge.yaml": MERGE, "merge_cache.sql": MERGE_POPULATE,
                                         "serve.sql": SERVE})
        write_source(root, [(1, "a", 1)])
        s = servers(root)
        assert s.refresh("merge")["full"] is True        # bootstrap: no watermark yet
        write_source(root, [(1, "a", 1), (2, "b", 2)])
        assert s.refresh("merge")["full"] is False       # incremental
        assert s.refresh("merge", full=True)["full"] is True
        audit = requests.get(f"{s.base}/api/v1/_config/endpoints/-merge/cache/audit", headers=AUTH,
                             timeout=10).json()
        flags = [e["full"] for e in audit if e["sync_type"] == "merge"]
        assert flags == [True, False, True], audit        # newest first
        metrics = requests.get(f"{s.base}/api/v1/_config/metrics", headers=AUTH, timeout=10).json()
        rows = [c for c in metrics["caches"] if c["cache"].endswith("merge_cache")]
        assert rows and rows[0]["last"]["full"] is True, metrics["caches"]


TIME_TRAVEL = """\
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
  table: tt_cache
  schema: sales
  template-file: full_cache.sql
"""


class TestTimeTravelBeforeSchema:
    def test_snapshot_before_the_schema_existed_is_404(self, root, servers):
        write_tree(root, base_config(), {
            "tt.yaml": TIME_TRAVEL, "full_cache.sql": POPULATE,
            "tt.sql": "SELECT * FROM {{{cache.catalog}}}.{{{cache.schema}}}.{{{cache.table}}} "
                      "AT (VERSION => {{ params.version }}) ORDER BY id"})
        write_source(root, [(1, "a", 1)])
        s = servers(root)
        answer = s.refresh("tt")
        assert requests.get(f"{s.base}/tt", params={"version": answer["snapshot_id"]}, timeout=10).status_code == 200
        r = requests.get(f"{s.base}/tt", params={"version": 0}, timeout=10)
        assert r.status_code == 404, r.text + s.output()[-1500:]
        assert r.json()["error"] == "snapshot_not_found"


TIME_TRAVEL_TS = TIME_TRAVEL.replace(
    "  - field-name: version\n    field-in: query\n    required: true\n    validators:\n      - type: int\n        min: 0\n",
    "  - field-name: version\n    field-in: query\n    required: false\n    validators:\n      - type: int\n        min: 0\n"
    "  - field-name: ts\n    field-in: query\n    required: false\n    validators:\n      - type: string\n"
    "        pattern: \"^[0-9]{4}-[0-9]{2}-[0-9]{2}( [0-9:]+)?$\"\n"
    "  - field-name: day\n    field-in: query\n    required: false\n    validators:\n      - type: date\n")


class TestTimeTravelParametersStayLiterals:
    """AT (VERSION|TIMESTAMP => ...) is resolved before parameters are bound (#217), so a
    validated value there is rendered as a literal, never turned into a `?`."""

    FUTURE = "2100-01-01 00:00:00"

    @pytest.mark.parametrize("clause,params", [
        ("AT (TIMESTAMP => {{ params.day }})", {"day": "2100-01-01"}),
        ("AT (TIMESTAMP => CAST('{{{ params.ts }}}' AS TIMESTAMP))", {"ts": FUTURE}),
        ("AT (TIMESTAMP => '{{{ params.ts }}}')", {"ts": FUTURE}),
        ("AT (TIMESTAMP => TIMESTAMP '{{{ params.ts }}}')", {"ts": FUTURE}),
        ("AT (VERSION => {{{ params.version }}})", "snapshot"),
        ("AT (VERSION => {{ params.version }})", "snapshot"),
    ], ids=["ts-bare-date", "ts-triple-cast", "ts-triple-quoted", "ts-typed-literal",
            "version-triple", "version-double"])
    def test_a_time_travel_value_is_a_validated_literal(self, root, servers, clause, params):
        write_tree(root, base_config(), {
            "tt.yaml": TIME_TRAVEL_TS, "full_cache.sql": POPULATE,
            "tt.sql": "SELECT * FROM {{{cache.catalog}}}.{{{cache.schema}}}.{{{cache.table}}} "
                      + clause + " ORDER BY id"})
        write_source(root, [(1, "a", 1)])
        s = servers(root)
        answer = s.refresh("tt")
        query = {"version": answer["snapshot_id"]} if params == "snapshot" else params
        r = requests.get(f"{s.base}/tt", params=query, timeout=10)
        assert r.status_code == 200, r.text + s.output()[-1500:]
        assert len(r.json()["data"]) == 1, r.text


ATTACH_CONFIG_EXTRA = """\
connections:
  src:
    init: |
      ATTACH './source.duckdb' AS seed;
      CREATE OR REPLACE TABLE seed.orders AS SELECT range AS id, range % 3 AS v FROM range(10);
      DETACH seed;
    properties:
      path: ./source.duckdb
"""
ATTACH_POPULATE = """\
DETACH DATABASE IF EXISTS src;
ATTACH '{{{conn.path}}}' AS src (READ_ONLY);
CREATE OR REPLACE TABLE {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS SELECT * FROM src.orders;
"""
DATABASES = """\
url-path: /databases
method: GET
template-source: databases.sql
connection: [src]
"""


def attach_config():
    config = base_config()
    head, _, tail = config.partition("connections:\n  src:\n    properties:\n      path: ./source.csv\n")
    return head + ATTACH_CONFIG_EXTRA + tail


class TestValidateAttach:
    def _validate(self, s):
        r = requests.post(f"{s.base}/api/v1/_config/endpoints/-full/template/expand?target=cache&validate_only=1",
                          headers=AUTH, json={"parameters": {}}, timeout=30)
        assert r.status_code == 200, r.text
        return r.json()

    def _attached(self, s):
        r = requests.get(f"{s.base}/databases", timeout=10)
        assert r.status_code == 200, r.text
        body = r.json()
        rows = body["data"] if isinstance(body, dict) else body
        return {row["database_name"] for row in rows}

    def test_read_only_attach_is_executed_and_detached_again(self, root, servers):
        write_tree(root, attach_config(), {
            "full.yaml": endpoint(), "full_cache.sql": ATTACH_POPULATE, "serve.sql": SERVE,
            "databases.yaml": DATABASES, "databases.sql": "SELECT database_name FROM duckdb_databases()"})
        s = servers(root)
        result = self._validate(s)
        assert result["valid"] is True, result
        assert result["errors"] == [], result
        assert not [w for w in result["warnings"] if "src" in w["message"]], result
        assert "src" not in self._attached(s)
        # The refresh the template was validated for still works.
        s.refresh("full")
        assert requests.get(f"{s.base}/full", timeout=10).status_code == 200

    def test_writable_attach_is_not_executed_and_said_so(self, root, servers):
        write_tree(root, attach_config(), {
            "full.yaml": endpoint(), "full_cache.sql": ATTACH_POPULATE.replace(" (READ_ONLY)", ""),
            "serve.sql": SERVE, "databases.yaml": DATABASES,
            "databases.sql": "SELECT database_name FROM duckdb_databases()"})
        s = servers(root)
        result = self._validate(s)
        assert result["errors"] == [], result
        messages = " ".join(w["message"] for w in result["warnings"])
        assert "without READ_ONLY" in messages and "was not checked" in messages, result
        assert "src" not in self._attached(s)


class TestReplicaWording:
    def test_unbuilt_on_a_replica_names_the_writer(self, root, servers):
        write_tree(root, base_config(), {"full.yaml": endpoint(), "full_cache.sql": POPULATE, "serve.sql": SERVE})
        write_source(root, [(1, "a", 1)])
        w = servers(root)   # creates the catalog, builds nothing (warmup: never)
        w.stop()
        with open(os.path.join(root, "flapi.yaml"), "w") as f:
            f.write(base_config("  read-only: true"))
        r = servers(root)
        answer = requests.get(f"{r.base}/full", timeout=10)
        assert answer.status_code == 503, answer.text
        body = answer.json()
        assert body["error"] == "cache_not_built"
        assert "writer has not built it yet" in body["message"] and "warm-up skipped" not in body["message"], body
        log = r.output()
        assert "attached read-only" in log
