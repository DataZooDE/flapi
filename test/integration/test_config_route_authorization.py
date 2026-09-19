"""GET /config must not hand out the configuration to anyone who asks.

The route returned the entire configuration - every endpoint, and every
connection's `init` SQL and `properties` - with no token, and it was registered
even when the config service was switched off. So a default deployment
disclosed its own credentials to any caller who could reach the port.

Verified against a running server before the fix: a password in
connections.*.properties came back in the body of an unauthenticated GET.

Two defences, because either alone is insufficient:

- The route now requires the config-service token, like every other
  configuration route.
- Credential-looking properties are redacted at the source, so a future caller
  of getFlapiConfig() cannot reintroduce the leak.

Redaction alone would not have been enough: a credential can sit inside the
`init` SQL - the shipped SAP example puts a PASSWD literal there - where no
key-name predicate can find it. The token is what actually closes that.
"""
import os
import subprocess
import tempfile
import time

import pytest
import requests

from otel_helpers import flapi_binary, free_port

pytestmark = pytest.mark.standalone_server

TOKEN = "the-real-config-token"
SECRET = "super-secret-connection-password"


class _Server:
    def __init__(self, config_service: bool):
        self.tmp = tempfile.mkdtemp(prefix="flapi_cfgroute_")
        self.port = free_port()
        self.base_url = f"http://127.0.0.1:{self.port}"
        self.log_path = os.path.join(self.tmp, "server.log")
        self.config_service = config_service
        sqls = os.path.join(self.tmp, "sqls")
        os.makedirs(sqls, exist_ok=True)
        with open(os.path.join(sqls, "hello.yaml"), "w") as f:
            f.write("url-path: /hello\nmethod: GET\n"
                    "template-source: hello.sql\nconnection: [inmem]\n")
        with open(os.path.join(sqls, "hello.sql"), "w") as f:
            f.write("SELECT 1 AS v\n")
        with open(os.path.join(self.tmp, "flapi.yaml"), "w") as f:
            f.write(
                "project-name: config-route-test\n"
                "project-description: /config must require the token\n"
                f"http-port: {self.port}\n"
                "template:\n  path: ./sqls\n"
                "connections:\n  inmem:\n    properties:\n"
                "      database: ':memory:'\n"
                f"      secret_password: '{SECRET}'\n")
        self.proc = None

    def start(self):
        cmd = [flapi_binary(), "-c", os.path.join(self.tmp, "flapi.yaml"),
               "-p", str(self.port), "--log-level", "warning"]
        if self.config_service:
            cmd += ["--config-service", "--config-service-token", TOKEN]
        self.proc = subprocess.Popen(
            cmd, stdout=open(self.log_path, "w"), stderr=subprocess.STDOUT,
            cwd=self.tmp, env={**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"},
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


class TestConfigRouteAuthorization:
    def test_unauthenticated_get_is_refused_on_a_default_server(self):
        # The exact shape of the disclosure: no --config-service, no token,
        # and the full configuration came back with a 200.
        with _Server(config_service=False) as s:
            r = requests.get(f"{s.base_url}/config", timeout=10)
            assert r.status_code == 401, r.text[:300]
            assert SECRET not in r.text

    def test_unauthenticated_get_is_refused_with_the_service_enabled(self):
        with _Server(config_service=True) as s:
            r = requests.get(f"{s.base_url}/config", timeout=10)
            assert r.status_code == 401
            assert SECRET not in r.text

    def test_a_wrong_token_is_refused(self):
        with _Server(config_service=True) as s:
            r = requests.get(f"{s.base_url}/config",
                             headers={"X-Config-Token": "not-the-token"}, timeout=10)
            assert r.status_code == 401
            assert SECRET not in r.text

    def test_the_correct_token_is_accepted_and_still_redacts(self):
        # Defence in depth: an authorised caller gets the configuration, but
        # credential-looking properties are redacted even for them.
        with _Server(config_service=True) as s:
            r = requests.get(f"{s.base_url}/config",
                             headers={"X-Config-Token": TOKEN}, timeout=10)
            assert r.status_code == 200, r.text[:300]
            assert SECRET not in r.text, "the credential survived redaction"
            assert "<redacted>" in r.text
            # A non-credential property must NOT be redacted - over-broad
            # redaction would make the endpoint useless for its actual purpose.
            assert ":memory:" in r.text

    def test_delete_is_refused_without_the_token(self):
        with _Server(config_service=False) as s:
            r = requests.delete(f"{s.base_url}/config", timeout=10)
            assert r.status_code == 401
