"""Per-request CPU waste removed without changing what a request sees (#243).

* The SQL template is no longer read from disk on every request: its text is cached by path and
  revalidated with one stat. An edited template must still be picked up by the very next request,
  even an edit that keeps the file's size, and a deleted template must still fail.
* The endpoint is resolved once per request and shared by the rate limiter, the auth middleware
  and the handler: all three must still judge the same (most specific) route.
"""
import base64
import os
import shutil
import tempfile

import pytest
import requests

from otel_helpers import flapi_binary
from test_duckdb_upgrade import Run

pytestmark = pytest.mark.standalone_server


def write(root):
    os.makedirs(os.path.join(root, "sqls"))
    with open(os.path.join(root, "flapi.yaml"), "w") as f:
        f.write("project-name: cpu243\ntemplate:\n  path: ./sqls\nconnections:\n  c:\n    properties:\n"
                "      x: '1'\nrate_limit:\n  enabled: false\n")
    files = {
        "edit.yaml": "url-path: /edit\nmethod: GET\ntemplate-source: edit.sql\nconnection: [c]\n"
                     "with-pagination: false\n",
        "edit.sql": "SELECT 'aaaa' AS v\n",
        # A literal route and a parameterised one that also matches /items/7: the literal one
        # is more specific, is open and unlimited; the parameterised one demands auth.
        "item.yaml": ("url-path: /items/:id\nmethod: GET\ntemplate-source: item.sql\nconnection: [c]\n"
                      "with-pagination: false\n"
                      "auth:\n  enabled: true\n  type: basic\n  users:\n    - username: u\n      password: p\n"
                      "rate-limit:\n  enabled: true\n  max: 2\n  interval: 60\n"),
        "item.sql": "SELECT 'param' AS route\n",
        "seven.yaml": ("url-path: /items/7\nmethod: GET\ntemplate-source: seven.sql\nconnection: [c]\n"
                       "with-pagination: false\nallow-shadowing: true\nauth:\n  enabled: false\n"),
        "seven.sql": "SELECT 'literal' AS route\n",
    }
    for name, body in files.items():
        with open(os.path.join(root, "sqls", name), "w") as f:
            f.write(body)


@pytest.fixture
def server():
    root = tempfile.mkdtemp(prefix="flapi_cpu243_")
    write(root)
    run = Run(flapi_binary(), root)
    run.root = root
    yield run
    run.stop()
    shutil.rmtree(root, ignore_errors=True)


def test_an_edited_template_is_served_by_the_next_request(server):
    path = os.path.join(server.root, "sqls", "edit.sql")
    s = requests.Session()
    assert s.get(f"{server.base}/edit", timeout=30).json() == [{"v": "aaaa"}]
    for i in range(30):
        value = f"{i:04d}"                       # the same size every time
        with open(path, "w") as f:
            f.write(f"SELECT '{value}' AS v\n")
        assert s.get(f"{server.base}/edit", timeout=30).json() == [{"v": value}], i
    stat = os.stat(path)
    with open(path, "w") as f:                    # same size AND the old mtime restored
        f.write("SELECT 'zzzz' AS v\n")
    os.utime(path, ns=(stat.st_atime_ns, stat.st_mtime_ns))
    assert s.get(f"{server.base}/edit", timeout=30).json() == [{"v": "zzzz"}]
    os.replace(path, path + ".gone")              # deleted: fails, as it always did
    assert s.get(f"{server.base}/edit", timeout=30).status_code == 500
    os.replace(path + ".gone", path)
    assert s.get(f"{server.base}/edit", timeout=30).json() == [{"v": "zzzz"}]


def test_rate_limit_auth_and_handler_judge_the_same_route(server):
    basic = {"Authorization": "Basic " + base64.b64encode(b"u:p").decode()}
    for _ in range(5):                            # the literal route: open and unlimited
        r = requests.get(f"{server.base}/items/7", timeout=30)
        assert r.status_code == 200 and r.json() == [{"route": "literal"}], r.text
        assert "X-RateLimit-Limit" not in r.headers
    assert requests.get(f"{server.base}/items/8", timeout=30).status_code == 401
    statuses = [requests.get(f"{server.base}/items/9", headers=basic, timeout=30) for _ in range(3)]
    assert [r.status_code for r in statuses] == [200, 429, 429], [r.text for r in statuses]
    assert statuses[0].json() == [{"route": "param"}]


def test_a_template_written_through_the_config_service_is_served_at_once(server):
    # Two same-size writes in quick succession may land in one filesystem timestamp tick (and on
    # Windows mtime has whole seconds): the stamp alone cannot tell them apart. A write flAPI
    # makes itself drops the cached text, so the next request reads the file.
    from test_duckdb_upgrade import TOKEN
    h = {"Authorization": f"Bearer {TOKEN}", "Content-Type": "application/json"}
    s = requests.Session()
    for i in range(40):
        for value in (f"a{i:03d}", f"b{i:03d}"):
            r = s.put(f"{server.base}/api/v1/_config/endpoints/-edit/template", headers=h,
                      json={"template": f"SELECT '{value}' AS v\n"}, timeout=30)
            assert r.status_code == 200, r.text
            assert s.get(f"{server.base}/edit", timeout=30).json() == [{"v": value}], (i, value)
