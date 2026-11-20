"""Cache operations API found in the #211 exit gate (#218).

* a refresh skipped because one of the same table is already running answers
  409 + Retry-After (it answered 200 {"status":"already_running"}, which a
  script cannot tell from a refresh that ran);
* garbage collection names the OTHER table whose retained snapshot pins an old
  incarnation's files and reports the pinned bytes, instead of
  `nothing_to_expire` / 0 bytes;
* validating a populate template against an alias the server attached before
  the writer's atomic rename reports `stale`, not a bind error against the old
  file.
"""
import concurrent.futures
import os
import time

import pytest
import requests

from test_cache_catalog_f1 import (AUTH, FULL_POPULATE, SERVE, base_config, parquet_files, root,  # noqa: F401
                                   servers, write_source, write_tree)

pytestmark = pytest.mark.standalone_server


def endpoint(slug, table, retention=True):
    body = f"""\
url-path: /{slug}
method: GET
template-source: serve.sql
connection: [src]
cache:
  enabled: true
  table: {table}
  schema: main
  template-file: {slug}_cache.sql
"""
    if retention:
        body += "  retention:\n    keep-last-snapshots: 1\n"
    return body


# Roughly a second or two of CPU on a laptop: long enough for a second request
# to arrive while the first still runs.
SLOW_POPULATE = """\
CREATE OR REPLACE TABLE {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS
SELECT sum(hash(a.range * 100003 + b.range)) % 1000 AS id, 'slow' AS name, 1 AS version
FROM range(40000) a, range(40000) b
"""


class TestSkippedRefreshIs409:
    def test_a_second_refresh_of_the_same_table_is_409_with_retry_after(self, root, servers):  # noqa: F811
        write_tree(root, base_config(), {"slow.yaml": endpoint("slow", "slow_cache", retention=False),
                                         "slow_cache.sql": SLOW_POPULATE, "serve.sql": SERVE})
        write_source(root, [(1, "a", 1)])
        s = servers(root)
        url = f"{s.base}/api/v1/_config/endpoints/-slow/cache/refresh"
        with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
            first = pool.submit(lambda: requests.post(url, headers=AUTH, timeout=120))
            time.sleep(0.4)
            second = requests.post(url, headers=AUTH, timeout=120)
            first = first.result()
        assert first.status_code == 200 and first.json()["status"] == "refreshed", (first.text, s.output()[-2000:])
        assert second.status_code == 409, (second.status_code, second.text)
        body = second.json()
        assert body["status"] == "already_running", body
        assert body["endpoint"] == "/slow" and "full" in body, body
        assert int(second.headers["Retry-After"]) >= 1, dict(second.headers)
        # Once the first is done the next refresh runs again.
        assert requests.post(url, headers=AUTH, timeout=120).status_code == 200


class TestGcNamesThePinningTable:
    def test_another_tables_snapshot_pins_the_old_incarnation(self, root, servers):  # noqa: F811
        write_tree(root, base_config(), {
            "a.yaml": endpoint("a", "a_cache"), "a_cache.sql": FULL_POPULATE,
            # b keeps every snapshot (no retention): its snapshot sits between a's.
            "b.yaml": endpoint("b", "b_cache", retention=False), "b_cache.sql": FULL_POPULATE,
            "serve.sql": SERVE})
        write_source(root, [(1, "a", 1), (2, "b", 1)])
        s = servers(root)
        s.refresh("a")
        s.refresh("b")          # a's first file is live in this snapshot
        for _ in range(3):
            s.refresh("a")      # a's own retention keeps only the newest
        # a's first file is still on disk: b's snapshot pins it.
        assert len(parquet_files(root, "main", "a_cache")) >= 2, s.output()[-3000:]

        g = s.gc("a")
        assert g.status_code == 200, g.text
        body = g.json()
        assert body["expired_snapshots"] == 0, body
        assert body.get("pinned_bytes", 0) > 0, body
        assert any("/b" in holder and "b_cache" in holder for holder in body.get("pinned_by", [])), body
        assert "pinned" in body["message"] and "b_cache" in body["message"], body

    def test_nothing_pinned_reports_no_pinned_by(self, root, servers):  # noqa: F811
        write_tree(root, base_config(), {"a.yaml": endpoint("a", "a_cache"), "a_cache.sql": FULL_POPULATE,
                                         "serve.sql": SERVE})
        write_source(root, [(1, "a", 1)])
        s = servers(root)
        for _ in range(3):
            s.refresh("a")
        body = s.gc("a").json()
        assert body["status"] == "nothing_to_expire", body
        assert "pinned_by" not in body and body.get("pinned_bytes", 0) == 0, body


STALE_CONFIG = """\
project-name: stale-218
project-description: validation against a replaced source
template:
  path: ./sqls
connections:
  src:
    init: |
      ATTACH './source.duckdb' AS seed;
      CREATE OR REPLACE TABLE seed.orders AS SELECT range AS id FROM range(5);
      DETACH seed;
      ATTACH './next.duckdb' AS nx;
      CREATE OR REPLACE TABLE nx.orders AS SELECT range AS id, 'x' AS extra FROM range(5);
      DETACH nx;
      ATTACH './source.duckdb' AS src (READ_ONLY);
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

STALE_ENDPOINT = """\
url-path: /orders
method: GET
template-source: serve.sql
connection: [src]
cache:
  enabled: true
  table: orders_cache
  schema: main
  template-file: orders_cache.sql
"""

STALE_POPULATE = """\
DETACH DATABASE IF EXISTS src;
ATTACH '{{{conn.path}}}' AS src (READ_ONLY);
CREATE OR REPLACE TABLE {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS
SELECT id, extra FROM src.orders;
"""


class TestStaleAttachedAlias:
    def test_validate_only_after_an_atomic_rename_reports_stale(self, root, servers):  # noqa: F811
        write_tree(root, STALE_CONFIG, {"orders.yaml": STALE_ENDPOINT, "orders_cache.sql": STALE_POPULATE,
                                        "serve.sql": SERVE})
        s = servers(root)
        validate = f"{s.base}/api/v1/_config/endpoints/-orders/template/expand?target=cache&validate_only=1"

        # Before the rename the template really does not bind: an error, no `stale`.
        before = requests.post(validate, headers=AUTH, json={"parameters": {}}, timeout=60).json()
        assert before["valid"] is False and any("extra" in e["message"] for e in before["errors"]), before
        assert not any(w["type"] == "stale" for w in before.get("warnings", [])), before

        # The writer publishes by rename: the server's attach still holds the old inode.
        os.replace(os.path.join(root, "next.duckdb"), os.path.join(root, "source.duckdb"))

        after = requests.post(validate, headers=AUTH, json={"parameters": {}}, timeout=60).json()
        stale = [w for w in after.get("warnings", []) if w["type"] == "stale"]
        assert stale, after
        assert "src" in stale[0]["message"] and "replaced" in stale[0]["message"], stale
        # Not a failure: the refresh re-attaches and sees the new file.
        assert after["valid"] is True and not after["errors"], after
        r = requests.post(f"{s.base}/api/v1/_config/endpoints/-orders/cache/refresh", headers=AUTH, timeout=60)
        assert r.status_code == 200, r.text
