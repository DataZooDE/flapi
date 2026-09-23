"""_dryRun must not hand back credentials embedded in the rendered SQL.

An MCP `tools/call` with `_dryRun: true` returns the fully rendered SQL without
executing it. A template that interpolates a connection property - the
documented way to reach `{{{ conn.path }}}` and friends - therefore returned
whatever that property held, and MCP is unauthenticated by default.

Measured before the fix:

    "rendered_sql":"SELECT 'connected' AS status, 'CONNECTION-PASSWORD-LEAKED' AS pw"

Credential-valued properties are now scrubbed from the rendered SQL by VALUE,
because by the time the SQL exists the key is gone. The same key predicate as
the audit log and the span path decides what counts as a credential.
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

SECRET = "CONNECTION-PASSWORD-LEAKED"
# Short secrets: the scrub used to skip anything under four bytes, so a PIN
# or a short token was returned verbatim. The long sentinel above could never
# reach that branch.
# A three-byte secret: under the length at which value-based scrubbing is
# safe, so the preview must be withheld rather than scrubbed.
NOT_SECRET = "/data/public/path.parquet"
# A whitelisted environment variable a template may interpolate as
# `{{{ env.API_KEY }}}`. Scrubbing covered conn.* only, so this came back
# verbatim to an unauthenticated caller.
ENV_SECRET = "ENV-API-KEY-LEAKED"
# A request field's configured default, copied into params and echoed in the
# payload's `parameters` object even when the SQL itself was clean.
DEFAULT_TOKEN = "DEFAULT-ACCESS-TOKEN-LEAKED"


class _Server:
    def __init__(self, short_secret: bool = False, unbindable: bool = False,
                 config_service_token: str = None):
        # When set, the config service is enabled with this token and the
        # flapi_* tools are advertised. Off by default, which is also the
        # shipped default.
        self.config_service_token = config_service_token
        self.short_secret = short_secret
        # When true the rendered SQL is valid to render but fails to BIND, so
        # the database error quotes it back - the error-path disclosure.
        self.unbindable = unbindable
        self.tmp = tempfile.mkdtemp(prefix="flapi_dryrun_")
        self.port = free_port()
        self.base_url = f"http://127.0.0.1:{self.port}"
        self.log_path = os.path.join(self.tmp, "server.log")
        sqls = os.path.join(self.tmp, "sqls")
        os.makedirs(sqls, exist_ok=True)
        with open(os.path.join(sqls, "t.yaml"), "w") as f:
            f.write("url-path: /t\nmethod: GET\n"
                    "template-source: t.sql\nconnection: [creds]\n"
                    # A credential-valued `default:` is copied into params and
                    # echoed back in the payload's `parameters` object.
                    "request:\n"
                    "  - field-name: access_token\n"
                    "    field-in: query\n"
                    f"    default: '{DEFAULT_TOKEN}'\n"
                    "mcp-tool:\n  name: leaky\n  description: Embeds connection properties.\n")
        with open(os.path.join(sqls, "t.sql"), "w") as f:
            f.write("SELECT '{{{ conn.password }}}' AS pw, '{{{ conn.path }}}' AS p, "
                    "'{{{ conn.pin_password }}}' AS short, "
                    "'{{{ env.API_KEY }}}' AS ak, "
                    "'{{{ params.access_token }}}' AS tok"
                    + (" FROM no_such_table_here" if self.unbindable else "")
                    + "\n")
        with open(os.path.join(self.tmp, "flapi.yaml"), "w") as f:
            f.write(
                "project-name: dryrun-secrets\n"
                "project-description: dry run must not return credentials\n"
                f"http-port: {self.port}\n"
                "template:\n  path: ./sqls\n"
                # The documented whitelisted-environment-variable pattern.
                "  environment-whitelist:\n    - '^API_KEY$'\n"
                "connections:\n  creds:\n    properties:\n"
                "      database: ':memory:'\n"
                f"      password: '{SECRET}'\n"
                + ("      pin_password: 'abc'\n" if self.short_secret else "")
                + f"      path: '{NOT_SECRET}'\n"
                "mcp:\n  enabled: true\n")
        self.proc = None

    def start(self):
        self.proc = subprocess.Popen(
            [flapi_binary(), "-c", os.path.join(self.tmp, "flapi.yaml"),
             "-p", str(self.port), "--log-level", "warning"]
            + (["--config-service", "--config-service-token",
                getattr(self, "config_service_token", "")]
               if getattr(self, "config_service_token", None) else []),
            stdout=open(self.log_path, "w"), stderr=subprocess.STDOUT, cwd=self.tmp,
            env={**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1",
                 "API_KEY": ENV_SECRET},
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

    def call(self, **arguments):
        """A normal tools/call - no _dryRun - returning the raw JSON-RPC."""
        body = {"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                "params": {"name": "leaky", "arguments": arguments}}
        return requests.post(f"{self.base_url}/mcp/jsonrpc",
                             headers={"Content-Type": "application/json"},
                             data=json.dumps(body), timeout=15).json()

    def dry_run(self):
        body = {"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                "params": {"name": "leaky", "arguments": {"_dryRun": True}}}
        r = requests.post(f"{self.base_url}/mcp/jsonrpc",
                          headers={"Content-Type": "application/json"},
                          data=json.dumps(body), timeout=15).json()
        return json.loads(r["result"]["content"][0]["text"])

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


class TestDryRunSecrets:
    def test_a_credential_property_is_not_returned(self):
        with _Server() as s:
            sql = s.dry_run()["rendered_sql"]
            assert SECRET not in sql, sql
            assert "<redacted>" in sql

    def test_a_non_credential_property_is_left_alone(self):
        # Over-broad scrubbing would make dry run useless for its actual
        # purpose - seeing the SQL that would run.
        with _Server() as s:
            sql = s.dry_run()["rendered_sql"]
            assert NOT_SECRET in sql, sql

    def test_the_sql_is_still_recognisable(self):
        with _Server() as s:
            sql = s.dry_run()["rendered_sql"]
            assert sql.strip().upper().startswith("SELECT")
            assert "AS pw" in sql


    def test_a_short_credential_withholds_the_preview(self):
        # A value under four bytes cannot be replaced without corrupting
        # unrelated SQL - measured: a one-byte secret rewrote the middle of a
        # file path. The first version simply skipped those, so a short
        # password or PIN came back verbatim to any unauthenticated _dryRun
        # caller. Neither leaking nor mangling is acceptable, so the preview is
        # withheld and the caller is told why.
        with _Server(short_secret=True) as s:
            sql = s.dry_run()["rendered_sql"]
            assert "abc" not in sql, f"the 3-byte secret survived: {sql!r}"
            assert "withheld" in sql, sql

    def test_a_connection_without_short_credentials_still_gets_a_preview(self):
        # Withholding must be the exception, or dry run stops being useful.
        with _Server() as s:
            sql = s.dry_run()["rendered_sql"]
            assert "withheld" not in sql, sql
            assert sql.strip().upper().startswith("SELECT")


class TestDryRunSecretsFromEverySource:
    """A template can interpolate three things. Scrubbing covered one.

    `conn.*` was scrubbed. `env.*` - the documented
    `{{{ env.API_KEY }}}` pattern - was not, and neither was a request
    field's configured `default:`, which is copied into params and echoed
    back in the payload's own `parameters` object. MCP is unauthenticated by
    default, so both were readable by anyone who could reach the port.

    These tests name the SOURCE rather than the one property that was fixed,
    because fixing the instance and missing the siblings is how this got here.
    """

    def test_a_whitelisted_environment_secret_is_not_returned(self):
        with _Server() as s:
            payload = s.dry_run()
            assert ENV_SECRET not in json.dumps(payload), payload

    def test_a_credential_valued_default_is_not_returned_in_the_sql(self):
        with _Server() as s:
            sql = s.dry_run()["rendered_sql"]
            assert DEFAULT_TOKEN not in sql, sql

    def test_a_credential_valued_default_is_not_echoed_in_parameters(self):
        # The echo is a second disclosure path, independent of the SQL.
        with _Server() as s:
            payload = s.dry_run()
            params = payload.get("parameters", {})
            assert DEFAULT_TOKEN not in json.dumps(params), params
            assert params.get("access_token") == "<redacted>", params

    def test_no_secret_from_any_source_survives_anywhere_in_the_payload(self):
        # The property, stated once over the whole response.
        with _Server() as s:
            blob = json.dumps(s.dry_run())
            for secret in (SECRET, ENV_SECRET, DEFAULT_TOKEN):
                assert secret not in blob, f"{secret} leaked: {blob}"

    def test_the_preview_is_still_useful(self):
        # Over-broad scrubbing would defeat the point of a dry run.
        with _Server() as s:
            sql = s.dry_run()["rendered_sql"]
            assert NOT_SECRET in sql, sql
            assert sql.strip().upper().startswith("SELECT")


class TestSecretsDoNotLeakThroughTheErrorPath:
    """The preview was scrubbed. The error was not.

    DuckDB quotes the failing statement verbatim in its message:

        Binder Error: Table with name no_such_table_here does not exist!
        LINE 1: SELECT 'CONNECTION-PASSWORD-LEAKED' AS pw ...

    and that message went straight back through createErrorResult. MCP is
    unauthenticated by default, so appending an unbindable table to a template
    and calling it WITHOUT `_dryRun` recovered every secret the preview path
    had just been fixed to withhold - the same defect class as F4, ~120 lines
    away in the same function.

    The secret set is now collected for every path, not just the preview, and
    the error message is scrubbed with it.
    """

    def test_a_database_error_does_not_quote_connection_secrets(self):
        with _Server(unbindable=True) as s:
            got = s.call()
            blob = json.dumps(got)
            assert "error" in got or "result" in got, blob
            assert SECRET not in blob, f"the connection secret leaked: {blob}"

    def test_a_database_error_does_not_quote_environment_secrets(self):
        with _Server(unbindable=True) as s:
            assert ENV_SECRET not in json.dumps(s.call()), s.call()

    def test_a_database_error_does_not_quote_credential_defaults(self):
        with _Server(unbindable=True) as s:
            assert DEFAULT_TOKEN not in json.dumps(s.call()), s.call()

    def test_the_error_is_still_useful(self):
        # Scrubbing must not reduce every failure to an opaque blob.
        with _Server(unbindable=True) as s:
            blob = json.dumps(s.call()).lower()
            assert "no_such_table_here" in blob or "binder" in blob, blob

    def test_a_successful_call_is_unaffected(self):
        with _Server() as s:
            got = s.call()
            assert "result" in got, got


class TestTheConfigToolsCannotBeUsedAsASecondDoor:
    """The config tools reach the same templates and the same environment.

    Wiring them to their real handlers made three of them a way to the very
    values this file exists to keep in: `flapi_get_environment` returns
    environment values, `flapi_expand_template` returns the rendered SQL, and
    `flapi_test_template` executes it. They were all `auth_required = false`
    at the time, so an unauthenticated MCP caller could read what `_dryRun`
    had just been fixed to withhold.

    These assert against the SAME seeded secrets as the dry-run tests above,
    so the two paths cannot diverge.
    """

    TOKEN = "config-tools-token"

    def _call(self, server, name, token=None, **arguments):
        headers = {"Content-Type": "application/json"}
        if token:
            headers["Authorization"] = f"Bearer {token}"
        body = {"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                "params": {"name": name, "arguments": arguments}}
        return requests.post(f"{server.base_url}/mcp/jsonrpc", headers=headers,
                             data=json.dumps(body), timeout=15).json()

    def test_environment_values_are_not_readable_without_a_token(self):
        with _Server(config_service_token=self.TOKEN) as s:
            got = self._call(s, "flapi_get_environment")
            assert ENV_SECRET not in json.dumps(got), got

    def test_a_template_cannot_be_expanded_without_a_token(self):
        with _Server(config_service_token=self.TOKEN) as s:
            got = self._call(s, "flapi_expand_template", endpoint="/t", params={})
            blob = json.dumps(got)
            for secret in (SECRET, ENV_SECRET, DEFAULT_TOKEN):
                assert secret not in blob, f"{secret} leaked: {blob}"

    def test_a_template_cannot_be_executed_without_a_token(self):
        with _Server(config_service_token=self.TOKEN) as s:
            got = self._call(s, "flapi_test_template", endpoint="/t", params={})
            blob = json.dumps(got)
            for secret in (SECRET, ENV_SECRET, DEFAULT_TOKEN):
                assert secret not in blob, f"{secret} leaked: {blob}"

    def test_no_config_tool_is_advertised_when_the_service_is_off(self):
        # The shipped default. The config tools are the config service's own
        # operations, so with it disabled they should not be on the menu.
        with _Server() as s:
            listed = requests.post(
                f"{s.base_url}/mcp/jsonrpc",
                headers={"Content-Type": "application/json"},
                data=json.dumps({"jsonrpc": "2.0", "id": 1,
                                 "method": "tools/list", "params": {}}),
                timeout=15).json()
            names = [t["name"] for t in listed["result"]["tools"]]
            assert names, "tools/list returned nothing at all"
            for name in names:
                blob = json.dumps(self._call(s, name, endpoint="/t", path="/t",
                                             params={}, content="SELECT 1"))
                for secret in (SECRET, ENV_SECRET, DEFAULT_TOKEN):
                    assert secret not in blob, f"{name} leaked {secret}: {blob}"

    def test_no_config_tool_leaks_a_secret_without_a_token(self):
        # The property over the whole surface, rather than three names, with
        # the config service actually on.
        with _Server(config_service_token=self.TOKEN) as s:
            listed = requests.post(
                f"{s.base_url}/mcp/jsonrpc",
                headers={"Content-Type": "application/json"},
                data=json.dumps({"jsonrpc": "2.0", "id": 1,
                                 "method": "tools/list", "params": {}}),
                timeout=15).json()
            names = [t["name"] for t in listed["result"]["tools"]
                     if t["name"].startswith("flapi_")]
            assert names, "no flapi_* tools advertised; this proves nothing"
            for name in names:
                blob = json.dumps(self._call(s, name, endpoint="/t", path="/t",
                                             params={}, content="SELECT 1"))
                for secret in (SECRET, ENV_SECRET, DEFAULT_TOKEN):
                    assert secret not in blob, f"{name} leaked {secret}: {blob}"
