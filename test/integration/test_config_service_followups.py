"""Config-service follow-ups found while documenting the #211 fixes (cluster D).

Each server here runs over its own temporary project. Covered:

* `PUT /_config/log-level {"level": "warn"}` is accepted as `warning` (was 400);
* a partial `PUT /_config/endpoints/{slug}` merges onto the endpoint (was 500
  "Missing required field in endpoint config"), and a bad body is 400;
* two YAML files serving the same url-path + method stop startup, naming both;
  a create (or a reload) that would duplicate one answers 409 (400 on reload);
* MCP `flapi_update_endpoint` writes the change to the YAML file;
* created endpoints get readable file names; reload errors carry `error`;
  `compaction.schedule` without `compaction.enabled` warns.
"""

import json
import os
import subprocess
import tempfile
import time

import pytest
import requests
import yaml

from otel_helpers import flapi_binary, free_port

TOKEN = "followups-token-1234"

BASE_CONFIG = (
    "project-name: p\nproject-description: d\n"
    "template:\n  path: ./sqls\n"
    "connections:\n  inmem:\n    properties:\n      database: ':memory:'\n"
    "ducklake:\n  enabled: true\n  alias: cache\n  metadata-path: ./cache.ducklake\n  data-path: ./cache\n"
    "  compaction:\n    schedule: 6h\n"
    "mcp:\n  enabled: true\n"
)


def _write(root, rel, text):
    path = os.path.join(root, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(text)


def _project(files):
    root = tempfile.mkdtemp(prefix="flapi_followups_")
    _write(root, "flapi.yaml", BASE_CONFIG)
    _write(root, "sqls/a.sql", "SELECT 1 AS n\n")
    _write(root, "sqls/b.sql", "SELECT 2 AS n\n")
    for rel, text in files.items():
        _write(root, rel, text)
    return root


def _spawn(root, port):
    log = os.path.join(root, "server.log")
    proc = subprocess.Popen(
        [flapi_binary(), "-c", os.path.join(root, "flapi.yaml"), "-p", str(port),
         "--config-service", "--config-service-token", TOKEN, "--log-level", "info"],
        stdout=open(log, "w"), stderr=subprocess.STDOUT, cwd=root,
        env={**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"})
    return proc, log


class _Server:
    def __init__(self, files):
        self.root = _project(files)
        self.port = free_port()
        self.base = f"http://127.0.0.1:{self.port}"
        self.proc, self.log = _spawn(self.root, self.port)
        for _ in range(300):
            if self.proc.poll() is not None:
                pytest.fail("server exited:\n" + self.output())
            try:
                if requests.get(f"{self.base}/health/live", timeout=2).status_code == 200:
                    return
            except requests.RequestException:
                pass
            time.sleep(0.2)
        pytest.fail("server did not start:\n" + self.output())

    h = {"Authorization": f"Bearer {TOKEN}", "Content-Type": "application/json"}

    def output(self):
        with open(self.log) as f:
            return f.read()

    def sqls(self, name):
        with open(os.path.join(self.root, "sqls", name)) as f:
            return yaml.safe_load(f)

    def mcp(self, tool, **args):
        r = requests.post(f"{self.base}/mcp/jsonrpc", headers=self.h, timeout=15,
                          data=json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                                           "params": {"name": tool, "arguments": args}}))
        return r.json()

    def stop(self):
        self.proc.terminate()
        self.proc.wait(timeout=30)


ENDPOINT_A = "url-path: /a\nmethod: GET\ntemplate-source: a.sql\nconnection: [inmem]\n"


@pytest.fixture
def server():
    s = _Server({"sqls/a.yaml": ENDPOINT_A})
    yield s
    s.stop()


class TestRuntimeLogLevel:
    @pytest.mark.parametrize("given", ["warn", "WARN", "warning"])
    def test_warn_is_an_alias_of_warning(self, server, given):
        r = requests.put(f"{server.base}/api/v1/_config/log-level", headers=server.h,
                         json={"level": given}, timeout=10)
        assert r.status_code == 200, r.text
        assert r.json()["level"] == "warning"

    def test_an_unknown_level_is_still_400(self, server):
        r = requests.put(f"{server.base}/api/v1/_config/log-level", headers=server.h,
                         json={"level": "verbose"}, timeout=10)
        assert r.status_code == 400


class TestPartialUpdate:
    def test_a_partial_body_merges_onto_the_endpoint(self, server):
        r = requests.put(f"{server.base}/api/v1/_config/endpoints/-a", headers=server.h,
                         json={"template-source": "b.sql"}, timeout=10)
        assert r.status_code == 200, r.text
        on_disk = server.sqls("a.yaml")
        assert on_disk["url-path"] == "/a" and on_disk["template-source"] == "b.sql", on_disk
        assert requests.get(f"{server.base}/a", timeout=10).json()["data"][0]["n"] == 2

    def test_a_partial_camelcase_body_takes_url_path_from_the_target(self, server):
        r = requests.put(f"{server.base}/api/v1/_config/endpoints/-a", headers=server.h,
                         json={"templateSource": "b.sql"}, timeout=10)
        assert r.status_code == 200, r.text

    def test_an_invalid_partial_body_is_400_not_500(self, server):
        for body in ({"template-source": "../outside.sql"}, {"method": "POST"}, {"urlPath": "/other"}):
            r = requests.put(f"{server.base}/api/v1/_config/endpoints/-a", headers=server.h,
                             json=body, timeout=10)
            assert r.status_code == 400, (body, r.text)
            assert "error" in r.json()


class TestDuplicateEndpoints:
    def test_two_files_for_one_url_path_and_method_stop_startup(self):
        root = _project({"sqls/a.yaml": ENDPOINT_A, "sqls/a_copy.yaml": ENDPOINT_A})
        proc, log = _spawn(root, free_port())
        try:
            proc.wait(timeout=60)
        except subprocess.TimeoutExpired:
            proc.kill()
            pytest.fail("a duplicate endpoint did not stop startup")
        assert proc.returncode != 0
        out = open(log).read()
        assert "Duplicate endpoint GET /a" in out, out[-2000:]
        assert "a.yaml" in out and "a_copy.yaml" in out

    def test_same_path_with_another_method_is_fine(self):
        s = _Server({"sqls/a.yaml": ENDPOINT_A,
                     "sqls/a_post.yaml": ENDPOINT_A.replace("method: GET", "method: POST")})
        s.stop()

    def test_creating_an_endpoint_declared_by_an_unloaded_file_is_409(self, server):
        # Written after startup and never reloaded: create used to write a second copy.
        _write(server.root, "sqls/late.yaml",
               "url-path: /late\nmethod: GET\ntemplate-source: a.sql\nconnection: [inmem]\n")
        body = {"url-path": "/late", "method": "GET", "template-source": "a.sql", "connection": ["inmem"]}
        r = requests.post(f"{server.base}/api/v1/_config/endpoints", headers=server.h, json=body, timeout=10)
        assert r.status_code == 409, r.text
        assert "late.yaml" in r.json()["error"]
        assert sorted(f for f in os.listdir(os.path.join(server.root, "sqls")) if f.endswith(".yaml")) == \
            ["a.yaml", "late.yaml"]

    def test_creating_a_loaded_endpoint_again_is_409(self, server):
        body = {"url-path": "/a", "method": "GET", "template-source": "a.sql", "connection": ["inmem"]}
        r = requests.post(f"{server.base}/api/v1/_config/endpoints", headers=server.h, json=body, timeout=10)
        assert r.status_code == 409, r.text

    def test_a_reload_that_would_duplicate_another_endpoint_fails(self):
        s = _Server({"sqls/a.yaml": ENDPOINT_A,
                     "sqls/b.yaml": "url-path: /b\nmethod: GET\ntemplate-source: b.sql\nconnection: [inmem]\n"})
        try:
            _write(s.root, "sqls/b.yaml", ENDPOINT_A)
            r = requests.post(f"{s.base}/api/v1/_config/endpoints/-b/reload", headers=s.h, timeout=10)
            assert r.status_code == 400, r.text
            assert "Duplicate endpoint GET /a" in r.json()["reason"], r.json()
            assert r.json()["error"]
        finally:
            s.stop()


class TestMcpUpdateEndpoint:
    def test_the_update_is_written_to_the_yaml(self, server):
        result = server.mcp("flapi_update_endpoint", path="/a", config={"template-source": "b.sql"})
        assert "error" not in result and not result.get("result", {}).get("isError"), result
        assert server.sqls("a.yaml")["template-source"] == "b.sql"
        assert requests.get(f"{server.base}/a", timeout=10).json()["data"][0]["n"] == 2

    def test_template_source_spelling_still_works(self, server):
        result = server.mcp("flapi_update_endpoint", path="/a", template_source="b.sql")
        assert not result.get("result", {}).get("isError"), result
        assert server.sqls("a.yaml")["template-source"] == "b.sql"


class TestSmallFixes:
    @pytest.mark.parametrize("path,file", [("/made-by-cli", "made-by-cli.yaml"), ("/x/y", "x_y.yaml")])
    def test_created_files_have_readable_names(self, server, path, file):
        body = {"url-path": path, "method": "GET", "template-source": "a.sql", "connection": ["inmem"]}
        r = requests.post(f"{server.base}/api/v1/_config/endpoints", headers=server.h, json=body, timeout=10)
        assert r.status_code == 201, r.text
        assert os.path.basename(r.json()["file"]) == file
        assert os.path.exists(os.path.join(server.root, "sqls", file))

    def test_a_reload_error_carries_error(self, server):
        _write(server.root, "sqls/a.yaml", "url-path: /a\nmethod: GET\nrequest: oops\n")
        r = requests.post(f"{server.base}/api/v1/_config/endpoints/-a/reload", headers=server.h, timeout=10)
        assert r.status_code == 400, r.text
        assert r.json()["error"] and r.json()["reason"], r.json()
        missing = requests.post(f"{server.base}/api/v1/_config/endpoints/-nope/reload", headers=server.h, timeout=10)
        assert missing.status_code == 404
        assert missing.json()["error"]

    def test_compaction_schedule_without_enabled_warns(self, server):
        assert "ducklake.compaction.schedule has no effect unless compaction.enabled: true" in server.output()
