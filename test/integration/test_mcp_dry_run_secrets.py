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
NOT_SECRET = "/data/public/path.parquet"


class _Server:
    def __init__(self):
        self.tmp = tempfile.mkdtemp(prefix="flapi_dryrun_")
        self.port = free_port()
        self.base_url = f"http://127.0.0.1:{self.port}"
        self.log_path = os.path.join(self.tmp, "server.log")
        sqls = os.path.join(self.tmp, "sqls")
        os.makedirs(sqls, exist_ok=True)
        with open(os.path.join(sqls, "t.yaml"), "w") as f:
            f.write("url-path: /t\nmethod: GET\n"
                    "template-source: t.sql\nconnection: [creds]\n"
                    "mcp-tool:\n  name: leaky\n  description: Embeds connection properties.\n")
        with open(os.path.join(sqls, "t.sql"), "w") as f:
            f.write("SELECT '{{{ conn.password }}}' AS pw, '{{{ conn.path }}}' AS p\n")
        with open(os.path.join(self.tmp, "flapi.yaml"), "w") as f:
            f.write(
                "project-name: dryrun-secrets\n"
                "project-description: dry run must not return credentials\n"
                f"http-port: {self.port}\n"
                "template:\n  path: ./sqls\n"
                "connections:\n  creds:\n    properties:\n"
                "      database: ':memory:'\n"
                f"      password: '{SECRET}'\n"
                f"      path: '{NOT_SECRET}'\n"
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
