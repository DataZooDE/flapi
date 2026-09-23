"""MCP config tools must check the token, not just its shape (#131).

`validateAuthToken` inspected only the FORM of the presented token - its
length, its character set, its scheme name - and returned success for anything
well-formed. The configured config-service token was never compared, so a tool
declared `auth_required = true` ran for any caller:

    Authorization: Bearer totally-not-the-token
    -> {"message":"Template updated successfully"}

Two things were wrong there, and this file pins both.

The second: `flapi_update_template` never wrote anything. It validated that the
endpoint existed and reported success, quoting the length of the content it had
just discarded - so an operator was told the update worked while the file on
disk was untouched. It now refuses honestly. Reporting success for work not
done is the one outcome a caller cannot detect.
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

TOKEN = "the-real-config-service-token"


class _Server:
    def __init__(self):
        self.tmp = tempfile.mkdtemp(prefix="flapi_mcptool_")
        self.port = free_port()
        self.base_url = f"http://127.0.0.1:{self.port}"
        self.log_path = os.path.join(self.tmp, "server.log")
        sqls = os.path.join(self.tmp, "sqls")
        os.makedirs(sqls, exist_ok=True)
        with open(os.path.join(sqls, "hello.yaml"), "w") as f:
            f.write("url-path: /hello\nmethod: GET\n"
                    "template-source: hello.sql\nconnection: [inmem]\n"
                    "mcp-tool:\n  name: hello\n  description: Say hello.\n")
        self.template = os.path.join(sqls, "hello.sql")
        with open(self.template, "w") as f:
            f.write("SELECT 'original-template' AS v\n")
        with open(os.path.join(self.tmp, "flapi.yaml"), "w") as f:
            f.write(
                "project-name: mcp-tool-auth\n"
                "project-description: config tools must verify the token\n"
                f"http-port: {self.port}\n"
                "template:\n  path: ./sqls\n"
                "connections:\n  inmem:\n    properties:\n      database: ':memory:'\n"
                "mcp:\n  enabled: true\n")
        self.proc = None

    def start(self):
        self.proc = subprocess.Popen(
            [flapi_binary(), "-c", os.path.join(self.tmp, "flapi.yaml"),
             "-p", str(self.port), "--config-service",
             "--config-service-token", TOKEN, "--log-level", "warning"],
            stdout=open(self.log_path, "w"), stderr=subprocess.STDOUT, cwd=self.tmp,
            env={**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"},
            preexec_fn=os.setsid)
        deadline = time.time() + 90
        while time.time() < deadline:
            try:
                if requests.get(f"{self.base_url}/health/live", timeout=2).status_code == 200:
                    return self
            except requests.RequestException:
                pass
            time.sleep(0.2)
        pytest.fail(f"server did not start:\n{open(self.log_path).read()[-3000:]}")

    def call_tool(self, name, token=None, **arguments):
        headers = {"Content-Type": "application/json"}
        if token is not None:
            headers["Authorization"] = f"Bearer {token}"
        body = {"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                "params": {"name": name, "arguments": arguments}}
        return requests.post(f"{self.base_url}/mcp/jsonrpc", headers=headers,
                             data=json.dumps(body), timeout=15).json()

    def list_tools(self, token=None):
        headers = {"Content-Type": "application/json"}
        if token is not None:
            headers["Authorization"] = f"Bearer {token}"
        body = {"jsonrpc": "2.0", "id": 1, "method": "tools/list", "params": {}}
        r = requests.post(f"{self.base_url}/mcp/jsonrpc", headers=headers,
                          data=json.dumps(body), timeout=15).json()
        assert "result" in r, r
        return r["result"]["tools"]

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


class TestMcpConfigToolAuth:
    def test_a_well_formed_but_wrong_token_is_refused(self):
        # The defect verbatim: this exact call used to answer
        # "Template updated successfully".
        with _Server() as s:
            got = s.call_tool("flapi_update_template", token="totally-not-the-token",
                              endpoint="/hello", content="SELECT 'pwned' AS v")
            assert "error" in got, got
            assert "Invalid authentication token" in got["error"]["message"]

    def test_a_missing_token_is_refused(self):
        with _Server() as s:
            got = s.call_tool("flapi_update_template",
                              endpoint="/hello", content="SELECT 'pwned' AS v")
            assert "error" in got, got
            assert "Authentication required" in got["error"]["message"]

    def test_a_token_that_is_a_prefix_of_the_real_one_is_refused(self):
        # Guards the comparison itself, not just the presence of a check.
        with _Server() as s:
            got = s.call_tool("flapi_update_template", token=TOKEN[:-1],
                              endpoint="/hello", content="SELECT 1")
            assert "error" in got
            assert "Invalid authentication token" in got["error"]["message"]

    def test_the_real_token_gets_past_authentication(self):
        # It must reach the tool - otherwise this suite would pass with the
        # gate stuck closed, which is not the contract either.
        with _Server() as s:
            got = s.call_tool("flapi_update_template", token=TOKEN,
                              endpoint="/hello", content="SELECT 1")
            assert "error" in got
            msg = got["error"]["message"]
            assert "Invalid authentication token" not in msg
            assert "not implemented" in msg

    def test_update_template_does_not_claim_success_it_did_not_achieve(self):
        # It used to report success and leave the file untouched.
        with _Server() as s:
            before = open(s.template).read()
            got = s.call_tool("flapi_update_template", token=TOKEN,
                              endpoint="/hello", content="SELECT 'pwned' AS v")
            assert "result" not in got, (
                f"claimed success without writing: {got}")
            assert open(s.template).read() == before

    def test_an_unauthenticated_read_tool_still_works(self):
        # Only the mutating tools require a token; this pins that the fix did
        # not quietly close the read path too.
        with _Server() as s:
            got = s.call_tool("flapi_get_project_config")
            assert "result" in got, got


class TestNoConfigToolFabricatesAResult:
    """No config tool may report work it did not do.

    `flapi_update_template` was fixed to refuse. Its two siblings, 40 lines
    away in the same file, were not: `flapi_expand_template` and
    `flapi_test_template` both returned the hardcoded literal

        {"expanded_sql": "SELECT * FROM data WHERE 1=1",
         "status": "Template expanded successfully"}

    Both are registered auth_required=false, so any unauthenticated MCP client
    received a fabricated SQL string indistinguishable from a real expansion -
    and `flapi_test_template` claimed a query had passed that it never ran.

    These tests are written against the PROPERTY ("no tool fabricates"), not
    against the two names, because fixing the instance and missing the siblings
    is exactly how this got here.
    """

    UNIMPLEMENTED = ["flapi_update_template", "flapi_expand_template", "flapi_test_template"]

    def test_no_unimplemented_tool_is_advertised(self):
        # tools/list is a contract: an agent that sees a tool will call it.
        with _Server() as s:
            listed = {t["name"] for t in s.list_tools()}
            for name in self.UNIMPLEMENTED:
                assert name not in listed, (
                    f"{name} is advertised but has no implementation")

    def test_every_unimplemented_tool_refuses(self):
        with _Server() as s:
            for name in self.UNIMPLEMENTED:
                got = s.call_tool(name, token=TOKEN,
                                  endpoint="/hello", content="SELECT 1")
                assert "error" in got, f"{name} claimed success: {got}"
                assert "not implemented" in got["error"]["message"], got

    def test_no_tool_returns_the_placeholder_sql(self):
        # The literal itself, so a future stub reusing it is caught whatever
        # it calls itself.
        with _Server() as s:
            for name in self.UNIMPLEMENTED:
                got = s.call_tool(name, token=TOKEN,
                                  endpoint="/hello", content="SELECT 1")
                assert "SELECT * FROM data WHERE 1=1" not in json.dumps(got), (
                    f"{name} returned fabricated SQL: {got}")

    def test_the_implemented_tools_are_still_advertised_and_work(self):
        # The guard must not swallow the working tools.
        with _Server() as s:
            listed = {t["name"] for t in s.list_tools()}
            assert "flapi_get_project_config" in listed, listed
            assert "flapi_get_template" in listed, listed
            got = s.call_tool("flapi_get_project_config")
            assert "result" in got, got
