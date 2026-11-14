"""REST authorisation and write semantics, against a real server (#211).

Each of these was documented, configured, and silently did nothing:

* `auth.roles` on an endpoint was never parsed: a user without the role got 200;
* a top-level `auth:` block protected no endpoint at all;
* passlib's pbkdf2_sha256 hashes ("ab64" base64) never verified;
* `field-in: header` fields were read from the query string;
* `method: DELETE` never ran its template;
* `cache.refresh-on-write` only logged (REST and MCP alike);
* a 500 body quoted the rendered SQL and absolute file paths;
* DECIMAL went through double (7790.62 -> 7790.61999999999989086) and
  timestamps lost their microseconds.

MCP authorisation is separate (mcp.auth) and must be unaffected by the global
REST auth block.
"""
import base64
import hashlib
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

TOKEN = "rest-auth-semantics-token"
CS_AUTH = {"Authorization": f"Bearer {TOKEN}"}


def passlib_pbkdf2_sha256(password: str, salt: bytes, rounds: int = 1000) -> str:
    """The exact string passlib.hash.pbkdf2_sha256 produces (ab64, no padding)."""
    def ab64(b: bytes) -> str:
        return base64.b64encode(b).decode().rstrip("=").replace("+", ".")
    dk = hashlib.pbkdf2_hmac("sha256", password.encode(), salt, rounds, dklen=32)
    return f"$pbkdf2-sha256${rounds}${ab64(salt)}${ab64(dk)}"


# A salt whose base64 contains '+', so the ab64 form contains '.'.
ALICE_HASH = passlib_pbkdf2_sha256("alice-pw", bytes([0xfb, 0xef, 0xbe] * 5 + [0x01]))
assert "." in ALICE_HASH.split("$")[3]

USERS = f"""\
  users:
    - username: alice
      password: '{ALICE_HASH}'
      roles: [admin, reader]
    - username: bob
      password: bob-pw
      roles: [reader]
"""

CONFIG = f"""\
project-name: rest-auth-semantics
template:
  path: ./sqls
connections:
  app:
    init: |
      ATTACH IF NOT EXISTS './app.duckdb' AS app;
      CREATE TABLE IF NOT EXISTS app.items (id INTEGER PRIMARY KEY, name VARCHAR);
    properties:
      missing: ./data/does-not-exist.parquet
ducklake:
  enabled: true
  alias: cache
  metadata-path: ./cache.ducklake
  data-path: ./cache_data
  warmup: never
rate_limit:
  enabled: false
cors:
  allow-origins: ["http://localhost"]
mcp:
  enabled: true
auth:
  enabled: true
  type: basic
{USERS}"""

CACHE_SQL = """\
CREATE OR REPLACE TABLE {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS
SELECT * FROM app.items
"""
SERVE_SQL = "SELECT * FROM {{{cache.catalog}}}.{{{cache.schema}}}.{{{cache.table}}} ORDER BY id"

ENDPOINTS = {
    # No auth block: inherits the global one.
    "items.yaml": "url-path: /items\nmethod: GET\ntemplate-source: items.sql\nconnection: [app]\n",
    "items.sql": "SELECT id, name FROM app.items ORDER BY id",
    # Explicit opt-out.
    "public.yaml": "url-path: /public\nmethod: GET\ntemplate-source: one.sql\nconnection: [app]\n"
                   "auth:\n  enabled: false\n",
    "one.sql": "SELECT 1 AS ok",
    # Own block with a role requirement.
    "admin.yaml": "url-path: /admin\nmethod: GET\ntemplate-source: one.sql\nconnection: [app]\n"
                  "auth:\n  enabled: true\n  type: basic\n  roles: [admin]\n" + USERS,
    # Header field.
    "tenant.yaml": """\
url-path: /tenant
method: GET
template-source: tenant.sql
connection: [app]
auth:
  enabled: false
request:
  - field-name: tenant
    field-in: header
    required: true
    validators:
      - type: string
        pattern: "^[a-z]+$"
""",
    "tenant.sql": "SELECT '{{{ params.tenant }}}' AS tenant",
    # Writes. POST inserts, DELETE deletes (and runs its template).
    "create.yaml": """\
url-path: /items
method: POST
template-source: create.sql
connection: [app]
operation:
  type: write
request:
  - field-name: id
    field-in: body
    required: true
    validators:
      - type: int
  - field-name: name
    field-in: body
    required: true
    validators:
      - type: string
        pattern: "^[a-z0-9-]+$"
""",
    "create.sql": "INSERT INTO app.items VALUES ({{ params.id }}, '{{{ params.name }}}')",
    "delete.yaml": """\
url-path: /items/:id
method: DELETE
template-source: delete.sql
connection: [app]
operation:
  type: write
request:
  - field-name: id
    field-in: path
    required: true
    validators:
      - type: int
""",
    "delete.sql": "DELETE FROM app.items WHERE id = {{ params.id }}",
    # Cached read over the same table, and two writers that maintain the cache.
    "cached.yaml": """\
url-path: /items_cached
method: GET
template-source: serve.sql
connection: [app]
auth:
  enabled: false
cache:
  enabled: true
  table: items_cache
  schema: main
  template-file: items_cache.sql
""",
    "serve.sql": SERVE_SQL,
    "items_cache.sql": CACHE_SQL,
    "write_sync.yaml": """\
url-path: /items_sync
method: POST
template-source: create.sql
connection: [app]
auth:
  enabled: false
operation:
  type: write
request:
  - field-name: id
    field-in: body
    required: true
    validators:
      - type: int
  - field-name: name
    field-in: body
    required: true
cache:
  enabled: true
  table: items_cache
  schema: main
  template-file: items_cache.sql
  invalidate-on-write: true
""",
    "write_async.yaml": """\
url-path: /items_async
method: POST
template-source: create.sql
connection: [app]
auth:
  enabled: false
mcp-tool:
  name: add_item_async
  description: Add an item and refresh the cache in the background
operation:
  type: write
request:
  - field-name: id
    field-in: body
    required: true
    validators:
      - type: int
  - field-name: name
    field-in: body
    required: true
cache:
  enabled: true
  table: items_cache
  schema: main
  template-file: items_cache.sql
  refresh-on-write: true
""",
    # A failing query: the 500 must not echo SQL or paths.
    "broken.yaml": "url-path: /broken\nmethod: GET\ntemplate-source: broken.sql\nconnection: [app]\n"
                   "auth:\n  enabled: false\n",
    "broken.sql": "SELECT * FROM read_parquet('{{{ conn.missing }}}')",
    # Exact types.
    "types.yaml": "url-path: /types\nmethod: GET\ntemplate-source: types.sql\nconnection: [app]\n"
                  "auth:\n  enabled: false\n",
    "types.sql": "SELECT 7790.62::DECIMAL(10,2) AS price, "
                 "'2024-05-06 07:08:09.123456'::TIMESTAMP AS ts",
}


@pytest.fixture(scope="module")
def server():
    root = tempfile.mkdtemp(prefix="flapi_rest_auth_")
    os.makedirs(os.path.join(root, "sqls"))
    with open(os.path.join(root, "flapi.yaml"), "w") as f:
        f.write(CONFIG)
    for name, body in ENDPOINTS.items():
        with open(os.path.join(root, "sqls", name), "w") as f:
            f.write(body)

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


def log_text(s):
    with open(s.log_path) as f:
        return f.read()


ALICE = ("alice", "alice-pw")
BOB = ("bob", "bob-pw")


class TestRestAuthorisation:
    def test_global_auth_protects_endpoint_without_own_block(self, server):
        assert requests.get(f"{server.base}/items", timeout=10).status_code == 401
        assert requests.get(f"{server.base}/items", auth=("bob", "nope"), timeout=10).status_code == 401
        assert requests.get(f"{server.base}/items", auth=BOB, timeout=10).status_code == 200

    def test_inheritance_is_logged_at_startup(self, server):
        assert "Global auth (type 'basic') applies to" in log_text(server)

    def test_explicit_opt_out_stays_public(self, server):
        r = requests.get(f"{server.base}/public", timeout=10)
        assert r.status_code == 200, r.text

    def test_roles_401_403_200(self, server):
        assert requests.get(f"{server.base}/admin", timeout=10).status_code == 401
        r = requests.get(f"{server.base}/admin", auth=BOB, timeout=10)
        assert r.status_code == 403, r.text
        assert r.json()["error"] == "Forbidden"
        r = requests.get(f"{server.base}/admin", auth=ALICE, timeout=10)
        assert r.status_code == 200, r.text

    def test_passlib_ab64_hash_verifies(self, server):
        assert requests.get(f"{server.base}/items", auth=ALICE, timeout=10).status_code == 200
        assert requests.get(f"{server.base}/items", auth=("alice", "alice-pX"), timeout=10).status_code == 401

    def test_mcp_is_not_affected_by_global_rest_auth(self, server):
        r = requests.post(f"{server.base}/mcp/jsonrpc", json={
            "jsonrpc": "2.0", "id": 1, "method": "initialize",
            "params": {"protocolVersion": "2024-11-05", "capabilities": {},
                       "clientInfo": {"name": "t", "version": "0"}}}, timeout=10)
        assert r.status_code == 200, r.text
        headers = {"Mcp-Session-Id": r.headers.get("Mcp-Session-Id", "")}
        r = requests.post(f"{server.base}/mcp/jsonrpc", headers=headers, json={
            "jsonrpc": "2.0", "id": 2, "method": "tools/list"}, timeout=10)
        assert r.status_code == 200, r.text
        names = [t["name"] for t in r.json()["result"]["tools"]]
        assert "add_item_async" in names


class TestHeaderFields:
    def test_header_field_is_read_from_headers_case_insensitively(self, server):
        r = requests.get(f"{server.base}/tenant", headers={"TENANT": "acme"}, timeout=10)
        assert r.status_code == 200, r.text
        assert r.json()["data"][0]["tenant"] == "acme"

    def test_query_parameter_cannot_stand_in_for_a_header_field(self, server):
        r = requests.get(f"{server.base}/tenant", params={"tenant": "evil"}, timeout=10)
        assert r.status_code == 400, r.text
        r = requests.get(f"{server.base}/tenant", params={"tenant": "evil"},
                         headers={"tenant": "acme"}, timeout=10)
        assert r.status_code == 200
        assert r.json()["data"][0]["tenant"] == "acme"

    def test_header_field_is_validated(self, server):
        r = requests.get(f"{server.base}/tenant", headers={"tenant": "ACME;--"}, timeout=10)
        assert r.status_code == 400, r.text


class TestWrites:
    def test_delete_runs_its_template(self, server):
        r = requests.post(f"{server.base}/items", json={"id": 101, "name": "gone"}, auth=BOB, timeout=10)
        assert r.status_code == 201, r.text
        ids = [row["id"] for row in requests.get(f"{server.base}/items", auth=BOB, timeout=10).json()["data"]]
        assert 101 in ids

        r = requests.delete(f"{server.base}/items/101", auth=BOB, timeout=10)
        assert r.status_code == 200, r.text
        assert r.json()["rows_affected"] == 1

        ids = [row["id"] for row in requests.get(f"{server.base}/items", auth=BOB, timeout=10).json()["data"]]
        assert 101 not in ids

    def test_delete_is_validated_and_authenticated(self, server):
        assert requests.delete(f"{server.base}/items/1", timeout=10).status_code == 401
        r = requests.delete(f"{server.base}/items/abc", auth=BOB, timeout=10)
        assert r.status_code == 400, r.text

    def _cached_ids(self, server):
        r = requests.get(f"{server.base}/items_cached", timeout=10)
        assert r.status_code == 200, r.text
        return [row["id"] for row in r.json()["data"]]

    def _prime_cache(self, server):
        r = requests.post(f"{server.base}/api/v1/_config/endpoints/-items_cached/cache/refresh",
                          headers=CS_AUTH, timeout=60)
        assert r.status_code == 200, r.text

    def test_invalidate_on_write_is_synchronous(self, server):
        self._prime_cache(server)
        r = requests.post(f"{server.base}/items_sync", json={"id": 201, "name": "sync"}, timeout=30)
        assert r.status_code == 201, r.text
        # Visible as soon as the write has answered.
        assert 201 in self._cached_ids(server)

    def test_refresh_on_write_is_asynchronous_but_happens(self, server):
        self._prime_cache(server)
        assert 301 not in self._cached_ids(server)
        r = requests.post(f"{server.base}/items_async", json={"id": 301, "name": "async"}, timeout=30)
        assert r.status_code == 201, r.text
        deadline = time.time() + 20
        while time.time() < deadline and 301 not in self._cached_ids(server):
            time.sleep(0.2)
        assert 301 in self._cached_ids(server), log_text(server)[-3000:]

    def test_refresh_on_write_applies_to_mcp_tool_writes(self, server):
        self._prime_cache(server)
        assert 401 not in self._cached_ids(server)
        r = requests.post(f"{server.base}/mcp/jsonrpc", json={
            "jsonrpc": "2.0", "id": 1, "method": "initialize",
            "params": {"protocolVersion": "2024-11-05", "capabilities": {},
                       "clientInfo": {"name": "t", "version": "0"}}}, timeout=10)
        headers = {"Mcp-Session-Id": r.headers.get("Mcp-Session-Id", "")}
        r = requests.post(f"{server.base}/mcp/jsonrpc", headers=headers, json={
            "jsonrpc": "2.0", "id": 2, "method": "tools/call",
            "params": {"name": "add_item_async", "arguments": {"id": 401, "name": "mcp"}}}, timeout=30)
        assert r.status_code == 200, r.text
        assert "error" not in r.json(), r.text
        deadline = time.time() + 20
        while time.time() < deadline and 401 not in self._cached_ids(server):
            time.sleep(0.2)
        assert 401 in self._cached_ids(server), log_text(server)[-3000:]


class TestErrorBodies:
    def test_500_body_leaks_neither_sql_nor_paths(self, server):
        r = requests.get(f"{server.base}/broken", timeout=10)
        assert r.status_code == 500
        body = r.text
        assert "does-not-exist" not in body
        assert server.root not in body
        assert "read_parquet" not in body
        assert "SELECT" not in body
        payload = r.json()
        assert payload["error"] == "Internal Server Error"
        rid = payload.get("request_id")
        assert rid and rid.startswith("req-")
        # The detail is in the log, under the same request id.
        deadline = time.time() + 5
        while time.time() < deadline and rid not in log_text(server):
            time.sleep(0.1)
        text = log_text(server)
        assert rid in text
        assert "does-not-exist" in text


class TestExactTypes:
    def test_decimal_and_timestamp_precision(self, server):
        r = requests.get(f"{server.base}/types", timeout=10)
        assert r.status_code == 200, r.text
        assert '"price":7790.62' in r.text, r.text
        # A naive TIMESTAMP carries no zone, so no `Z` (only TIMESTAMPTZ does).
        assert r.json()["data"][0]["ts"] == "2024-05-06T07:08:09.123456"


class TestVersion:
    def test_version_flag_is_the_build_version(self):
        out = subprocess.run([flapi_binary(), "--version"], capture_output=True, text=True, timeout=30)
        assert out.returncode == 0, out.stderr
        assert out.stdout.strip() != "1.0"
        assert out.stdout.strip() != ""

    def test_health_reports_the_build_version(self, server):
        r = requests.get(f"{server.base}/api/v1/_config/health", headers=CS_AUTH, timeout=10)
        assert r.status_code == 200, r.text
        version = r.json()["server"]["version"]
        assert version != "1.0.0"
        cli = subprocess.run([flapi_binary(), "--version"], capture_output=True, text=True, timeout=30)
        assert version == cli.stdout.strip()


class TestLogLevelAlias:
    def _config(self, level_line):
        root = tempfile.mkdtemp(prefix="flapi_loglevel_")
        os.makedirs(os.path.join(root, "sqls"))
        with open(os.path.join(root, "flapi.yaml"), "w") as f:
            f.write("project-name: ll\ntemplate:\n  path: ./sqls\n" + level_line)
        return root

    def test_env_warn_is_accepted(self):
        root = self._config("")
        out = subprocess.run([flapi_binary(), "-c", "flapi.yaml", "--validate-config"], cwd=root,
                             capture_output=True, text=True, timeout=60,
                             env={**os.environ, "FLAPI_LOG_LEVEL": "warn", "DATAZOO_DISABLE_TELEMETRY": "1"})
        assert out.returncode == 0, out.stdout + out.stderr

    def test_config_unknown_level_is_a_startup_error(self):
        root = self._config("log-level: loud\n")
        out = subprocess.run([flapi_binary(), "-c", "flapi.yaml", "--validate-config"], cwd=root,
                             capture_output=True, text=True, timeout=60,
                             env={**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"})
        assert out.returncode != 0
        assert "log-level" in out.stdout + out.stderr
