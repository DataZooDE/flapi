"""Config-service and MCP behaviour fixed for #211 (cluster C), against the real binary.

* MCP tools/list and resources/list were built once at startup, so a tool
  created, changed or deleted at runtime was never listed (and an updated
  description never shown).
* REST create/update/delete changed memory only, and create dropped the
  mcp-tool block (a REST-only endpoint), so `flapii endpoints create -f x.yaml`
  produced no tool, and nothing survived a reload.
* An int field's default was emitted as the string "10" in the tool schema.
* flapi_* tools refused a config token shorter than 8 characters that REST
  accepted, and read it only from `Authorization: Bearer`.
* A failed reload answered 404 like an unknown endpoint.
* `schema tables` was a Binder Error; `schema connections` always [].
* DELETE /config answered 500, PUT /project 501.
* The startup log claimed "started" before the bind, with MCP at a hard-coded
  localhost:8080.
* A packed binary's --validate-config warned sqls/sqls/x.sql for every endpoint.
"""
import json
import os
import shutil
import signal
import socket
import subprocess
import tempfile
import time

import pytest
import requests
import yaml

from otel_helpers import flapi_binary, free_port

pytestmark = pytest.mark.standalone_server

SHORT_TOKEN = "abc123"  # 6 characters: REST always took it, MCP refused it

CONFIG = """\
project-name: cluster-c
project-description: persistence and MCP refresh
template:
  path: ./sqls
connections:
  inmem:
    properties:
      database: ':memory:'
mcp:
  enabled: true
"""

ITEMS_YAML = """\
url-path: /items
method: GET
template-source: items.sql
connection: [inmem]
request:
  - field-name: limit
    field-in: query
    required: false
    default: '10'
    validators:
      - type: int
        min: 1
        max: 100
mcp-tool:
  name: list_items
  description: First description
"""

ITEMS_SQL = "SELECT * FROM range(3) t(id)\n"


class Server:
    def __init__(self, root=None, token=SHORT_TOKEN):
        self.root = root or tempfile.mkdtemp(prefix="flapi_clusterc_")
        self.token = token
        self.port = free_port()
        self.base = f"http://127.0.0.1:{self.port}"
        self.sqls = os.path.join(self.root, "sqls")
        os.makedirs(self.sqls, exist_ok=True)
        if not os.path.exists(os.path.join(self.root, "flapi.yaml")):
            with open(os.path.join(self.root, "flapi.yaml"), "w") as f:
                f.write(CONFIG)
            with open(os.path.join(self.sqls, "items.sql"), "w") as f:
                f.write(ITEMS_SQL)
        self.log_path = os.path.join(self.root, f"server-{self.port}.log")

    def start(self):
        self.log = open(self.log_path, "w")
        self.proc = subprocess.Popen(
            [flapi_binary(), "-c", "flapi.yaml", "-p", str(self.port), "--log-level", "info",
             "--config-service", "--config-service-token", self.token],
            cwd=self.root, stdout=self.log, stderr=subprocess.STDOUT, preexec_fn=os.setsid,
            env={**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"})
        deadline = time.time() + 60
        while time.time() < deadline:
            if self.proc.poll() is not None:
                pytest.fail("server exited:\n" + self.logs())
            try:
                if requests.get(f"{self.base}/health/live", timeout=1).status_code == 200:
                    return self
            except requests.RequestException:
                time.sleep(0.2)
        pytest.fail("server never became live")

    def stop(self):
        os.killpg(self.proc.pid, signal.SIGTERM)
        try:
            self.proc.wait(timeout=20)
        except subprocess.TimeoutExpired:
            os.killpg(self.proc.pid, signal.SIGKILL)
        self.log.close()

    def logs(self):
        with open(self.log_path) as f:
            return f.read()

    @property
    def auth(self):
        return {"X-Config-Token": self.token}

    def rpc(self, method, params=None, headers=None):
        h = {"Content-Type": "application/json", **(headers or {})}
        r = requests.post(f"{self.base}/mcp/jsonrpc", headers=h, timeout=30,
                          data=json.dumps({"jsonrpc": "2.0", "id": 1, "method": method, "params": params or {}}))
        return r.json()

    def tools(self):
        resp = self.rpc("tools/list")
        assert "error" not in resp, resp
        return {t["name"]: t for t in resp["result"]["tools"]}

    def __enter__(self):
        return self.start()

    def __exit__(self, *a):
        self.stop()


def create(s, doc):
    return requests.post(f"{s.base}/api/v1/_config/endpoints", headers=s.auth, json=doc, timeout=30)


class TestRestCreatePersistsAndMcpSeesIt:
    def test_create_with_mcp_tool_is_listed_callable_and_on_disk(self):
        with Server() as s:
            assert "list_items" not in s.tools()

            r = create(s, yaml.safe_load(ITEMS_YAML))
            assert r.status_code == 201, r.text
            body = r.json()
            assert body["slug"] == "-items" and body["url-path"] == "/items", body

            # The mcp-tool block survived the create (it used to be dropped) and
            # tools/list sees it without a restart.
            tools = s.tools()
            assert "list_items" in tools, sorted(tools)
            assert tools["list_items"]["description"] == "First description"
            # The int default is typed, not "10".
            assert tools["list_items"]["inputSchema"]["properties"]["limit"]["default"] == 10

            call = s.rpc("tools/call", {"name": "list_items", "arguments": {}})
            assert "error" not in call and not call["result"].get("isError"), call

            # Persisted, with the request validators the JSON deserializer dropped.
            path = os.path.join(s.sqls, "items.yaml")
            assert os.path.exists(path), os.listdir(s.sqls)
            with open(path) as f:
                on_disk = yaml.safe_load(f)
            assert on_disk["mcp-tool"]["name"] == "list_items"
            assert on_disk["request"][0]["validators"][0]["type"] == "int"

            # Creating it again is a conflict, not a duplicate.
            again = create(s, yaml.safe_load(ITEMS_YAML))
            assert again.status_code == 409, again.text

    def test_update_is_persisted_and_relisted_delete_removes_file_and_tool(self):
        with Server() as s:
            assert create(s, yaml.safe_load(ITEMS_YAML)).status_code == 201
            assert s.tools()["list_items"]["description"] == "First description"

            r = requests.put(f"{s.base}/api/v1/_config/endpoints/-items", headers=s.auth, timeout=30,
                             json={"url-path": "/items", "mcp-tool": {"description": "Second description"}})
            assert r.status_code == 200, r.text
            assert s.tools()["list_items"]["description"] == "Second description"
            with open(os.path.join(s.sqls, "items.yaml")) as f:
                on_disk = yaml.safe_load(f)
            assert on_disk["mcp-tool"] == {"name": "list_items", "description": "Second description"}
            # Deep merge: what the patch did not mention is kept.
            assert on_disk["request"][0]["validators"][0]["max"] == 100

            r = requests.delete(f"{s.base}/api/v1/_config/endpoints/-items", headers=s.auth, timeout=30)
            assert r.status_code == 200, r.text
            assert "list_items" not in s.tools()
            assert not os.path.exists(os.path.join(s.sqls, "items.yaml"))

    def test_created_endpoint_survives_a_restart(self):
        s = Server()
        with s:
            assert create(s, yaml.safe_load(ITEMS_YAML)).status_code == 201
        with Server(root=s.root) as again:
            assert "list_items" in again.tools()
            assert requests.get(f"{again.base}/items", timeout=30).status_code == 200

    def test_reload_after_an_edit_on_disk_is_relisted(self):
        with Server() as s:
            assert create(s, yaml.safe_load(ITEMS_YAML)).status_code == 201
            path = os.path.join(s.sqls, "items.yaml")
            with open(path) as f:
                text = f.read()
            with open(path, "w") as f:
                f.write(text.replace("First description", "Edited on disk"))
            r = requests.post(f"{s.base}/api/v1/_config/endpoints/-items/reload", headers=s.auth, timeout=30)
            assert r.status_code == 200, r.text
            assert s.tools()["list_items"]["description"] == "Edited on disk"


class TestReloadStatus:
    def test_broken_yaml_is_400_with_reason_unknown_is_404(self):
        with Server() as s:
            assert create(s, yaml.safe_load(ITEMS_YAML)).status_code == 201
            with open(os.path.join(s.sqls, "items.yaml"), "w") as f:
                f.write("url-path: /items\nmethod: GET\nconnection: [inmem]\n")  # no template-source
            r = requests.post(f"{s.base}/api/v1/_config/endpoints/-items/reload", headers=s.auth, timeout=30)
            assert r.status_code == 400, r.text
            body = r.json()
            assert body["success"] is False and body["reason"], body
            assert "template-source" in body["reason"], body

            r = requests.post(f"{s.base}/api/v1/_config/endpoints/nope/reload", headers=s.auth, timeout=30)
            assert r.status_code == 404, r.text


class TestConfigToken:
    def test_short_token_works_for_mcp_config_tools_via_x_config_token(self):
        with Server() as s:
            # REST takes it ...
            assert requests.get(f"{s.base}/api/v1/_config/endpoints", headers=s.auth, timeout=30).status_code == 200
            # ... and so do the flapi_* tools, from X-Config-Token (Authorization
            # stays free for the MCP credential).
            resp = s.rpc("tools/call", {"name": "flapi_list_endpoints", "arguments": {}}, headers=s.auth)
            assert "error" not in resp, resp
            # Bearer still works.
            resp = s.rpc("tools/call", {"name": "flapi_list_endpoints", "arguments": {}},
                         headers={"Authorization": f"Bearer {s.token}"})
            assert "error" not in resp, resp
            # A wrong one does not.
            resp = s.rpc("tools/call", {"name": "flapi_list_endpoints", "arguments": {}},
                         headers={"X-Config-Token": "wrong"})
            assert "error" in resp, resp
            assert "config-service token is only 6 characters" in s.logs()


class TestSchemaAndMethods:
    def test_schema_tables_and_connections(self):
        with Server() as s:
            r = requests.get(f"{s.base}/api/v1/_config/schema?tables=true", headers=s.auth, timeout=30)
            assert r.status_code == 200, r.text
            assert isinstance(r.json()["tables"], list) and "error" not in r.json(), r.text
            r = requests.get(f"{s.base}/api/v1/_config/schema?connections=true", headers=s.auth, timeout=30)
            assert r.status_code == 200, r.text
            assert [c["name"] for c in r.json()["connections"]] == ["inmem"], r.text

    def test_unsupported_methods_are_405_with_allow(self):
        with Server() as s:
            r = requests.delete(f"{s.base}/config", headers=s.auth, timeout=30)
            assert r.status_code == 405, r.text
            assert r.headers.get("Allow") == "GET"
            r = requests.put(f"{s.base}/api/v1/_config/project", headers=s.auth, json={}, timeout=30)
            assert r.status_code == 405, r.text
            assert r.headers.get("Allow") == "GET"


class TestValidateUsesTheRealParser:
    def test_template_relative_to_yaml_dir_and_validators_are_checked(self):
        with Server() as s:
            r = requests.post(f"{s.base}/api/v1/_config/endpoints/-items/validate", headers=s.auth,
                              data=ITEMS_YAML, timeout=30)
            assert r.status_code == 200, r.text
            assert not any("does not exist" in w for w in r.json().get("warnings", [])), r.text
            # A validator the loader rejects is now an error here too.
            bad = ITEMS_YAML.replace("type: int", "type: int\n        min: not-a-number")
            r = requests.post(f"{s.base}/api/v1/_config/endpoints/-items/validate", headers=s.auth,
                              data=bad, timeout=30)
            assert r.status_code == 400, r.text
            assert r.json()["valid"] is False and r.json()["errors"], r.text
            # No scratch file is left behind.
            assert not [n for n in os.listdir(s.sqls) if n.startswith(".flapi-validate-")]

    def test_empty_body_says_why(self):
        with Server() as s:
            r = requests.post(f"{s.base}/api/v1/_config/endpoints/-items/validate", headers=s.auth,
                              data="", timeout=30)
            assert r.status_code == 400
            assert "No YAML given" in r.json()["errors"][0], r.text


class TestStartupLog:
    def test_listening_line_has_the_real_port_and_comes_after_bind(self):
        with Server() as s:
            time.sleep(0.3)
            logs = s.logs()
            assert f"listening on http://0.0.0.0:{s.port}" in logs or f":{s.port} (REST API" in logs, logs[-3000:]
            assert f"/mcp/jsonrpc" in logs
            assert "localhost:8080/mcp/jsonrpc" not in logs
            assert "unified server started" not in logs

    def test_port_in_use_never_claims_started(self):
        blocker = socket.socket()
        blocker.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        blocker.bind(("0.0.0.0", 0))
        blocker.listen(1)
        port = blocker.getsockname()[1]
        s = Server()
        s.port = port
        try:
            log = open(s.log_path, "w")
            proc = subprocess.run(
                [flapi_binary(), "-c", "flapi.yaml", "-p", str(port), "--log-level", "info"],
                cwd=s.root, stdout=log, stderr=subprocess.STDOUT, timeout=60,
                env={**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"})
            log.close()
            assert proc.returncode != 0
            logs = s.logs()
            assert "could not start" in logs, logs[-3000:]
            assert "listening on" not in logs and "server started" not in logs, logs[-3000:]
        finally:
            blocker.close()


class TestPackedValidateConfig:
    def test_no_doubled_template_prefix(self, tmp_path):
        s = Server()
        with open(os.path.join(s.sqls, "items.yaml"), "w") as f:
            f.write(ITEMS_YAML)
        out = str(tmp_path / "flapi-packed")
        r = subprocess.run([flapi_binary(), "pack", "--in", s.root, "--out", out],
                           capture_output=True, text=True, timeout=300,
                           env={**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"})
        assert r.returncode == 0, r.stdout + r.stderr
        run_dir = tmp_path / "elsewhere"
        run_dir.mkdir()
        r = subprocess.run([out, "--validate-config"], cwd=run_dir, capture_output=True, text=True,
                           timeout=120, env={**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"})
        output = r.stdout + r.stderr
        assert "sqls/sqls/" not in output, output
        assert "Template file does not exist" not in output, output
        shutil.rmtree(s.root, ignore_errors=True)
