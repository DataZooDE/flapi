"""`default-value` must apply over MCP as it does over REST (#124).

MCPToolHandler::prepareParameters carried a loop that READ as "apply default
values" but had an inverted condition (`defaultValue.empty()`) and an empty
body, so it applied nothing. The same endpoint therefore behaved differently
depending on which protocol called it.

A template writing `LIMIT {{ params.lim }}` with `default: "3"` rendered
`LIMIT 3` over REST and `LIMIT ` over MCP - a parser error, or an unbounded
query, depending on the SQL.

These tests assert the two protocols agree, which is the actual contract;
asserting the MCP result alone would not catch the two drifting apart again.
"""
import json
import os
import subprocess
import tempfile
import time

import pytest
import requests

from otel_helpers import flapi_binary, free_port

pytestmark = pytest.mark.standalone_server


class _Server:
    def __init__(self, required_with_default: bool = False):
        self.required_with_default = required_with_default
        self.tmp = tempfile.mkdtemp(prefix="flapi_mcpdefaults_")
        self.port = free_port()
        self.base_url = f"http://127.0.0.1:{self.port}"
        self.log_path = os.path.join(self.tmp, "server.log")
        sqls = os.path.join(self.tmp, "sqls")
        os.makedirs(sqls, exist_ok=True)
        required = "true" if self.required_with_default else "false"
        with open(os.path.join(sqls, "l.yaml"), "w") as f:
            f.write("url-path: /l\nmethod: GET\n"
                    "request:\n  - field-name: lim\n    field-in: query\n"
                    f"    required: {required}\n    default: \"3\"\n"
                    "template-source: l.sql\nconnection: [inmem]\n"
                    "mcp-tool:\n  name: limited\n  description: Uses a default.\n")
        with open(os.path.join(sqls, "l.sql"), "w") as f:
            f.write("SELECT i FROM range(100) t(i) LIMIT {{ params.lim }}\n")
        with open(os.path.join(self.tmp, "flapi.yaml"), "w") as f:
            f.write(
                "project-name: mcp-defaults\n"
                "project-description: default-value must apply on both protocols\n"
                f"http-port: {self.port}\n"
                "template:\n  path: ./sqls\n"
                "connections:\n  inmem:\n    properties:\n      database: ':memory:'\n"
                "mcp:\n  enabled: true\n")
        self.proc = None

    def start(self):
        self.proc = subprocess.Popen(
            [flapi_binary(), "-c", os.path.join(self.tmp, "flapi.yaml"),
             "-p", str(self.port), "--log-level", "warning"],
            stdout=open(self.log_path, "w"), stderr=subprocess.STDOUT, cwd=self.tmp,
            env={**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"}, preexec_fn=os.setsid)
        deadline = time.time() + 90
        while time.time() < deadline:
            try:
                if requests.get(f"{self.base_url}/health/live", timeout=2).status_code == 200:
                    return self
            except requests.RequestException:
                pass
            time.sleep(0.2)
        pytest.fail(f"server did not start:\n{open(self.log_path).read()[-3000:]}")

    def rest_rows(self, **params):
        r = requests.get(f"{self.base_url}/l", params=params, timeout=10)
        assert r.status_code == 200, r.text[:300]
        return r.json()["data"]

    def mcp_rows(self, **arguments):
        body = {"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                "params": {"name": "limited", "arguments": arguments}}
        r = requests.post(f"{self.base_url}/mcp/jsonrpc",
                          headers={"Content-Type": "application/json"},
                          data=json.dumps(body), timeout=15).json()
        assert "result" in r, r
        assert not r["result"].get("isError"), r
        return r["result"]["structuredContent"]["rows"]

    def stop(self):
        if self.proc:
            import signal
            try:
                os.killpg(os.getpgid(self.proc.pid), signal.SIGKILL)
            except ProcessLookupError:
                pass
            self.proc.wait(timeout=30)

    def __enter__(self):
        return self.start()

    def __exit__(self, *exc):
        self.stop()


class TestMcpDefaultValues:
    def test_mcp_applies_the_default_when_the_argument_is_omitted(self):
        with _Server() as s:
            assert len(s.mcp_rows()) == 3

    def test_the_two_protocols_agree_when_the_value_is_omitted(self):
        # The actual contract. Asserting the MCP side alone would not catch
        # the two drifting apart again.
        with _Server() as s:
            assert s.mcp_rows() == s.rest_rows()

    def test_an_explicit_value_still_overrides_the_default(self):
        with _Server() as s:
            assert len(s.mcp_rows(lim="7")) == 7
            assert s.mcp_rows(lim="7") == s.rest_rows(lim="7")

    def test_a_required_field_with_a_default_passes_validation_on_both(self):
        # Defaults must be applied BEFORE validation, which is the order the
        # REST path uses. Applying them afterwards leaves a second asymmetry in
        # place of the one #124 fixed: a required field carrying a `default:`
        # would pass over REST and fail validation over MCP.
        with _Server(required_with_default=True) as s:
            assert len(s.mcp_rows()) == 3
            assert s.mcp_rows() == s.rest_rows()
