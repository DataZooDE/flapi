"""A caller must not be able to declare its own identity.

`__auth_*` is the reserved prefix APIServer uses to inject the authenticated
principal into the template context (api_server.cpp:325), surfaced to templates
as `auth.username`, `auth.roles`, `auth.email`, `auth.type` and
`auth.authenticated`.

RequestValidator whitelists the prefix so the injected keys are not reported as
unknown parameters - and combineParameters then copied every query parameter
over the top of the defaults. So the client could simply send them.

Measured before the fix, on an endpoint with NO auth configured:

    GET /a?__auth_username=admin&__auth_roles=admin&__auth_authenticated=true
    -> {"who":"admin","roles":"admin","authed":"true"}

Any template filtering rows on `{{ auth.username }}` or `{{ auth.roles }}` -
the documented multi-tenant pattern - was letting the caller choose who they
were. On an endpoint WITH auth it was worse: the query parameter overwrote the
identity the middleware had just established.
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

SPOOF = {
    "__auth_username": "admin",
    "__auth_roles": "admin",
    "__auth_email": "admin@example.com",
    "__auth_type": "basic",
    "__auth_authenticated": "true",
}


class _Server:
    def __init__(self):
        self.tmp = tempfile.mkdtemp(prefix="flapi_authspoof_")
        self.port = free_port()
        self.base_url = f"http://127.0.0.1:{self.port}"
        self.log_path = os.path.join(self.tmp, "server.log")
        sqls = os.path.join(self.tmp, "sqls")
        os.makedirs(sqls, exist_ok=True)
        with open(os.path.join(sqls, "who.yaml"), "w") as f:
            f.write("url-path: /who\nmethod: GET\n"
                    "template-source: who.sql\nconnection: [inmem]\n"
                    "mcp-tool:\n  name: whoami\n  description: Shows the auth context.\n")
        with open(os.path.join(sqls, "who.sql"), "w") as f:
            f.write("SELECT '{{ auth.username }}' AS who, "
                    "'{{ auth.roles }}' AS roles, "
                    "'{{ auth.authenticated }}' AS authed\n")
        with open(os.path.join(sqls, "w.yaml"), "w") as f:
            f.write("url-path: /w\nmethod: POST\n"
                    "operation:\n  type: write\n  returns-data: false\n  transaction: false\n"
                    "template-source: w.sql\nconnection: [inmem]\n")
        with open(os.path.join(sqls, "w.sql"), "w") as f:
            f.write("CREATE OR REPLACE TABLE seen AS SELECT '{{ auth.username }}' AS who\n")
        # A reader over the table the write endpoint populates. Without this
        # the body test asserted nothing: it POSTed a spoof, then checked
        # /who - which renders the CURRENT request's context and is empty
        # whatever the write stored.
        with open(os.path.join(sqls, "seen.yaml"), "w") as f:
            f.write("url-path: /seen\nmethod: GET\n"
                    "template-source: seen.sql\nconnection: [inmem]\n")
        with open(os.path.join(sqls, "seen.sql"), "w") as f:
            f.write("SELECT who FROM seen\n")
        with open(os.path.join(self.tmp, "flapi.yaml"), "w") as f:
            f.write(
                "project-name: auth-spoof\n"
                "project-description: the caller must not set its own identity\n"
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


class TestAuthContextSpoofing:
    def test_query_parameters_cannot_set_the_auth_context(self):
        # The measured attack, verbatim.
        with _Server() as s:
            r = requests.get(f"{s.base_url}/who", params=SPOOF, timeout=10)
            assert r.status_code == 200, r.text
            row = r.json()["data"][0]
            assert row["who"] == "", row
            assert row["roles"] == "", row
            assert row["authed"] == "", row

    def test_the_unauthenticated_context_is_empty_without_a_spoof(self):
        # Baseline, so the test above cannot pass merely because the template
        # renders nothing under all circumstances.
        with _Server() as s:
            r = requests.get(f"{s.base_url}/who", timeout=10)
            row = r.json()["data"][0]
            assert row == {"who": "", "roles": "", "authed": ""}

    def test_a_json_body_cannot_set_the_auth_context(self):
        # Write operations take parameters from the body, which is a second
        # entry point into the same map.
        #
        # The check reads back what the write actually STORED. An earlier
        # version checked /who instead, which renders the current request's
        # context and is empty whatever the write did - so it passed even
        # when the spoofed identity had been written straight into the table.
        with _Server() as s:
            r = requests.post(f"{s.base_url}/w", json=dict(SPOOF), timeout=10)
            assert r.status_code in (200, 201), r.text

            stored = requests.get(f"{s.base_url}/seen", timeout=10)
            assert stored.status_code == 200, stored.text
            rows = stored.json()["data"]
            assert rows, "the write endpoint stored nothing, so this proves nothing"
            assert rows[0]["who"] == "", (
                f"the spoofed identity reached the database: {rows[0]!r}")

    def test_a_spoof_in_the_query_string_of_a_write_is_also_ignored(self):
        # combineWriteParameters has its own query-parameter fallback, which is
        # a fourth entry point into the same map and was guarded separately.
        with _Server() as s:
            r = requests.post(f"{s.base_url}/w", params=SPOOF, json={}, timeout=10)
            assert r.status_code in (200, 201), r.text
            rows = requests.get(f"{s.base_url}/seen", timeout=10).json()["data"]
            assert rows and rows[0]["who"] == "", rows

    def test_ordinary_parameters_still_work(self):
        # The guard is prefix-scoped; it must not eat normal input.
        with _Server() as s:
            r = requests.get(f"{s.base_url}/who",
                             params={"__auth_username": "admin", "limit": "1"}, timeout=10)
            assert r.status_code == 200, r.text
            assert r.json()["data"][0]["who"] == ""


class TestAuthContextSpoofingOverMcp:
    """The same guard must hold on the MCP surface.

    The first fix stripped `__auth_*` in combineParameters, which is the REST
    path only. MCP arguments reach the template context through
    MCPToolHandler::prepareParameters, which did not strip - so the identical
    attack still worked, measured:

        tools/call {"__auth_username":"admin","__auth_roles":"admin"}
        -> {"who":"admin","roles":"admin"}

    flAPI treats REST and MCP as equal surfaces. A guard on one of them is not
    a guard, and MCP is the surface agents call.
    """

    def _call(self, server, **arguments):
        body = {"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                "params": {"name": "whoami", "arguments": arguments}}
        r = requests.post(f"{server.base_url}/mcp/jsonrpc",
                          headers={"Content-Type": "application/json"},
                          data=json.dumps(body), timeout=15).json()
        assert "result" in r, r
        return r["result"]["structuredContent"]["rows"][0]

    def test_mcp_arguments_cannot_set_the_auth_context(self):
        with _Server() as s:
            row = self._call(s, **SPOOF)
            assert row["who"] == "", row
            assert row["roles"] == "", row

    def test_the_two_surfaces_agree(self):
        # The property that matters: a guard present on one protocol and
        # absent on the other is the bug, so assert they behave the same.
        with _Server() as s:
            mcp = self._call(s, **SPOOF)
            rest = requests.get(f"{s.base_url}/who", params=SPOOF, timeout=10).json()["data"][0]
            assert mcp["who"] == rest["who"] == ""
            assert mcp["roles"] == rest["roles"] == ""


class _AuthedServer(_Server):
    """The same /who endpoint, but behind real authentication.

    Stripping `__auth_*` from caller input is only half the contract. The other
    half is that the SERVER injects the identity it authenticated - otherwise
    `auth.username` is empty for everyone, and the documented multi-tenant
    pattern

        WHERE tenant = '{{ auth.username }}'

    renders `WHERE tenant = ''`, which matches no rows, or - in the far more
    common `{{#auth.username}}...{{/auth.username}}` form - renders no filter
    at all and returns every tenant's rows to any caller.

    REST injects it (api_server.cpp). MCP did not: MCPToolHandler stripped the
    caller's `__auth_*` and then never put the real one back, so `auth.*` was
    unconditionally empty over MCP even for an authenticated caller.
    """

    def __init__(self):
        super().__init__()
        sqls = os.path.join(self.tmp, "sqls")
        # A resource rendering the SAME template, so the two MCP surfaces can
        # be compared directly.
        with open(os.path.join(sqls, "whores.yaml"), "w") as f:
            f.write("url-path: /whores\nmethod: GET\n"
                    "template-source: who.sql\nconnection: [inmem]\n"
                    "mcp-resource:\n  name: whoami_resource\n"
                    "  description: Shows the auth context.\n"
                    "  mime-type: application/json\n"
                    "  allowed-roles:\n    - reader\n")

        # REST auth is per-endpoint; MCP auth is server-wide under `mcp.auth`.
        with open(os.path.join(sqls, "who.yaml"), "w") as f:
            f.write("url-path: /who\nmethod: GET\n"
                    "template-source: who.sql\nconnection: [inmem]\n"
                    "mcp-tool:\n  name: whoami\n  description: Shows the auth context.\n"
                    "  allowed-roles:\n    - reader\n"
                    "auth:\n  enabled: true\n  type: basic\n  users:\n"
                    "    - username: alice\n      password: correct-horse\n"
                    "      roles: [reader]\n")
        with open(os.path.join(self.tmp, "flapi.yaml"), "w") as f:
            f.write(
                "project-name: auth-context-injection\n"
                "project-description: the server must inject the identity it authenticated\n"
                f"http-port: {self.port}\n"
                "template:\n  path: ./sqls\n"
                "connections:\n  inmem:\n    properties:\n      database: ':memory:'\n"
                "mcp:\n  enabled: true\n"
                "  auth:\n    enabled: true\n    type: basic\n    users:\n"
                "      - username: alice\n        password: correct-horse\n"
                "        roles: [reader]\n")


class TestAuthContextIsInjected:
    AUTH = ("alice", "correct-horse")

    def _mcp_whoami(self, server, auth):
        body = {"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                "params": {"name": "whoami", "arguments": {}}}
        r = requests.post(f"{server.base_url}/mcp/jsonrpc",
                          headers={"Content-Type": "application/json"},
                          data=json.dumps(body), auth=auth, timeout=15).json()
        assert "result" in r, r
        return r["result"]["structuredContent"]["rows"][0]

    def test_rest_renders_the_authenticated_identity(self):
        with _AuthedServer() as s:
            r = requests.get(f"{s.base_url}/who", auth=self.AUTH, timeout=10)
            assert r.status_code == 200, r.text
            row = r.json()["data"][0]
            assert row["who"] == "alice", row
            assert row["authed"] == "true", row

    def test_mcp_renders_the_authenticated_identity(self):
        # This is the one that failed: empty `who` for an authenticated caller.
        with _AuthedServer() as s:
            row = self._mcp_whoami(s, self.AUTH)
            assert row["who"] == "alice", (
                "MCP did not inject the authenticated identity; a template "
                f"filtering on auth.username sees nothing: {row!r}")
            assert row["roles"] == "reader", row
            assert row["authed"] == "true", row

    def test_the_two_surfaces_agree_on_who_the_caller_is(self):
        # The property, stated directly: REST and MCP are equal surfaces, so
        # the same credential must produce the same identity on both.
        with _AuthedServer() as s:
            mcp = self._mcp_whoami(s, self.AUTH)
            rest = requests.get(f"{s.base_url}/who", auth=self.AUTH,
                                timeout=10).json()["data"][0]
            assert mcp["who"] == rest["who"] == "alice", (mcp, rest)
            assert mcp["authed"] == rest["authed"] == "true", (mcp, rest)

    def test_a_spoof_cannot_override_the_injected_identity(self):
        # The strip must win over caller input, not merely run before it.
        with _AuthedServer() as s:
            body = {"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                    "params": {"name": "whoami", "arguments": dict(SPOOF)}}
            r = requests.post(f"{s.base_url}/mcp/jsonrpc",
                              headers={"Content-Type": "application/json"},
                              data=json.dumps(body), auth=self.AUTH, timeout=15).json()
            row = r["result"]["structuredContent"]["rows"][0]
            assert row["who"] == "alice", row
            assert row["roles"] == "reader", row


class TestAuthContextOnMcpResources:
    """resources/read is a second MCP surface, and it had the same hole.

    The tools/call fix was made inline in MCPToolHandler::prepareParameters.
    resources/read authenticates, applies per-resource RBAC, and then passed
    its bound URI-template params straight into executeQuery - no
    `__auth_*` strip, no injection. So `auth.*` was unconditionally empty
    here, and the documented

        {{#auth.username}}WHERE tenant = '{{ auth.username }}'{{/auth.username}}

    filter rendered NOTHING and returned every tenant's rows to any
    authenticated caller. Identical failure, one protocol method over.

    Both surfaces now call applyMcpAuthContext.
    """

    AUTH = ("alice", "correct-horse")

    def _read(self, server, auth):
        body = {"jsonrpc": "2.0", "id": 1, "method": "resources/read",
                "params": {"uri": "flapi://whoami_resource"}}
        r = requests.post(f"{server.base_url}/mcp/jsonrpc",
                          headers={"Content-Type": "application/json"},
                          data=json.dumps(body), auth=auth, timeout=15).json()
        assert "result" in r, r
        text = r["result"]["contents"][0]["text"]
        return json.loads(text)

    def test_a_resource_renders_the_authenticated_identity(self):
        with _AuthedServer() as s:
            payload = self._read(s, self.AUTH)
            blob = json.dumps(payload)
            assert "alice" in blob, (
                "resources/read did not inject the authenticated identity; a "
                f"template filtering on auth.username sees nothing: {blob}")

    def test_the_resource_and_tool_surfaces_agree(self):
        # The property: REST, tools/call and resources/read are equal
        # surfaces, so one credential produces one identity on all of them.
        with _AuthedServer() as s:
            resource = json.dumps(self._read(s, self.AUTH))
            body = {"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                    "params": {"name": "whoami", "arguments": {}}}
            tool = requests.post(f"{s.base_url}/mcp/jsonrpc",
                                 headers={"Content-Type": "application/json"},
                                 data=json.dumps(body), auth=self.AUTH,
                                 timeout=15).json()
            tool_row = tool["result"]["structuredContent"]["rows"][0]
            assert tool_row["who"] == "alice", tool_row
            assert "alice" in resource, resource
