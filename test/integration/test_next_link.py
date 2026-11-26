"""The `next` link of a paginated read, against a real server.

`next` used to be built from every entry of the template context, unencoded:

* the server-injected identity (`__auth_username`, `__auth_roles`, ...) of the
  caller went into the URL, and from there into logs, proxies and the browser
  history;
* a value with `&`, a space or `#` broke the link or added parameters;
* a POST read (`operation.type: read`) got a GET query string carrying its body
  filters, and following it re-ran page 1: the defaults for offset/limit beat
  the query string.

Now `next` echoes the client's own query string with offset/limit replaced by
the new window, percent-encoded. It drops `__auth_*` and any name declared as a
header, body or path field, and never takes a value from the server side, so a
field's `default:` stays out of it. A POST read's `next` carries the client's
query string too; POST the same body to it: the query's offset/limit win.
"""
import os
import signal
import socket
import subprocess
import tempfile
import time
from types import SimpleNamespace

import pytest
import requests

from otel_helpers import flapi_binary

pytestmark = pytest.mark.standalone_server

NAME = "a&b #c"

CONFIG = """\
project-name: next-link
template:
  path: ./sqls
connections:
  app:
    init: |
      ATTACH IF NOT EXISTS './app.duckdb' AS app;
      CREATE OR REPLACE TABLE app.items (id INTEGER, name VARCHAR);
      INSERT INTO app.items VALUES (1, 'a&b #c'), (2, 'a&b #c'), (3, 'a&b #c'), (4, 'other'), (5, 'other');
    properties:
      unused: x
rate_limit:
  enabled: false
auth:
  enabled: true
  type: basic
  users:
    - username: alice
      password: alice-pw
      roles: [reader]
"""

ENDPOINTS = {
    "items.yaml": """\
url-path: /items
method: GET
template-source: items.sql
connection: [app]
request:
  - field-name: name
    field-in: query
    required: true
    validators:
      - type: string
        max-length: 20
  - field-name: tenant
    field-in: header
    required: false
  - field-name: region
    field-in: query
    required: false
    default: secret-default
""",
    "items.sql": "SELECT id, name FROM app.items WHERE name = {{ params.name }} ORDER BY id",
    "find.yaml": """\
url-path: /find
method: POST
template-source: find.sql
connection: [app]
operation:
  type: read
request:
  - field-name: name
    field-in: body
    required: true
    validators:
      - type: string
        max-length: 20
  - field-name: min_id
    field-in: query
    required: false
    validators:
      - type: int
""",
    "find.sql": "SELECT id, name FROM app.items WHERE name = {{ params.name }}\n"
                "{{#params.min_id}} AND id >= {{{ params.min_id }}} {{/params.min_id}} ORDER BY id",
}


def _port_in_range() -> int:
    for port in range(31000, 31100):
        s = socket.socket()
        try:
            s.bind(("127.0.0.1", port))
            return port
        except OSError:
            continue
        finally:
            s.close()
    pytest.skip("no free port in 31000-31099")


@pytest.fixture(scope="module")
def server():
    root = tempfile.mkdtemp(prefix="flapi_next_link_")
    os.makedirs(os.path.join(root, "sqls"))
    with open(os.path.join(root, "flapi.yaml"), "w") as f:
        f.write(CONFIG)
    for name, body in ENDPOINTS.items():
        with open(os.path.join(root, "sqls", name), "w") as f:
            f.write(body)
    port = _port_in_range()
    log_path = os.path.join(root, "server.log")
    log = open(log_path, "w")
    proc = subprocess.Popen(
        [flapi_binary(), "-c", "flapi.yaml", "-p", str(port), "--log-level", "info"],
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
            pass
        time.sleep(0.3)
    else:
        pytest.fail("server never became live")
    try:
        yield SimpleNamespace(base=base, root=root, log_path=log_path)
    finally:
        os.killpg(proc.pid, signal.SIGTERM)
        try:
            proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            os.killpg(proc.pid, signal.SIGKILL)
        log.close()


AUTH = ("alice", "alice-pw")
EXPECTED_NEXT = "/items?offset=1&limit=1&name=a%26b%20%23c&undeclared=x&format=json"


class TestGetNextLink:
    def _first(self, server):
        # requests sends the params in this order.
        r = requests.get(f"{server.base}/items", auth=AUTH, timeout=10,
                         params=[("name", NAME), ("limit", 1), ("undeclared", "x"),
                                 ("format", "json"), ("tenant", "spoof")],
                         headers={"tenant": "acme"})
        assert r.status_code == 200, r.text
        return r

    def test_next_echoes_the_client_query(self, server):
        r = self._first(server)
        body = r.json()
        assert body["data"] == [{"id": 1, "name": NAME}], r.text
        assert body["total_count"] == 3
        assert body["next"] == EXPECTED_NEXT, r.text
        assert r.headers["X-Next"] == EXPECTED_NEXT

    def test_next_carries_no_identity_header_field_or_default(self, server):
        nxt = self._first(server).json()["next"]
        for leaked in ("__auth", "alice", "reader", "tenant", "acme", "spoof", "region", "secret-default"):
            assert leaked not in nxt, nxt

    def test_following_next_gives_page_two_with_the_same_filter(self, server):
        nxt = self._first(server).json()["next"]
        r = requests.get(server.base + nxt, auth=AUTH, timeout=10)
        assert r.status_code == 200, r.text
        body = r.json()
        assert body["data"] == [{"id": 2, "name": NAME}], r.text
        assert body["total_count"] == 3
        assert body["next"] == "/items?offset=2&limit=1&name=a%26b%20%23c&undeclared=x&format=json", r.text


class TestPostReadNextLink:
    def test_next_holds_the_window_and_the_client_query(self, server):
        r = requests.post(f"{server.base}/find?min_id=2", auth=AUTH, json={"name": NAME, "limit": 1}, timeout=10)
        assert r.status_code == 200, r.text
        body = r.json()
        assert body["data"] == [{"id": 2, "name": NAME}], r.text
        assert body["next"] == "/find?offset=1&limit=1&min_id=2", r.text

    def test_posting_the_body_to_next_keeps_the_query_filter(self, server):
        body = {"name": NAME, "limit": 1}
        nxt = requests.post(f"{server.base}/find?min_id=2", auth=AUTH, json=body, timeout=10).json()["next"]
        r = requests.post(server.base + nxt, auth=AUTH, json=body, timeout=10)
        assert r.status_code == 200, r.text
        assert r.json()["data"] == [{"id": 3, "name": NAME}], r.text
        assert r.json()["next"] == "", r.text

    def test_query_window_wins_over_the_body_window(self, server):
        # The same body, offset 0 included, POSTed to `next` must move forward.
        body = {"name": NAME, "limit": 1, "offset": 0}
        first = requests.post(f"{server.base}/find", auth=AUTH, json=body, timeout=10).json()
        assert first["next"] == "/find?offset=1&limit=1", first
        r = requests.post(server.base + first["next"], auth=AUTH, json=body, timeout=10)
        assert r.status_code == 200, r.text
        assert r.json()["data"] == [{"id": 2, "name": NAME}], r.text
        assert r.json()["next"] == "/find?offset=2&limit=1", r.text
