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
                              endpoint="/hello", content="SELECT 1 AS v")
            assert "result" in got, got

    def test_update_template_writes_what_it_says_it_wrote(self):
        # This tool used to report success and leave the file on disk
        # untouched - the one outcome a caller cannot detect. It now
        # delegates to the same handler PUT .../template uses, so the
        # assertion is the file itself.
        with _Server() as s:
            before = open(s.template).read()
            got = s.call_tool("flapi_update_template", token=TOKEN,
                              endpoint="/hello", content="SELECT 'written' AS v")
            assert "result" in got, got
            after = open(s.template).read()
            assert after != before, "reported success and wrote nothing"
            assert "written" in after, after

    def test_update_template_without_a_token_writes_nothing(self):
        # The auth gate has to hold now that the tool actually mutates.
        with _Server() as s:
            before = open(s.template).read()
            got = s.call_tool("flapi_update_template",
                              endpoint="/hello", content="SELECT 'pwned' AS v")
            assert "error" in got, got
            assert open(s.template).read() == before

    def test_even_a_read_tool_requires_the_token(self):
        # This used to assert the opposite - that a read tool stayed
        # anonymous - on the theory that reading is harmless. It is not: these
        # tools read the project configuration, the environment, and the
        # rendered SQL, and every equivalent REST route is token-gated. The
        # config service is opt-in; MCP must not be a second, weaker door to
        # the same handlers.
        with _Server() as s:
            got = s.call_tool("flapi_get_project_config")
            assert "error" in got, got
            assert "Authentication required" in got["error"]["message"], got


class TestNoConfigToolFabricatesAResult:
    """No config tool may report work it did not do.

    Eleven of them did. `flapi_expand_template` and `flapi_test_template`
    answered with a hardcoded `SELECT * FROM data WHERE 1=1` and "Template
    test passed"; `flapi_refresh_cache` said a refresh "has been scheduled"
    with nothing scheduled; `flapi_get_cache_audit` INVENTED audit rows;
    `flapi_get_environment`, `flapi_get_filesystem` and `flapi_get_schema`
    returned empty data as success; `flapi_get_project_config` reported
    version "1.0.0" for every build.

    Every one of them had a working REST handler that the adapter was
    constructing and then discarding. They now delegate to it, so the MCP and
    REST surfaces cannot disagree.

    These tests enumerate tools/list rather than naming tools, because the
    first version of this class was a literal three-name list and that is
    exactly why eight more fabricating tools survived a review round.
    """

    # Vocabulary that denotes work DEFERRED or FAKED rather than done.
    # "successfully" is deliberately absent: the endpoint tools genuinely do
    # their work and say so, and a heuristic that flags them teaches people
    # to ignore it.
    CLAIM_WORDS = ("triggered", "scheduled", "sample",
                   "will be processed", "has been queued")

    # Tools that destroy the fixture they are called against. Run last, so
    # the tools visited after them are not all answering "not found" - the
    # first version of this sweep iterated an unordered_map-derived list and
    # called flapi_delete_endpoint somewhere in the middle, so WHICH tools it
    # really exercised varied per build.
    DESTRUCTIVE = ("flapi_delete_endpoint",)

    def _sweep_order(self, names):
        ordinary = sorted(n for n in names if n not in self.DESTRUCTIVE)
        return ordinary + [n for n in sorted(names) if n in self.DESTRUCTIVE]

    def test_no_advertised_tool_claims_work_without_doing_it(self):
        with _Server() as s:
            listed = self._sweep_order([t["name"] for t in s.list_tools()])
            assert listed, "tools/list returned nothing; the test proves nothing"

            offenders = []
            answered = 0
            for name in listed:
                got = s.call_tool(name, token=TOKEN, endpoint="/hello",
                                  content="SELECT 1", path="/hello")
                if "result" not in got:
                    continue
                answered += 1
                blob = json.dumps(got["result"]).lower()
                for word in self.CLAIM_WORDS:
                    if word in blob:
                        offenders.append((name, word, blob[:200]))
                        break
            assert not offenders, (
                "tools advertised by tools/list claim deferred work:\n" +
                "\n".join(f"  {n}: says {w!r} -> {b}" for n, w, b in offenders))
            # A build where every tool errors would otherwise satisfy this
            # sweep perfectly.
            assert answered >= 8, (
                f"only {answered} of {len(listed)} tools returned a result; "
                "this sweep cannot judge tools that never answered")

    def test_a_tool_that_reports_success_actually_changed_something(self):
        # The word heuristic is a widening, not the contract. This is the
        # contract, spot-checked on a mutating tool.
        with _Server() as s:
            listed_before = s.call_tool("flapi_list_endpoints", token=TOKEN)
            assert "/hello" in json.dumps(listed_before), listed_before

            got = s.call_tool("flapi_delete_endpoint", token=TOKEN, path="/hello")
            assert "result" in got, got

            listed_after = s.call_tool("flapi_list_endpoints", token=TOKEN)
            assert "/hello" not in json.dumps(listed_after), (
                "flapi_delete_endpoint reported success and the endpoint is "
                f"still listed: {listed_after}")

    def test_no_advertised_tool_returns_the_placeholder_sql(self):
        with _Server() as s:
            for name in self._sweep_order([t["name"] for t in s.list_tools()]):
                got = s.call_tool(name, token=TOKEN, endpoint="/hello",
                                  content="SELECT 1", path="/hello")
                assert "SELECT * FROM data WHERE 1=1" not in json.dumps(got), (
                    f"{name} returned fabricated SQL: {got}")

    def test_no_advertised_tool_invents_audit_records(self):
        # An invented audit row is the worst thing a tool can return: it is
        # the record someone consults to find out whether work happened.
        with _Server() as s:
            for name in self._sweep_order([t["name"] for t in s.list_tools()]):
                got = s.call_tool(name, token=TOKEN, endpoint="/hello",
                                  content="SELECT 1", path="/hello")
                assert "cache_status_checked" not in json.dumps(got), (
                    f"{name} returned a manufactured audit entry: {got}")

    def test_expand_template_returns_this_endpoints_sql(self):
        # The specific fabrication, pinned: the answer must come from the
        # endpoint's own template, not from a constant.
        with _Server() as s:
            got = s.call_tool("flapi_expand_template", token=TOKEN,
                              endpoint="/hello", params={})
            blob = json.dumps(got)
            assert "SELECT * FROM data WHERE 1=1" not in blob, blob
            # hello.sql is `SELECT 'original-template' AS v` in this fixture.
            assert "original-template" in blob, blob

    def test_the_project_config_version_is_not_hardcoded(self):
        with _Server() as s:
            got = s.call_tool("flapi_get_project_config", token=TOKEN)
            assert "result" in got, got
            assert '"1.0.0"' not in json.dumps(got["result"]), (
                "flapi_get_project_config still reports a hardcoded version")

    def test_the_implemented_tools_are_advertised_and_work(self):
        with _Server() as s:
            listed = {t["name"] for t in s.list_tools()}
            for name in ("flapi_get_project_config", "flapi_get_template",
                         "flapi_list_endpoints", "flapi_expand_template",
                         "flapi_get_schema", "flapi_get_environment"):
                assert name in listed, (name, listed)
            got = s.call_tool("flapi_get_project_config", token=TOKEN)
            assert "result" in got, got


class TestTheMutatingCacheToolsWithAToken:
    """The authenticated path for the mutating tools.

    A previous version of the sibling test accepted either "not implemented"
    OR "Authentication required", and the client sent no token - so for
    `flapi_update_template`, `flapi_refresh_cache` and `flapi_run_cache_gc`
    it could only ever reach the auth branch. The behaviour of exactly the
    tools where fabricated success was worst was untested.
    """

    def test_refresh_cache_with_a_token_does_not_report_a_phantom_schedule(self):
        # There is no cache configured on /hello, so the honest answer is an
        # error naming that - never "Cache refresh has been scheduled", which
        # is what it used to say for any endpoint at all.
        with _Server() as s:
            got = s.call_tool("flapi_refresh_cache", token=TOKEN, path="/hello")
            blob = json.dumps(got)
            assert "has been scheduled" not in blob, blob
            assert "Cache refresh triggered" not in blob, blob
            # It reached the tool rather than stopping at the gate.
            assert "Authentication required" not in blob, blob

    def test_run_cache_gc_with_a_token_does_not_report_a_phantom_collection(self):
        with _Server() as s:
            got = s.call_tool("flapi_run_cache_gc", token=TOKEN, path="/hello")
            blob = json.dumps(got)
            assert "Garbage collection triggered" not in blob, blob
            assert "Authentication required" not in blob, blob

    def test_the_cache_tools_agree_with_the_rest_routes(self):
        # The point of delegating: one implementation, so the two surfaces
        # cannot answer differently.
        with _Server() as s:
            mcp = s.call_tool("flapi_get_cache_status", token=TOKEN, path="/hello")
            rest = requests.get(
                f"{s.base_url}/api/v1/_config/endpoints/-hello/cache",
                headers={"X-Config-Token": TOKEN}, timeout=10)
            if "result" in mcp:
                mcp_body = mcp["result"]["content"][0]["text"]
                assert rest.status_code == 200, rest.text
                assert json.loads(mcp_body) == rest.json(), (mcp_body, rest.text)
            else:
                # Both must refuse, and for the same reason.
                assert rest.status_code >= 400, rest.text
