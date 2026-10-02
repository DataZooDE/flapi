"""flapi_create_endpoint must create an endpoint that can be given a template,
reloaded, served, and that survives a restart (#155).

It dropped `template-source` (it read `template_source`, which its schema never
declared), kept the endpoint in memory only, and left `update_template` writing
to the template DIRECTORY and `reload` finding nothing on disk. Driven through
the real binary over POST /mcp/jsonrpc, the documented create -> update ->
reload -> serve workflow.
"""

import json
import os
import subprocess
import tempfile
import time

import pytest
import requests

from otel_helpers import flapi_binary, free_port

TOKEN = "create-ep-token"


class _Server:
    def __init__(self, tmp=None):
        self.tmp = tmp or tempfile.mkdtemp(prefix="flapi_createep_")
        self.port = free_port()
        self.base = f"http://127.0.0.1:{self.port}"
        os.makedirs(os.path.join(self.tmp, "sqls"), exist_ok=True)
        with open(os.path.join(self.tmp, "flapi.yaml"), "w") as f:
            f.write("project-name: p\nproject-description: d\n"
                    "template:\n  path: ./sqls\n"
                    "connections:\n  inmem:\n    properties:\n      database: ':memory:'\n"
                    "mcp:\n  enabled: true\n")

    def start(self):
        self.proc = subprocess.Popen(
            [flapi_binary(), "-c", os.path.join(self.tmp, "flapi.yaml"), "-p", str(self.port),
             "--config-service", "--config-service-token", TOKEN, "--log-level", "warning"],
            stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT, cwd=self.tmp,
            env={**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"})
        for _ in range(300):
            try:
                if requests.get(f"{self.base}/health/live", timeout=2).status_code == 200:
                    return self
            except requests.RequestException:
                pass
            time.sleep(0.2)
        pytest.fail("server did not start")

    def stop(self):
        self.proc.terminate()
        self.proc.wait(timeout=30)

    def rpc(self, method, params=None):
        r = requests.post(f"{self.base}/mcp/jsonrpc",
                          headers={"Authorization": f"Bearer {TOKEN}", "Content-Type": "application/json"},
                          data=json.dumps({"jsonrpc": "2.0", "id": 1, "method": method,
                                           "params": params or {}}), timeout=15)
        return r.json()

    def tool(self, name, **args):
        return self.rpc("tools/call", {"name": name, "arguments": args})

    def __enter__(self):
        return self.start()

    def __exit__(self, *a):
        self.stop()


def _ok(resp):
    assert "error" not in resp, resp
    assert not resp["result"].get("isError"), resp
    return resp


def test_the_schema_declares_what_an_endpoint_needs():
    with _Server() as s:
        tools = {t["name"]: t for t in s.rpc("tools/list")["result"]["tools"]}
        props = tools["flapi_create_endpoint"]["inputSchema"]["properties"]
        for key in ("path", "method", "template-source", "connection"):
            assert key in props, f"create_endpoint does not declare {key}: {sorted(props)}"


def test_the_documented_workflow_creates_serves_and_persists():
    s = _Server()
    with s:
        _ok(s.tool("flapi_create_endpoint", path="/orders2", method="GET",
                   **{"template-source": "orders2.sql"}, connection=["inmem"]))
        sqls = os.path.join(s.tmp, "sqls")
        assert os.path.exists(os.path.join(sqls, "orders2.yaml")), os.listdir(sqls)
        assert os.path.exists(os.path.join(sqls, "orders2.sql")), os.listdir(sqls)

        _ok(s.tool("flapi_update_template", endpoint="/orders2", content="SELECT 42 AS n"))
        with open(os.path.join(sqls, "orders2.sql")) as f:
            assert "42" in f.read()

        _ok(s.tool("flapi_reload_endpoint", path="/orders2"))
        r = requests.get(f"{s.base}/orders2", timeout=15)
        assert r.status_code == 200, r.text
        assert "42" in r.text, r.text

    # Survives a restart: it is on disk, not just in memory.
    again = _Server(tmp=s.tmp)
    with again:
        r = requests.get(f"{again.base}/orders2", timeout=15)
        assert r.status_code == 200, r.text


def test_a_missing_template_source_defaults_to_one_that_update_template_can_write():
    with _Server() as s:
        _ok(s.tool("flapi_create_endpoint", path="/defaulted", connection=["inmem"]))
        _ok(s.tool("flapi_update_template", endpoint="/defaulted", content="SELECT 7 AS n"))


@pytest.mark.parametrize("bad", ["../escape.sql", "/etc/passwd", "a/../../b.sql"])
def test_a_template_source_cannot_leave_the_template_directory(bad):
    with _Server() as s:
        resp = s.tool("flapi_create_endpoint", path="/evil", **{"template-source": bad},
                      connection=["inmem"])
        assert "error" in resp or resp["result"].get("isError"), resp
        assert not os.path.exists(os.path.join(s.tmp, "escape.sql"))
