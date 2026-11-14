"""#211 cluster E2: MCP resources, method-aware config service, reload by file,
metrics attribution and the cache-refresh one-shot - against a real server.
"""
import json
import os
import shutil
import signal
import subprocess
import tempfile
import time

import pytest
import requests

from otel_helpers import flapi_binary, free_port

pytestmark = pytest.mark.standalone_server

TOKEN = "cluster-e2-token"
AUTH = {"X-Config-Token": TOKEN}

CONFIG = """\
project-name: cluster-e2
template:
  path: ./sqls
connections:
  mem:
    properties:
      db: ':memory:'
ducklake:
  enabled: true
  alias: cache
  metadata-path: ./cache.ducklake
  data-path: ./cache_data
  warmup: never
rate_limit:
  enabled: false
"""

FILES = {
    # GET + POST on one url-path, each with its own tool.
    "orders_get.yaml": """\
url-path: /orders
method: GET
template-source: orders.sql
connection: [mem]
mcp-tool:
  name: orders_list
  description: list orders
""",
    "orders_post.yaml": """\
url-path: /orders
method: POST
template-source: orders.sql
connection: [mem]
mcp-tool:
  name: orders_create
  description: create an order
""",
    "orders.sql": "SELECT 1 AS id\n",
    # A templated resource whose uri value lands in a triple-brace literal.
    "customer.yaml": """\
request:
  - field-name: id
    field-in: path
    validators:
      - type: string
        regex: '^[A-Z]{5}$'
mcp-resource:
  name: customer_by_id
  description: one customer
  uri-template: "flapi://customers/{id}"
template-source: customer.sql
connection: [mem]
""",
    "customer.sql": "SELECT '{{{ params.id }}}' AS id\n",
    # A big static resource: capped by default, and an empty one.
    "big.yaml": """\
mcp-resource:
  name: big_table
  description: many rows
template-source: big.sql
connection: [mem]
""",
    "big.sql": "SELECT range AS n, 'secret' || range AS email FROM range(5000)\n",
    "shaped.yaml": """\
mcp-resource:
  name: shaped_table
  description: shaped rows
  response:
    max-rows: 3
    redact-columns: [email]
template-source: big.sql
connection: [mem]
""",
    "empty.yaml": """\
mcp-resource:
  name: empty_table
  description: nothing
template-source: empty.sql
connection: [mem]
""",
    "empty.sql": "SELECT 1 AS n WHERE false\n",
    # Prepared rendering: a typed field becomes a bound `?`.
    "lookup.yaml": """\
url-path: /lookup
request:
  - field-name: limit
    field-in: query
    validators:
      - type: int
        min: 1
template-source: lookup.sql
connection: [mem]
mcp-tool:
  name: lookup
  description: look up rows
""",
    "lookup.sql": "SELECT n FROM range(10) t(n) WHERE ({{ params.limit }} IS NULL OR n < {{ params.limit }})\n",
    # A cached endpoint with a cache template-file, and a cursor type that
    # does not match its column (VARCHAR values under type: integer).
    "cached.yaml": """\
url-path: /cached
template-source: cached_serve.sql
connection: [mem]
cache:
  enabled: true
  table: cached_t
  schema: main
  cursor:
    column: tag
    type: integer
  template-file: cached_populate.sql
""",
    "cached_serve.sql": "SELECT * FROM {{{cache.catalog}}}.{{{cache.schema}}}.{{{cache.table}}}\n",
    "cached_populate.sql": """\
CREATE OR REPLACE TABLE {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS
SELECT 'x' || range AS tag FROM range(3)
""",
    # Basic auth on one route, to see its 401 in that route's series.
    "secure.yaml": """\
url-path: /secure
template-source: orders.sql
connection: [mem]
auth:
  enabled: true
  type: basic
  users:
    - username: u
      password: p
""",
}


class Server:
    def __init__(self, root, port, proc, log_path):
        self.root, self.port, self.proc, self.log_path = root, port, proc, log_path
        self.base = f"http://127.0.0.1:{port}"

    def cfg(self, method, path, **kw):
        headers = dict(AUTH)
        headers.update(kw.pop("headers", {}))
        return requests.request(method, self.base + "/api/v1/_config" + path, headers=headers, timeout=30, **kw)

    def rpc(self, method, params=None, rid=1):
        body = {"jsonrpc": "2.0", "id": rid, "method": method, "params": params or {}}
        r = requests.post(self.base + "/mcp/jsonrpc", json=body, timeout=30,
                          headers={"Accept": "application/json, text/event-stream"})
        return r.json()

    def log(self):
        with open(self.log_path) as f:
            return f.read()


@pytest.fixture
def server():
    root = tempfile.mkdtemp(prefix="flapi_e2_")
    os.makedirs(os.path.join(root, "sqls"))
    with open(os.path.join(root, "flapi.yaml"), "w") as f:
        f.write(CONFIG)
    for name, body in FILES.items():
        with open(os.path.join(root, "sqls", name), "w") as f:
            f.write(body)
    port = free_port()
    log_path = os.path.join(root, "server.log")
    log = open(log_path, "w")
    proc = subprocess.Popen(
        [flapi_binary(), "-c", "flapi.yaml", "-p", str(port), "--log-level", "info",
         "--config-service", "--config-service-token", TOKEN, "--no-telemetry"],
        cwd=root, stdout=log, stderr=subprocess.STDOUT, preexec_fn=os.setsid,
        env={**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"},
    )
    srv = Server(root, port, proc, log_path)
    deadline = time.time() + 60
    while time.time() < deadline:
        if proc.poll() is not None:
            pytest.fail("server exited:\n" + srv.log())
        try:
            if requests.get(srv.base + "/api/v1/_config/health", timeout=1).status_code == 200:
                break
        except requests.exceptions.RequestException:
            pass
        time.sleep(0.3)
    else:
        pytest.fail("server did not start:\n" + srv.log())
    srv.rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                           "clientInfo": {"name": "e2", "version": "1"}})
    yield srv
    os.killpg(proc.pid, signal.SIGTERM)
    try:
        proc.wait(timeout=20)
    except subprocess.TimeoutExpired:
        os.killpg(proc.pid, signal.SIGKILL)
    log.close()
    shutil.rmtree(root, ignore_errors=True)


def resource_text(reply):
    assert "result" in reply, reply
    return reply["result"]["contents"][0]


# --- P1 1: resources/read runs the validators --------------------------------

def test_resource_read_validates_uri_template_values(server):
    ok = server.rpc("resources/read", {"uri": "flapi://customers/ALFKI"})
    assert json.loads(resource_text(ok)["text"]) == [{"id": "ALFKI"}]

    bad = server.rpc("resources/read", {"uri": "flapi://customers/x'%20OR%20'1'='1"})
    assert "error" in bad, bad
    assert bad["error"]["code"] == -32602
    assert "id" in bad["error"]["message"]


# --- P1 2 + P2 5: shaping, default cap, empty result, list -------------------

def test_resource_read_is_capped_and_shaped(server):
    big = resource_text(server.rpc("resources/read", {"uri": "flapi://big_table"}))
    assert len(json.loads(big["text"])) == 1000
    assert big["_meta"]["truncated"] is True
    assert big["_meta"]["row_count"] == 5000

    shaped = json.loads(resource_text(server.rpc("resources/read", {"uri": "flapi://shaped_table"}))["text"])
    assert len(shaped) == 3
    assert all(row["email"] == "<redacted>" for row in shaped)


def test_empty_resource_is_an_empty_array(server):
    empty = resource_text(server.rpc("resources/read", {"uri": "flapi://empty_table"}))
    assert empty["text"] == "[]"


def test_templated_resources_only_in_templates_list(server):
    listed = [r["name"] for r in server.rpc("resources/list")["result"]["resources"]]
    assert "customer_by_id" not in listed
    assert "big_table" in listed
    templates = [t["name"] for t in server.rpc("resources/templates/list")["result"]["resourceTemplates"]]
    assert "customer_by_id" in templates


# --- P1 3: method-aware slug routes, list, reload-file -----------------------

def test_shared_path_needs_a_method(server):
    r = server.cfg("GET", "/endpoints/-orders")
    assert r.status_code == 400
    assert "method=" in r.json()["error"]
    r = server.cfg("GET", "/endpoints/-orders?method=POST")
    assert r.status_code == 200
    assert r.json()["method"] == "POST"

    listing = server.cfg("GET", "/endpoints").json()
    assert "GET /orders" in listing and "POST /orders" in listing
    assert "/lookup" in listing  # a path served once keeps its plain key


def test_reload_by_slug_and_method_reloads_that_file(server):
    path = os.path.join(server.root, "sqls", "orders_post.yaml")
    with open(path, "w") as f:
        f.write(FILES["orders_post.yaml"].replace("create an order", "create an order v2"))
    r = server.cfg("POST", "/endpoints/-orders/reload?method=POST")
    assert r.status_code == 200, r.text
    tools = {t["name"]: t for t in server.rpc("tools/list")["result"]["tools"]}
    assert tools["orders_create"]["description"] == "create an order v2"
    assert tools["orders_list"]["description"] == "list orders"


def test_reload_file_loads_a_new_file(server):
    path = os.path.join(server.root, "sqls", "fresh.yaml")
    with open(path, "w") as f:
        f.write("url-path: /fresh\ntemplate-source: orders.sql\nconnection: [mem]\n")
    # create refuses a file already in template.path and points at reload-file
    created = server.cfg("POST", "/endpoints", data=open(path).read(),
                         headers={"Content-Type": "application/x-yaml"})
    assert created.status_code == 409
    assert "reload-file" in created.json()["error"]

    r = server.cfg("POST", "/endpoints/reload-file", json={"file": path})
    assert r.status_code == 201, r.text
    assert r.json()["endpoints"] == ["GET /fresh"]
    assert requests.get(server.base + "/fresh", timeout=10).status_code == 200

    again = server.cfg("POST", "/endpoints/reload-file", json={"file": path})
    assert again.status_code == 200
    assert again.json()["status"] == "reloaded"

    outside = server.cfg("POST", "/endpoints/reload-file", json={"file": os.path.join(server.root, "flapi.yaml")})
    assert outside.status_code == 400


def test_duplicate_tool_name_is_refused_on_reload_file(server):
    path = os.path.join(server.root, "sqls", "dupe.yaml")
    with open(path, "w") as f:
        f.write("template-source: orders.sql\nconnection: [mem]\nmcp-tool:\n  name: orders_list\n  description: x\n")
    r = server.cfg("POST", "/endpoints/reload-file", json={"file": path})
    assert r.status_code == 409
    assert "orders_list" in r.json()["error"]
    assert "orders_get.yaml" in r.json()["error"]


def test_colon_slug_resolves(server):
    with open(os.path.join(server.root, "sqls", "product.yaml"), "w") as f:
        f.write("url-path: /products/:id\ntemplate-source: orders.sql\nconnection: [mem]\n")
    assert server.cfg("POST", "/endpoints/reload-file",
                      json={"file": os.path.join(server.root, "sqls", "product.yaml")}).status_code == 201
    r = server.cfg("GET", "/endpoints/-products-%3Aid")
    assert r.status_code == 200, r.text


# --- P2 6: by-template matches the cache template-file -----------------------

def test_by_template_matches_cache_template_file(server):
    populate = os.path.join(server.root, "sqls", "cached_populate.sql")
    r = server.cfg("POST", "/endpoints/by-template", json={"template_path": populate})
    found = r.json()["endpoints"]
    assert len(found) == 1
    assert found[0]["role"] == "cache.template-file"
    assert found[0]["url_path"] == "/cached"


# --- P2 7: the preview shows bound sites as `?` -----------------------------

def test_expand_and_dry_run_show_bindings(server):
    r = server.cfg("POST", "/endpoints/-lookup/template/expand", json={"parameters": {"limit": "3"}})
    body = r.json()
    assert "?" in body["expanded"] and "3" not in body["expanded"], body
    assert body["bindings"][0]["name"] == "limit"
    assert body["bindings"][0]["value"] == 3

    dry = server.rpc("tools/call", {"name": "lookup", "arguments": {"limit": 3, "_dryRun": True}})
    sc = dry["result"]["structuredContent"]
    assert sc["rows"] == []
    assert sc["parameters"]["limit"] == 3
    assert "?" in sc["rendered_sql"]


# --- P2 8: a dropped watermark is reported -----------------------------------

def test_dropped_watermark_is_reported(server):
    first = server.cfg("POST", "/endpoints/-cached/cache/refresh")
    assert first.status_code == 200, first.text
    expand = server.cfg("POST", "/endpoints/-cached/template/expand?target=cache", json={"parameters": {}})
    assert "watermark_dropped_reason" in expand.json(), expand.json()
    assert "an integer" in expand.json()["watermark_dropped_reason"]
    second = server.cfg("POST", "/endpoints/-cached/cache/refresh").json()
    assert "warning" in second and "cursor.type" in second["warning"]
    audit = server.cfg("GET", "/cache/audit?table=main.cached_t").json()
    assert audit and any("warning" in (row.get("message") or "") for row in audit)


# --- P2 10: metrics attribution ----------------------------------------------

def test_rest_401_is_attributed_to_its_route(server):
    assert requests.get(server.base + "/secure", timeout=10).status_code == 401
    metrics = server.cfg("GET", "/metrics").json()
    series = [s for s in metrics["requests"]["series"] if s.get("route") == "/secure"]
    assert series, metrics["requests"]
    assert series[0]["rejected"] == 1
    assert series[0]["status"]["4xx"] == 1


# --- P2 11: /health counts stale ---------------------------------------------

def test_health_reports_a_stale_count(server):
    health = requests.get(server.base + "/health", timeout=10).json()
    assert "stale" in health["caches"]


# --- P3 13: config-service 401s ----------------------------------------------

def test_config_service_401_is_json_and_logged(server):
    r = requests.get(server.base + "/api/v1/_config/endpoints", headers={"X-Config-Token": "wrong"}, timeout=10)
    assert r.status_code == 401
    assert "error" in r.json()
    m = requests.get(server.base + "/api/v1/_config/metrics", timeout=10)
    assert m.status_code == 401 and m.json()["error"] == r.json()["error"]
    assert "rejected X-Config-Token token" in server.log()

    tool = server.rpc("tools/call", {"name": "flapi_list_endpoints", "arguments": {}})
    assert tool["error"]["code"] == -32001
    assert "X-Config-Token" in tool["error"]["message"]


# --- P3 12: the one-shot's argument handling ---------------------------------

def test_cache_refresh_one_shot_arguments(tmp_path):
    bad = subprocess.run([flapi_binary(), "cache", "refresh", "--bogus"], capture_output=True, text=True, timeout=60)
    assert bad.returncode == 2, bad.stderr
    root = tmp_path
    (root / "sqls").mkdir()
    (root / "flapi.yaml").write_text(CONFIG)
    for name in ("cached.yaml", "cached_serve.sql", "cached_populate.sql"):
        (root / "sqls" / name).write_text(FILES[name])
    ok = subprocess.run([flapi_binary(), "cache", "refresh", "-c", "flapi.yaml", "--all",
                         "--no-telemetry", "--log-level", "error"],
                        cwd=root, capture_output=True, text=True, timeout=120)
    assert ok.returncode == 0, ok.stdout + ok.stderr


# --- the plugin hook uses reload-file, renders, and reports ------------------

HOOK = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "plugins", "flapi", "hooks",
                    "validate_and_reload.py")


def run_hook(server, edited, token=TOKEN, flapi_bin=True):
    env = {k: v for k, v in os.environ.items() if not k.startswith("FLAPI_")}
    env.update({"FLAPI_URL": server.base, "FLAPI_CONFIG_SERVICE_TOKEN": token})
    if flapi_bin:
        env["FLAPI_BIN"] = flapi_binary()
    else:
        env["PATH"] = "/usr/bin:/bin"
    event = json.dumps({"tool_input": {"file_path": str(edited)}})
    proc = subprocess.run(["python3", HOOK], input=event, capture_output=True, text=True, timeout=120, env=env)
    context = ""
    if proc.stdout.strip():
        context = json.loads(proc.stdout)["hookSpecificOutput"]["additionalContext"]
    return proc.returncode, context, proc.stderr


def test_hook_reloads_the_edited_file_of_a_shared_path(server):
    path = os.path.join(server.root, "sqls", "orders_post.yaml")
    with open(path, "w") as f:
        f.write(FILES["orders_post.yaml"].replace("create an order", "create an order v3"))
    code, context, err = run_hook(server, path)
    assert code == 0, err
    assert "POST /orders" in context and "GET /orders" not in context
    tools = {t["name"]: t for t in server.rpc("tools/list")["result"]["tools"]}
    assert tools["orders_create"]["description"] == "create an order v3"
    # the plain-password basic auth in secure.yaml is a security warning, passed on although valid
    assert "SECURITY WARNINGS" in context


def test_hook_loads_a_new_file(server):
    path = os.path.join(server.root, "sqls", "brand_new.yaml")
    with open(path, "w") as f:
        f.write("url-path: /brand-new\ntemplate-source: orders.sql\nconnection: [mem]\n")
    code, context, err = run_hook(server, path, flapi_bin=False)
    assert code == 0, err
    assert "GET /brand-new" in context
    assert requests.get(server.base + "/brand-new", timeout=10).status_code == 200


def test_hook_reports_render_errors_of_a_sql_edit(server):
    path = os.path.join(server.root, "sqls", "orders.sql")
    with open(path, "w") as f:
        f.write("SELECT FROM WHERE\n")
    code, _, err = run_hook(server, path, flapi_bin=False)
    assert code == 2
    assert "rendering failed" in err


def test_hook_renders_a_cache_template_file(server):
    path = os.path.join(server.root, "sqls", "cached_populate.sql")
    code, context, err = run_hook(server, path, flapi_bin=False)
    assert code == 0, err
    assert "GET /cached" in context


def test_hook_main_config_and_token_and_outside_files(server):
    code, context, _ = run_hook(server, os.path.join(server.root, "flapi.yaml"), flapi_bin=False)
    assert code == 0 and "restart the server" in context

    path = os.path.join(server.root, "sqls", "orders_get.yaml")
    code, _, err = run_hook(server, path, token="wrong-token", flapi_bin=False)
    assert code == 2 and "rejected" in err

    outside = os.path.join(server.root, "notes.yaml")
    with open(outside, "w") as f:
        f.write("anything: 1\n")
    code, context, err = run_hook(server, outside, flapi_bin=False)
    assert code == 0 and context == "" and err == ""
