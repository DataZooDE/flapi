"""Validating templates without running them, and the Claude Code hook (#211, F2).

* `template/expand?target=cache&validate_only=1` renders the cache populate
  template with the live watermark, splits it into statements, reports parse
  errors per statement, applies the bare-SELECT rule and binds every statement
  in a rolled-back transaction - `MERGE INTOO`, `SELEC oops;`, unknown columns
  and bare SELECTs used to pass and fail at the next refresh.
* The serving path's `validate_only` binds the SQL that actually runs.
* `expand --cache` of a template with an unclosed section is a 400 naming the
  line, not a 500.
* The plugin hook validates (not just renders) cache and serving templates,
  also with sample values for typed request fields, treats a cache that is not
  built yet as a note, recognises a packed server, and honours FLAPI_CONFIG for
  a template.path outside the config's directory.

The server's config lives in writer/ and its template.path is ../sqls: the
shared layout of a writer and a replica.
"""
import json
import os
import shutil
import subprocess
import tempfile
import time
from types import SimpleNamespace

import pytest
import requests

from otel_helpers import flapi_binary, free_port

pytestmark = pytest.mark.standalone_server

TOKEN = "f2-validation-token"
AUTH = {"Authorization": f"Bearer {TOKEN}"}
HOOK = os.path.join(os.path.dirname(__file__), "..", "..", "plugins", "flapi", "hooks",
                    "validate_and_reload.py")

CONFIG = """\
project-name: f2-validation
project-description: template validation
template:
  path: ../sqls
connections:
  src:
    properties:
      path: ./source.csv
ducklake:
  enabled: true
  alias: cache
  metadata-path: ./cache.ducklake
  data-path: ./cache_data
  warmup: never
rate_limit:
  enabled: false
"""

GOOD_CACHE = """\
{{^cache.previousSnapshotTimestamp}}
CREATE TABLE IF NOT EXISTS {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS
SELECT * FROM read_csv('{{{conn.path}}}') WHERE false;
{{/cache.previousSnapshotTimestamp}}
MERGE INTO {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS t
USING (SELECT * FROM read_csv('{{{conn.path}}}')) AS s
ON t.id = s.id
WHEN MATCHED THEN UPDATE SET name = s.name, version = s.version
WHEN NOT MATCHED THEN INSERT *;
"""

FILES = {
    "orders.yaml": """\
url-path: /orders
method: GET
template-source: serve.sql
connection: [src]
cache:
  enabled: true
  table: orders_cache
  schema: main
  primary-key: [id]
  cursor:
    column: version
    type: integer
  template-file: orders_cache.sql
""",
    "serve.sql": "SELECT * FROM {{{cache.catalog}}}.{{{cache.schema}}}.{{{cache.table}}} ORDER BY id\n",
    "orders_cache.sql": GOOD_CACHE,
    "items.yaml": """\
url-path: /items
method: GET
template-source: items.sql
connection: [src]
request:
  - field-name: id
    field-in: query
    required: false
    validators:
      - type: int
        min: 1
  - field-name: name
    field-in: query
    required: false
    validators:
      - type: string
        regex: "^[a-z]+$"
""",
    "items.sql": """\
SELECT * FROM read_csv('{{{conn.path}}}')
WHERE 1=1
{{#params.id}} AND id = {{ params.id }} {{/params.id}}
{{#params.name}} AND name = '{{{ params.name }}}' {{/params.name}}
""",
}


def _start(cmd, cwd):
    return subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT, cwd=cwd,
                            env={**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"})


def _wait(base):
    for _ in range(300):
        try:
            if requests.get(f"{base}/health/live", timeout=2).status_code == 200:
                return
        except requests.RequestException:
            pass
        time.sleep(0.2)
    raise RuntimeError("server did not start")


@pytest.fixture(scope="module")
def server():
    root = tempfile.mkdtemp(prefix="flapi_f2_")
    writer = os.path.join(root, "writer")
    sqls = os.path.join(root, "sqls")
    os.makedirs(writer)
    os.makedirs(sqls)
    with open(os.path.join(writer, "flapi.yaml"), "w") as f:
        f.write(CONFIG)
    with open(os.path.join(writer, "source.csv"), "w") as f:
        f.write("id,name,version\n1,a,1\n2,b,1\n")
    for name, body in FILES.items():
        with open(os.path.join(sqls, name), "w") as f:
            f.write(body)
    port = free_port()
    proc = _start([flapi_binary(), "-c", os.path.join(writer, "flapi.yaml"), "-p", str(port),
                   "--config-service", "--config-service-token", TOKEN, "--log-level", "warning"], writer)
    base = f"http://127.0.0.1:{port}"
    try:
        _wait(base)
        yield SimpleNamespace(base=base, root=root, writer=writer, sqls=sqls)
    finally:
        proc.terminate()
        proc.wait(timeout=30)
        shutil.rmtree(root, ignore_errors=True)


def _expand(server, slug, query, params=None, template=None):
    body = {"parameters": params or {}}
    if template is not None:
        body["template"] = template
    return requests.post(f"{server.base}/api/v1/_config/endpoints/{slug}/template/expand?{query}",
                         headers=AUTH, json=body, timeout=30)


def _write(server, name, body):
    with open(os.path.join(server.sqls, name), "w") as f:
        f.write(body)


@pytest.fixture
def cache_template(server):
    yield lambda body: _write(server, "orders_cache.sql", body)
    _write(server, "orders_cache.sql", GOOD_CACHE)


def _cache_validate(server):
    r = _expand(server, "-orders", "target=cache&validate_only=1")
    assert r.status_code == 200, r.text
    return r.json()


def test_a_good_cache_template_is_valid_and_nothing_is_built(server):
    result = _cache_validate(server)
    assert result["valid"] is True, result
    assert result["target"] == "cache" and result["statements"] == 2, result
    assert "MERGE INTO cache.main.orders_cache" in result["expanded"]
    # Validation ran nothing: the cache table still does not exist.
    serving = _expand(server, "-orders", "validate_only=1").json()
    assert any(w["type"] == "cache_not_built" for w in serving["warnings"]), serving


@pytest.mark.parametrize("broken,statement,kind,needle", [
    (GOOD_CACHE.replace("MERGE INTO", "MERGE INTOO"), 2, "parse", "syntax error"),
    ("SELEC oops;\n", 1, "parse", "syntax error"),
    (GOOD_CACHE.replace("ON t.id = s.id", "ON t.c_bogus = s.id"), 2, "bind", "c_bogus"),
])
def test_cache_template_errors_are_reported_per_statement(server, cache_template, broken, statement, kind, needle):
    cache_template(broken)
    result = _cache_validate(server)
    assert result["valid"] is False, result
    errors = [e for e in result["errors"] if e["statement"] == statement and e["kind"] == kind]
    assert errors, result
    assert needle in errors[0]["message"], errors


def test_a_guard_select_is_refused_with_the_discarded_result_message(server, cache_template):
    cache_template(GOOD_CACHE.replace("MERGE INTO", "SELECT count(*) FROM read_csv('{{{conn.path}}}');\nMERGE INTO"))
    result = _cache_validate(server)
    assert result["valid"] is False, result
    [error] = [e for e in result["errors"] if e["type"] == "bare_select"]
    assert error["statement"] == 2
    assert "statement 2 of 3 is a bare SELECT; its result is discarded" in error["message"]
    assert "CREATE OR REPLACE TEMP TABLE _chk AS SELECT" in error["message"]
    # The refresh refuses it with the same words.
    r = requests.post(f"{server.base}/api/v1/_config/endpoints/-orders/cache/refresh", headers=AUTH, timeout=60)
    assert r.status_code >= 400 and "its result is discarded" in r.text, r.text


def test_expand_cache_with_an_unclosed_section_is_a_400_with_the_line(server, cache_template):
    cache_template("{{#cache.previousSnapshotTimestamp}}\nSELECT 1\n")
    r = _expand(server, "-orders", "target=cache")
    assert r.status_code == 400, r.text
    assert "orders_cache.sql:1:" in r.text, r.text
    v = _cache_validate(server)
    assert v["valid"] is False and v["errors"][0]["type"] == "template_syntax", v


def test_serving_validation_binds_sql_inside_sections(server):
    template = FILES["items.sql"].replace("AND id = {{ params.id }}", "AND bogus_col = {{ params.id }}")
    absent = _expand(server, "-items", "validate_only=1", template=template).json()
    assert absent["valid"] is True, absent
    present = _expand(server, "-items", "validate_only=1", params={"id": "1"}, template=template).json()
    assert present["valid"] is False, present
    assert "bogus_col" in present["errors"][0]["message"]
    assert present["errors"][0]["statement"] == 1
    assert "bogus_col" in present["expanded"], present


def run_hook(server, edited, config=True, base=None):
    env = {k: v for k, v in os.environ.items() if not k.startswith("FLAPI_")}
    env.update({"FLAPI_URL": base or server.base, "FLAPI_CONFIG_SERVICE_TOKEN": TOKEN,
                "FLAPI_BIN": flapi_binary()})
    if config:
        env["FLAPI_CONFIG"] = os.path.join(server.writer, "flapi.yaml")
    event = json.dumps({"tool_input": {"file_path": str(edited)}})
    proc = subprocess.run(["python3", HOOK], input=event, capture_output=True, text=True, timeout=180, env=env)
    context = ""
    if proc.stdout.strip():
        context = json.loads(proc.stdout)["hookSpecificOutput"]["additionalContext"]
    return proc.returncode, context, proc.stderr


def test_hook_needs_flapi_config_for_a_shared_template_path(server):
    path = os.path.join(server.sqls, "items.sql")
    # No flapi.yaml above ../sqls: without FLAPI_CONFIG the hook cannot know the project.
    assert run_hook(server, path, config=False) == (0, "", "")
    code, context, err = run_hook(server, path)
    assert code == 0, err
    assert "GET /items" in context
    assert "checked with no params and sample params id=1, name=x" in context, context


def test_hook_reports_a_bug_inside_a_param_section(server):
    path = os.path.join(server.sqls, "items.sql")
    try:
        _write(server, "items.sql", FILES["items.sql"].replace("AND id =", "AND bogus_col ="))
        code, _, err = run_hook(server, path)
        assert code == 2, err
        assert "sample params id=1" in err and "bogus_col" in err, err
    finally:
        _write(server, "items.sql", FILES["items.sql"])


def test_hook_validates_a_cache_template_and_notes_an_unbuilt_cache(server, cache_template):
    path = os.path.join(server.sqls, "orders_cache.sql")
    code, context, err = run_hook(server, path)
    assert code == 0, err
    assert "GET /orders" in context, context

    # The serving template reads a cache table that is not built yet: a note.
    code, context, err = run_hook(server, os.path.join(server.sqls, "orders.yaml"))
    assert code == 0, err
    assert "cache not built yet; run `flapii cache refresh /orders`" in context, context

    cache_template(GOOD_CACHE.replace("MERGE INTO", "MERGE INTOO"))
    code, _, err = run_hook(server, path)
    assert code == 2 and "syntax error" in err, err


def test_hook_does_not_reload_into_another_project(server, tmp_path):
    other = tmp_path / "other"
    (other / "sqls").mkdir(parents=True)
    (other / "flapi.yaml").write_text("project-name: o\nproject-description: o\ntemplate:\n  path: ./sqls\n"
                                      "connections:\n  m:\n    properties:\n      database: ':memory:'\n")
    edited = other / "sqls" / "x.sql"
    edited.write_text("SELECT 1\n")
    code, context, err = run_hook(server, edited, config=False)
    assert code == 0, err
    assert "not this project's" in context, context


def test_hook_recognises_a_packed_server(server, tmp_path):
    project = tmp_path / "project"
    (project / "sqls").mkdir(parents=True)
    # This project, packed: same project-name, so the token-free identity
    # check passes and the bundled check is what answers.
    (project / "flapi.yaml").write_text("project-name: f2-validation\nproject-description: p\ntemplate:\n  path: ./sqls\n"
                                        "connections:\n  m:\n    properties:\n      database: ':memory:'\n")
    (project / "sqls" / "one.yaml").write_text("url-path: /one\ntemplate-source: one.sql\nconnection: [m]\n")
    (project / "sqls" / "one.sql").write_text("SELECT 1 AS one\n")
    packed = str(tmp_path / "flapi-packed")
    r = subprocess.run([flapi_binary(), "pack", "--in", str(project), "--out", packed],
                       capture_output=True, text=True, timeout=300,
                       env={**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"})
    assert r.returncode == 0, r.stdout + r.stderr
    run_dir = tmp_path / "run"
    run_dir.mkdir()
    port = free_port()
    proc = _start([packed, "-p", str(port), "--config-service", "--config-service-token", TOKEN,
                   "--log-level", "warning"], str(run_dir))
    base = f"http://127.0.0.1:{port}"
    try:
        _wait(base)
        project = requests.get(f"{base}/api/v1/_config/project", headers=AUTH, timeout=10).json()
        assert project["bundled"] is True
        assert requests.get(f"{server.base}/api/v1/_config/project", headers=AUTH,
                            timeout=10).json()["bundled"] is False
        code, context, err = run_hook(server, os.path.join(server.sqls, "items.sql"), base=base)
        assert code == 0, err
        assert "packed bundle" in context and "cannot be hot-reloaded" in context, context
    finally:
        proc.terminate()
        proc.wait(timeout=30)


def _load_hook_module():
    import importlib.util
    spec = importlib.util.spec_from_file_location("flapi_hook", HOOK)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_hook_sends_no_token_to_another_projects_server(server, tmp_path):
    """G2 12(a): the identity check that decides where the token may go needs no token."""
    import http.server
    import threading

    seen = []

    class OtherProject(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            seen.append((self.path, self.headers.get("X-Config-Token"), self.headers.get("Authorization")))
            if self.path == "/doc.yaml":
                body = b"openapi: 3.0.0\ninfo:\n  title: someone-else\n  version: 1.0.0\n"
                self.send_response(200)
                self.end_headers()
                self.wfile.write(body)
            else:
                self.send_response(404)
                self.end_headers()

        do_POST = do_GET

        def log_message(self, *args):
            pass

    httpd = http.server.HTTPServer(("127.0.0.1", 0), OtherProject)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    try:
        base = f"http://127.0.0.1:{httpd.server_address[1]}"
        code, context, err = run_hook(server, os.path.join(server.sqls, "items.sql"), base=base)
        assert code == 0, err
        assert "serves project 'someone-else'" in context and "f2-validation" in context, context
        assert base in context, context
        assert seen and all(token is None and auth is None for _, token, auth in seen), seen
    finally:
        httpd.shutdown()


def _uptime(server):
    # /health is 503 once an earlier test leaves a cache degraded; the body still has uptime_s.
    r = requests.get(f"{server.base}/health", timeout=5)
    assert r.status_code in (200, 503), r.text
    return float(r.json()["uptime_s"])


def test_hook_says_restart_when_flapi_yaml_changed_under_the_server(server):
    """G2 12(b): a changed ducklake/auth/mcp block needs a restart, not a reload."""
    cfg = os.path.join(server.writer, "flapi.yaml")
    original = open(cfg).read()
    try:
        with open(cfg, "w") as f:
            f.write(original.replace("ducklake:\n  enabled: true", "ducklake:\n  enabled: false"))
        # The worst case CI hit: an edit within a second of server start must still count.
        started = time.time() - _uptime(server)
        os.utime(cfg, (started + 0.5, started + 0.5))
        code, context, err = run_hook(server, os.path.join(server.sqls, "items.sql"))
        assert code == 0, err
        assert "Restart the server first" in context, context
        assert "ducklake.enabled is false in flapi.yaml, true on the server" in context, context
        assert "hot-reloaded" not in context, context
    finally:
        with open(cfg, "w") as f:
            f.write(original)
    # Newer than the server but the same settings: reloads as usual.
    code, context, err = run_hook(server, os.path.join(server.sqls, "items.sql"))
    assert code == 0, err
    assert "hot-reloaded" in context, context


def test_hook_unloads_an_endpoint_file_a_shell_command_deleted(server):
    """G2 12(c): `rm sqls/x.yaml` -> reload-file, which unloads it (or says it cannot)."""
    _write(server, "doomed.yaml", "url-path: /doomed\nmethod: GET\ntemplate-source: items.sql\n"
                                  "connection: [src]\n")
    code, context, err = run_hook(server, os.path.join(server.sqls, "doomed.yaml"))
    assert code == 0, err
    assert "GET /doomed" in context, context
    os.remove(os.path.join(server.sqls, "doomed.yaml"))

    env = {k: v for k, v in os.environ.items() if not k.startswith("FLAPI_")}
    env.update({"FLAPI_URL": server.base, "FLAPI_CONFIG_SERVICE_TOKEN": TOKEN,
                "FLAPI_CONFIG": os.path.join(server.writer, "flapi.yaml")})
    event = json.dumps({"tool_name": "Bash", "cwd": server.sqls,
                        "tool_input": {"command": "rm -f doomed.yaml && echo done"}})
    proc = subprocess.run(["python3", HOOK], input=event, capture_output=True, text=True, timeout=60, env=env)
    assert proc.returncode == 0, proc.stderr
    context = json.loads(proc.stdout)["hookSpecificOutput"]["additionalContext"]
    assert "doomed.yaml was deleted" in context, context
    # 200 {"status":"removed","removed":[...]} -> named; a server without
    # unload support answers 404, reported as nothing loaded / restart.
    assert ("unloaded GET /doomed" in context) or ("had nothing loaded from it" in context), context

    # A command that deletes nothing is a no-op.
    event = json.dumps({"tool_name": "Bash", "cwd": server.sqls, "tool_input": {"command": "ls -la"}})
    proc = subprocess.run(["python3", HOOK], input=event, capture_output=True, text=True, timeout=60, env=env)
    assert (proc.returncode, proc.stdout, proc.stderr) == (0, "", "")


def test_hook_helpers_time_travel_deletions_and_config_blocks(tmp_path):
    """G2 12(c)(d)(b): the parsing the hook does without a YAML library."""
    hook = _load_hook_module()
    assert hook.time_travel_params(
        "SELECT * FROM t {{#params.v}} AT (VERSION => {{ params.v }}) {{/params.v}} "
        "WHERE id = {{ params.id }} AT (TIMESTAMP => CAST('{{{ params.ts }}}' AS TIMESTAMP))"
    ) == {"v", "ts"}
    assert hook.time_travel_params("SELECT 1 WHERE a = {{ params.a }}") == set()

    (tmp_path / "keep.yaml").write_text("x: 1\n")
    found = hook.deleted_yaml_files(
        "rm -f gone.yaml keep.yaml 'with space.yml' *.yaml; git rm old.yml && mv moved.yaml /tmp/dest.yaml"
        " | cat; mv a.sql b.yaml", tmp_path)
    assert sorted(p.name for p in found) == ["gone.yaml", "moved.yaml", "old.yml", "with space.yml"]

    cfg = tmp_path / "flapi.yaml"
    cfg.write_text("project-name: 'my proj'  # c\nducklake:\n  enabled: yes\n  alias: cache\n  retention:\n"
                   "    keep-last-snapshots: 3\nauth:\n  enabled: '{{env.X}}'\nmcp:\n  enabled: false\n")
    assert hook.project_name(cfg) == "my proj"
    blocks = hook.top_level_scalars(cfg, ("ducklake", "auth", "mcp"))
    assert blocks["ducklake"] == {"enabled": "yes", "alias": "cache"}
    assert blocks["mcp"] == {"enabled": "false"}
    assert hook._scalar(blocks["ducklake"]["enabled"]) == "true"


def test_hook_passes_on_overlapping_route_warnings(tmp_path):
    # Cluster I-3: an allow-shadowing split between overlapping routes with
    # different auth validates, so its WARNING must reach Claude as context -
    # e.g. a protected general route added under an open literal.
    project = tmp_path / "shadow"
    (project / "sqls").mkdir(parents=True)
    (project / "flapi.yaml").write_text("project-name: s\nproject-description: s\ntemplate:\n  path: ./sqls\n"
                                        "connections:\n  m:\n    properties:\n      database: ':memory:'\n")
    (project / "sqls" / "q.sql").write_text("SELECT 1 AS n\n")
    (project / "sqls" / "seven.yaml").write_text(
        "url-path: /customers/7\nmethod: GET\ntemplate-source: q.sql\nconnection: [m]\n"
        "auth:\n  enabled: false\nallow-shadowing: true\n")
    general = project / "sqls" / "general.yaml"
    general.write_text(
        "url-path: /customers/:id\nmethod: GET\ntemplate-source: q.sql\nconnection: [m]\n"
        "auth:\n  enabled: true\n  type: basic\n  users:\n    - username: u\n      password: hook-pw-789\n")
    env = {k: v for k, v in os.environ.items() if not k.startswith("FLAPI_")}
    env.update({"FLAPI_URL": f"http://127.0.0.1:{free_port()}", "FLAPI_CONFIG_SERVICE_TOKEN": "t",
                "FLAPI_BIN": flapi_binary()})
    event = json.dumps({"tool_input": {"file_path": str(general)}})
    proc = subprocess.run(["python3", HOOK], input=event, capture_output=True, text=True, timeout=180, env=env)
    assert proc.returncode == 0, proc.stderr
    context = json.loads(proc.stdout)["hookSpecificOutput"]["additionalContext"]
    assert "SECURITY WARNINGS" in context, context
    assert ("Overlapping routes with different auth: GET /customers/7 (open) takes precedence over "
            "GET /customers/:id (basic)") in context, context
    assert "still serves every other URL its pattern matches" in context, context
    assert context.count("Overlapping routes with different auth") == 1, context
    assert "hook-pw-789" not in context


def test_hook_warns_when_an_uncached_template_reads_an_alias_only_a_populate_attaches(tmp_path):
    """#218: the populate template's ATTACH runs on refresh only; an uncached endpoint reading that alias
    does not see it (or a rename-published update of it) until something attached it."""
    hook = _load_hook_module()
    populate = ("DETACH DATABASE IF EXISTS stats;\n"
                "ATTACH '{{{conn.path}}}' AS stats (READ_ONLY);\n"
                "CREATE OR REPLACE TABLE cache.main.t AS SELECT * FROM stats.orders;\n")
    assert hook.attached_aliases(populate) == {"stats"}
    assert hook.attached_aliases("ATTACH IF NOT EXISTS 'a.db' AS \"Mixed\"; attach 'b.db' as other") == {"mixed", "other"}
    assert hook.attached_aliases("SELECT 1") == set()

    # `stats.orders` is a read; the literal 'stats.orders' and a column called stats are not.
    assert hook.reads_alias("SELECT * FROM stats.orders", "stats")
    assert hook.reads_alias("SELECT count(*) FROM \"stats\".main.orders", "stats")
    assert not hook.reads_alias("SELECT 'stats.orders' AS s, stats FROM t", "stats")
    assert not hook.reads_alias("SELECT * FROM mystats.orders", "stats")

    note = hook.populate_only_alias_note("SELECT * FROM stats.orders WHERE id = 1", {"stats_cache.sql": populate})
    assert note and "`stats.`" in note and "stats_cache.sql" in note and "never sees a rename-published" in note, note
    # Not about this alias: no note.
    assert hook.populate_only_alias_note("SELECT * FROM other.orders", {"stats_cache.sql": populate}) is None
    # The connection's init attaches it as well: fine.
    config = "connections:\n  c:\n    init: |\n      ATTACH './s.duckdb' AS stats (READ_ONLY);\n"
    assert hook.populate_only_alias_note("SELECT * FROM stats.orders", {"stats_cache.sql": populate}, config) is None

    # The populate templates are found through the endpoint YAMLs' template-file.
    (tmp_path / "stats.yaml").write_text("url-path: /stats\ntemplate-source: stats.sql\n"
                                         "cache:\n  enabled: true\n  table: t\n  template-file: stats_cache.sql\n")
    (tmp_path / "stats_cache.sql").write_text(populate)
    (tmp_path / "plain.yaml").write_text("url-path: /plain\ntemplate-source: plain.sql\n")
    (tmp_path / "plain.sql").write_text("SELECT * FROM stats.orders")
    assert hook.populate_templates(tmp_path) == {"stats_cache.sql": populate}
