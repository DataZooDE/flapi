"""An endpoint sent back through PUT must keep its protections, and pagination
parameters must be validated (#191).

Found by the crew review of the server and reproduced against a real binary:
GET an endpoint then PUT the returned JSON dropped every request validator (a
value previously rejected was accepted) and the endpoint's auth users, and
re-enabled a disabled cache; `?limit=1;SELECT 1` was spliced into
`LIMIT {{params.limit}}` because only a numeric prefix was parsed.
"""

import os
import subprocess
import tempfile
import time

import pytest
import requests

from otel_helpers import flapi_binary, free_port

TOKEN = "roundtrip-token-123"


def _w(base, rel, text):
    path = os.path.join(base, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(text)


@pytest.fixture(scope="module")
def server():
    tmp = tempfile.mkdtemp(prefix="flapi_roundtrip_")
    port = free_port()
    _w(tmp, "flapi.yaml",
       "project-name: p\nproject-description: d\ntemplate:\n  path: ./sqls\n"
       "connections:\n  inmem:\n    properties:\n      database: ':memory:'\n"
       "ducklake:\n  enabled: true\n  alias: cache\n  metadata-path: ./cache.ducklake\n  data-path: ./cache\n")
    _w(tmp, "sqls/v.yaml",
       "url-path: /v\nmethod: GET\ntemplate-source: v.sql\nconnection: [inmem]\n"
       "request-fields-validation: true\n"
       "request:\n"
       "  - field-name: amount\n    field-in: query\n    validators:\n      - type: int\n        min: 1\n        max: 100\n"
       "  - field-name: code\n    field-in: query\n    validators:\n      - type: string\n        regex: '^[A-Z]{3}$'\n        max-length: 3\n"
       "  - field-name: kind\n    field-in: query\n    validators:\n      - type: enum\n        allowedValues: [red, green]\n"
       "  - field-name: day\n    field-in: query\n    validators:\n      - type: date\n        min: '2020-01-01'\n        max: '2020-12-31'\n")
    _w(tmp, "sqls/v.sql", "SELECT 1 AS n FROM range(5) LIMIT {{params.limit}}\n")
    _w(tmp, "sqls/a.yaml",
       "url-path: /a\nmethod: GET\ntemplate-source: a.sql\nconnection: [inmem]\n"
       "auth:\n  enabled: true\n  type: basic\n  users:\n    - username: alice\n      password: wonderland\n      roles: [admin]\n")
    _w(tmp, "sqls/a.sql", "SELECT 1 AS n\n")
    _w(tmp, "sqls/c.yaml",
       "url-path: /c\nmethod: GET\ntemplate-source: c.sql\nconnection: [inmem]\n"
       "cache:\n  enabled: false\n  table: c_cache\n  schedule: 5m\n")
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


H = {"Authorization": f"Bearer {TOKEN}", "Content-Type": "application/json"}


def _roundtrip(base, slug):
    got = requests.get(f"{base}/api/v1/_config/endpoints/{slug}", headers=H, timeout=10)
    assert got.status_code == 200, got.text
    put = requests.put(f"{base}/api/v1/_config/endpoints/{slug}", headers=H, data=got.text, timeout=10)
    assert put.status_code == 200, put.text


class TestValidatorsSurviveGetThenPut:

    @pytest.mark.parametrize("query,expected", [
        ("amount=500", 400),            # int max 100
        ("amount=0", 400),              # int min 1
        ("code=abc", 400),              # regex ^[A-Z]{3}$
        ("code=ABCD", 400),             # max-length 3
        ("kind=blue", 400),             # enum
        ("day=2019-12-31", 400),        # date min
        ("day=2021-01-01", 400),        # date max
        ("amount=5&code=ABC&kind=red&day=2020-06-01", 200),
    ])
    def test_the_same_requests_are_accepted_or_rejected_before_and_after(self, server, query, expected):
        before = requests.get(f"{server}/v?{query}", timeout=10).status_code
        assert before == expected, f"precondition: {query} -> {before}"
        _roundtrip(server, "-v")
        after = requests.get(f"{server}/v?{query}", timeout=10).status_code
        assert after == expected, f"after GET->PUT, {query} became {after} (validators were dropped)"


class TestAuthSurvivesGetThenPut:

    def test_an_endpoint_with_basic_auth_still_requires_it(self, server):
        assert requests.get(f"{server}/a", timeout=10).status_code == 401
        assert requests.get(f"{server}/a", auth=("alice", "wonderland"), timeout=10).status_code == 200
        _roundtrip(server, "-a")
        assert requests.get(f"{server}/a", timeout=10).status_code == 401, "auth was dropped by GET->PUT"
        assert requests.get(f"{server}/a", auth=("alice", "wonderland"), timeout=10).status_code == 200
        assert requests.get(f"{server}/a", auth=("alice", "wrong"), timeout=10).status_code == 401


class TestCacheStaysDisabled:

    def test_a_disabled_cache_is_not_re_enabled_by_a_round_trip(self, server):
        got = requests.get(f"{server}/api/v1/_config/endpoints/-c/cache", headers=H, timeout=10).json()
        assert got["enabled"] is False
        _roundtrip(server, "-c")
        after = requests.get(f"{server}/api/v1/_config/endpoints/-c/cache", headers=H, timeout=10).json()
        assert after["enabled"] is False, after
        assert after["table"] == "c_cache" and after["schedule"] == "5m", after


class TestPaginationIsValidated:

    @pytest.mark.parametrize("bad", ["1;SELECT 1", "abc", "-1", "1e3", "1 OR 1=1", "99999999999999999999", "0x10", " 5", "5 "])
    def test_a_malformed_limit_is_rejected_before_it_reaches_sql(self, server, bad):
        r = requests.get(f"{server}/v", params={"limit": bad}, timeout=10)
        assert r.status_code == 400, f"limit={bad!r} -> {r.status_code} {r.text[:200]}"

    @pytest.mark.parametrize("bad", ["1;SELECT 1", "abc", "-1", "9999999999999999999999"])
    def test_a_malformed_offset_is_rejected(self, server, bad):
        r = requests.get(f"{server}/v", params={"offset": bad}, timeout=10)
        assert r.status_code == 400, f"offset={bad!r} -> {r.status_code}"

    def test_valid_pagination_still_works(self, server):
        r = requests.get(f"{server}/v", params={"limit": "2"}, timeout=10)
        assert r.status_code == 200, r.text
        assert len(r.json()["data"]) == 2
        assert requests.get(f"{server}/v", timeout=10).status_code == 200
