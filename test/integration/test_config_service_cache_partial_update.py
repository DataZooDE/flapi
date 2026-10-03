"""PUT /api/v1/_config/endpoints/{path}/cache with a partial body must not disable caching.

`enabled` was read unconditionally (a missing key reads as false), so
`{"schedule": "10m"}` switched the cache OFF - found when the CLI's
`cache update --ttl` (which sends no `enabled`) did exactly that.
"""

import os
import subprocess
import tempfile
import time

import pytest
import requests

from otel_helpers import flapi_binary, free_port

TOKEN = "cache-partial-token"


def _w(base, rel, text):
    path = os.path.join(base, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(text)


@pytest.fixture(scope="module")
def server():
    tmp = tempfile.mkdtemp(prefix="flapi_cachepartial_")
    port = free_port()
    _w(tmp, "flapi.yaml",
       "project-name: p\nproject-description: d\ntemplate:\n  path: ./sqls\n"
       "connections:\n  inmem:\n    properties:\n      database: ':memory:'\n"
       "ducklake:\n  enabled: true\n  alias: cache\n  metadata-path: ./cache.ducklake\n  data-path: ./cache\n")
    _w(tmp, "sqls/c.yaml",
       "url-path: /c\nmethod: GET\ntemplate-source: c.sql\nconnection: [inmem]\n"
       "cache:\n  enabled: true\n  table: c_cache\n  schedule: 5m\n")
    _w(tmp, "sqls/c.sql", "SELECT 1 AS n\n")
    proc = subprocess.Popen(
        [flapi_binary(), "-c", os.path.join(tmp, "flapi.yaml"), "-p", str(port),
         "--config-service", "--config-service-token", TOKEN, "--log-level", "warning"],
        stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT, cwd=tmp,
        env={**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"})
    base = f"http://127.0.0.1:{port}"
    for _ in range(300):
        try:
            if requests.get(f"{base}/health/live", timeout=2).status_code == 200:
                break
        except requests.RequestException:
            pass
        time.sleep(0.2)
    yield base
    proc.terminate()
    proc.wait(timeout=30)


def _cache(base):
    r = requests.get(f"{base}/api/v1/_config/endpoints/-c/cache",
                     headers={"Authorization": f"Bearer {TOKEN}"}, timeout=10)
    assert r.status_code == 200, r.text
    return r.json()


def test_a_partial_update_keeps_caching_enabled(server):
    assert _cache(server)["enabled"] is True
    r = requests.put(f"{server}/api/v1/_config/endpoints/-c/cache",
                     headers={"Authorization": f"Bearer {TOKEN}", "Content-Type": "application/json"},
                     json={"schedule": "10m"}, timeout=10)
    assert r.status_code in (200, 204), r.text
    after = _cache(server)
    assert after["enabled"] is True, f"a partial update disabled the cache: {after}"
    assert after["schedule"] == "10m", after
