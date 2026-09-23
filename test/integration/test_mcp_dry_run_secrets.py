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
    def __init__(self, short_secret: bool = False):
        self.short_secret = short_secret
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
                    "'{{{ params.access_token }}}' AS tok\n")
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
             "-p", str(self.port), "--log-level", "warning"],
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
