"""The config service must not read or write files outside the templates directory,
the CORS allowlist must hold, and a supplied token must not be printed (#189).

Found by the crew review of the server and reproduced against a real binary:
`POST /_config/endpoints` with an absolute `template-source`, then
`PUT .../template`, overwrote that file; a disallowed Origin still got
`Access-Control-Allow-Origin: *`; the bearer token was echoed to stdout.
"""

import json
import os
import subprocess
import tempfile
import time

import pytest
import requests

from otel_helpers import flapi_binary, free_port

TOKEN = "containment-token-123"


class _Server:
    def __init__(self, extra_config=""):
        self.tmp = tempfile.mkdtemp(prefix="flapi_contain_")
        self.outside = tempfile.mkdtemp(prefix="flapi_outside_")
        os.makedirs(os.path.join(self.tmp, "sqls"))
        self.port = free_port()
        self.base = f"http://127.0.0.1:{self.port}"
        with open(os.path.join(self.tmp, "flapi.yaml"), "w") as f:
            f.write("project-name: p\nproject-description: d\n"
                    "template:\n  path: ./sqls\n"
                    "connections:\n  inmem:\n    properties:\n      database: ':memory:'\n"
                    "ducklake:\n  enabled: true\n  alias: cache\n  metadata-path: ./cache.ducklake\n  data-path: ./cache\n"
                    "mcp:\n  enabled: true\n" + extra_config)
        with open(os.path.join(self.tmp, "sqls", "ok.yaml"), "w") as f:
            f.write("url-path: /ok\nmethod: GET\ntemplate-source: ok.sql\nconnection: [inmem]\n"
                    "cache:\n  enabled: true\n  table: ok_cache\n  schedule: 5m\n")
        with open(os.path.join(self.tmp, "sqls", "ok.sql"), "w") as f:
            f.write("SELECT 1 AS n\n")
        self.log = os.path.join(self.tmp, "server.out")
        self.proc = subprocess.Popen(
            [flapi_binary(), "-c", os.path.join(self.tmp, "flapi.yaml"), "-p", str(self.port),
             "--config-service", "--config-service-token", TOKEN, "--log-level", "warning"],
            stdout=open(self.log, "w"), stderr=subprocess.STDOUT, cwd=self.tmp,
            env={**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"})
        for _ in range(300):
            try:
                if requests.get(f"{self.base}/health/live", timeout=2).status_code == 200:
                    return
            except requests.RequestException:
                pass
            time.sleep(0.2)
        pytest.fail("server did not start")

    @property
    def h(self):
        return {"Authorization": f"Bearer {TOKEN}", "Content-Type": "application/json"}

    def create(self, path, template_source, **extra):
        body = {"url-path": path, "method": "GET", "template-source": template_source,
                "connection": ["inmem"], **extra}
        return requests.post(f"{self.base}/api/v1/_config/endpoints", headers=self.h,
                             json=body, timeout=10)

    def put_template(self, slug, text):
        return requests.put(f"{self.base}/api/v1/_config/endpoints/{slug}/template",
                            headers=self.h, json={"template": text}, timeout=10)

    def mcp(self, tool, **args):
        r = requests.post(f"{self.base}/mcp/jsonrpc", headers=self.h, timeout=15,
                          data=json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                                           "params": {"name": tool, "arguments": args}}))
        return r.json()

    def stop(self):
        self.proc.terminate()
        self.proc.wait(timeout=30)


@pytest.fixture(scope="module")
def server():
    s = _Server("cors:\n  allow-origins:\n    - 'https://allowed.example'\n")
    yield s
    s.stop()


class TestTemplatePathContainment:

    @pytest.mark.parametrize("bad", ["ABSOLUTE", "../outside.sql", "sub/../../outside.sql"])
    def test_an_endpoint_cannot_point_its_template_outside_the_directory(self, server, bad):
        target = os.path.join(server.outside, "victim.txt")
        source = target if bad == "ABSOLUTE" else bad
        r = server.create("/esc", source)
        assert r.status_code == 400, f"created an endpoint with template-source {source!r}: {r.status_code} {r.text}"
        # Even if something got stored, writing through it must not reach the target.
        server.put_template("-esc", "PWNED")
        assert not os.path.exists(target), "a file outside the templates directory was written"
        assert not os.path.exists(os.path.join(server.tmp, "outside.sql"))

    def test_a_symlink_inside_the_directory_cannot_be_used_to_escape(self, server):
        target = os.path.join(server.outside, "via-link.txt")
        open(target, "w").write("original")
        os.symlink(target, os.path.join(server.tmp, "sqls", "link.sql"))
        r = server.create("/viasymlink", "link.sql")
        assert r.status_code == 400, r.text
        server.put_template("-viasymlink", "PWNED")
        assert open(target).read() == "original"

    def test_a_normal_nested_relative_template_still_works(self, server):
        r = server.create("/nested", "deep/er/nested.sql")
        assert r.status_code == 201, r.text
        assert server.put_template("-nested", "SELECT 7 AS n").status_code == 200
        assert open(os.path.join(server.tmp, "sqls", "deep", "er", "nested.sql")).read() == "SELECT 7 AS n"

    def test_a_cache_template_file_cannot_point_outside(self, server):
        target = os.path.join(server.outside, "cache-victim.txt")
        r = requests.put(f"{server.base}/api/v1/_config/endpoints/-ok/cache", headers=server.h,
                         json={"enabled": True, "template-file": target}, timeout=10)
        assert r.status_code == 400, r.text
        requests.put(f"{server.base}/api/v1/_config/endpoints/-ok/cache/template", headers=server.h,
                     json={"template": "PWNED"}, timeout=10)
        assert not os.path.exists(target)

    def test_mcp_update_endpoint_cannot_point_outside(self, server):
        server.create("/mcpupd", "mcpupd.sql")
        target = os.path.join(server.outside, "mcp-victim.txt")
        result = server.mcp("flapi_update_endpoint", path="/mcpupd", template_source=target)
        failed = "error" in result or result.get("result", {}).get("isError")
        server.mcp("flapi_update_template", endpoint="/mcpupd", content="PWNED")
        assert not os.path.exists(target), f"MCP update_endpoint let a template point outside: {result}"
        assert failed, result


class TestCorsAllowlist:

    def test_a_disallowed_origin_gets_no_allow_origin_header(self, server):
        r = requests.get(f"{server.base}/ok", headers={"Origin": "https://evil.example"}, timeout=10)
        assert "Access-Control-Allow-Origin" not in r.headers, dict(r.headers)

    def test_an_allowed_origin_is_echoed(self, server):
        r = requests.get(f"{server.base}/ok", headers={"Origin": "https://allowed.example"}, timeout=10)
        assert r.headers.get("Access-Control-Allow-Origin") == "https://allowed.example"


def test_a_supplied_token_is_not_printed():
    s = _Server()
    s.stop()  # stdout is block-buffered into the file: stopping flushes it
    out = open(s.log).read()
    assert "CONFIG SERVICE ENABLED" in out, out[-500:]
    assert TOKEN not in out, "the supplied config-service token was written to stdout"


def test_a_generated_token_is_still_shown_once():
    # With no token supplied the operator has no other way to learn it.
    tmp = tempfile.mkdtemp(prefix="flapi_gentoken_")
    os.makedirs(os.path.join(tmp, "sqls"))
    port = free_port()
    with open(os.path.join(tmp, "flapi.yaml"), "w") as f:
        f.write("project-name: p\nproject-description: d\ntemplate:\n  path: ./sqls\n"
                "connections:\n  inmem:\n    properties:\n      database: ':memory:'\n")
    log = os.path.join(tmp, "out")
    proc = subprocess.Popen([flapi_binary(), "-c", os.path.join(tmp, "flapi.yaml"), "-p", str(port),
                             "--config-service", "--log-level", "warning"],
                            stdout=open(log, "w"), stderr=subprocess.STDOUT, cwd=tmp,
                            env={k: v for k, v in os.environ.items() if k != "FLAPI_CONFIG_SERVICE_TOKEN"}
                            | {"DATAZOO_DISABLE_TELEMETRY": "1"})
    for _ in range(300):
        try:
            if requests.get(f"http://127.0.0.1:{port}/health/live", timeout=2).status_code == 200:
                break
        except requests.RequestException:
            pass
        time.sleep(0.2)
    proc.terminate()
    proc.wait(timeout=30)
    assert "Token: " in open(log).read()
