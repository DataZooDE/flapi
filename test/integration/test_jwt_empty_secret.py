"""A token signed with an empty key must not authenticate.

`jwt-secret: '{{env.API_JWT_SECRET}}'` resolves to "" when the variable is
unset - silently. HS256 with a zero-length key is not a weak secret, it is no
secret: anyone can sign a token and it verifies.

Measured against the shipped binary before the fix, with the variable unset:

    Authorization: Bearer <forged, empty key, roles=["admin"]>
    -> 200 {"data":[{"v":"protected-data"}]}

The forged token claimed a subject and a role list of its own choosing, so this
was a complete authentication bypass reachable by forgetting an environment
variable - which produces no error anywhere.
"""
import base64
import json
import hashlib
import hmac
import os
import subprocess
import tempfile
import time

import pytest
import requests

from otel_helpers import flapi_binary, free_port

pytestmark = pytest.mark.standalone_server

REAL_SECRET = "a-real-secret-value"


def make_token(key: str, roles=("admin",), issuer="flapi") -> str:
    def b64(raw: bytes) -> str:
        return base64.urlsafe_b64encode(raw).rstrip(b"=").decode()

    header = b64(json.dumps({"alg": "HS256", "typ": "JWT"}, separators=(",", ":")).encode())
    payload = b64(json.dumps(
        {"sub": "attacker", "iss": issuer, "roles": list(roles),
         "exp": int(time.time()) + 3600}, separators=(",", ":")).encode())
    signing_input = f"{header}.{payload}".encode()
    sig = b64(hmac.new(key.encode(), signing_input, hashlib.sha256).digest())
    return f"{header}.{payload}.{sig}"


class _Server:
    def __init__(self, secret_env_value=None):
        self.tmp = tempfile.mkdtemp(prefix="flapi_jwtsecret_")
        self.port = free_port()
        self.base_url = f"http://127.0.0.1:{self.port}"
        self.log_path = os.path.join(self.tmp, "server.log")
        self.secret_env_value = secret_env_value
        sqls = os.path.join(self.tmp, "sqls")
        os.makedirs(sqls, exist_ok=True)
        with open(os.path.join(sqls, "s.yaml"), "w") as f:
            f.write("url-path: /s\nmethod: GET\n"
                    "template-source: s.sql\nconnection: [inmem]\n"
                    "auth:\n  enabled: true\n  type: bearer\n"
                    "  jwt-secret: '{{env.FLAPI_TEST_JWT_SECRET}}'\n"
                    "  jwt-issuer: 'flapi'\n")
        with open(os.path.join(sqls, "s.sql"), "w") as f:
            f.write("SELECT 'protected-data' AS v\n")
        with open(os.path.join(self.tmp, "flapi.yaml"), "w") as f:
            f.write(
                "project-name: jwt-secret-test\n"
                "project-description: an empty HMAC key must not authenticate\n"
                f"http-port: {self.port}\n"
                "template:\n  path: ./sqls\n"
                "  environment-whitelist:\n    - '^FLAPI_.*'\n"
                "connections:\n  inmem:\n    properties:\n      database: ':memory:'\n")
        self.proc = None

    def start(self):
        env = {**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"}
        env.pop("FLAPI_TEST_JWT_SECRET", None)
        if self.secret_env_value is not None:
            env["FLAPI_TEST_JWT_SECRET"] = self.secret_env_value
        self.proc = subprocess.Popen(
            [flapi_binary(), "-c", os.path.join(self.tmp, "flapi.yaml"),
             "-p", str(self.port), "--log-level", "warning"],
            stdout=open(self.log_path, "w"), stderr=subprocess.STDOUT,
            cwd=self.tmp, env=env, preexec_fn=os.setsid)
        deadline = time.time() + 90
        while time.time() < deadline:
            try:
                if requests.get(f"{self.base_url}/health/live", timeout=2).status_code == 200:
                    return self
            except requests.RequestException:
                pass
            time.sleep(0.2)
        pytest.fail(f"server did not start:\n{open(self.log_path).read()[-3000:]}")

    def get(self, token=None):
        headers = {"Authorization": f"Bearer {token}"} if token else {}
        return requests.get(f"{self.base_url}/s", headers=headers, timeout=10)

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


class TestJwtEmptySecret:
    def test_an_empty_key_forgery_is_refused(self):
        # The measured bypass, verbatim.
        with _Server(secret_env_value=None) as s:
            r = s.get(make_token(""))
            assert r.status_code == 401, r.text[:200]
            assert "protected-data" not in r.text

    def test_the_startup_warning_names_the_problem(self):
        with _Server(secret_env_value=None) as s:
            log = open(s.log_path).read()
            assert "AUTH_EMPTY_JWT_SECRET" in log, log[-1500:]

    def test_a_correctly_signed_token_still_authenticates(self):
        # The guard must not break legitimate JWT auth.
        with _Server(secret_env_value=REAL_SECRET) as s:
            r = s.get(make_token(REAL_SECRET))
            assert r.status_code == 200, r.text[:200]
            assert "protected-data" in r.text

    def test_a_wrongly_signed_token_is_refused(self):
        with _Server(secret_env_value=REAL_SECRET) as s:
            assert s.get(make_token("not-the-secret")).status_code == 401

    def test_an_empty_key_forgery_is_refused_even_when_a_secret_is_set(self):
        with _Server(secret_env_value=REAL_SECRET) as s:
            assert s.get(make_token("")).status_code == 401

    def test_no_token_is_refused(self):
        with _Server(secret_env_value=REAL_SECRET) as s:
            assert s.get().status_code == 401


class TestMcpJwtEmptySecret:
    """The empty-secret guard must hold on the MCP surface too.

    The first fix refused an empty jwt-secret in AuthMiddleware, which is the
    REST path. MCPAuthHandler built jwt::algorithm::hs256{mcp_auth.jwt_secret}
    with no such check, so the identical complete-bypass remained open over
    MCP - the surface agents call.
    """

    def _server(self, secret_env_value=None):
        s = _Server(secret_env_value=secret_env_value)
        # Re-point the fixture at MCP bearer auth.
        cfg = os.path.join(s.tmp, "flapi.yaml")
        with open(cfg) as f:
            body = f.read()
        body += ("mcp:\n  enabled: true\n  auth:\n    enabled: true\n"
                 "    type: bearer\n"
                 "    jwt-secret: '{{env.FLAPI_TEST_JWT_SECRET}}'\n"
                 "    jwt-issuer: 'flapi'\n")
        with open(cfg, "w") as f:
            f.write(body)
        return s

    def test_an_empty_key_forgery_is_refused_over_mcp(self):
        with self._server(secret_env_value=None) as s:
            body = {"jsonrpc": "2.0", "id": 1, "method": "tools/list", "params": {}}
            r = requests.post(
                f"{s.base_url}/mcp/jsonrpc",
                headers={"Content-Type": "application/json",
                         "Authorization": f"Bearer {make_token('')}"},
                data=json.dumps(body), timeout=15)
            # Either an auth error or a refusal - what must NOT happen is the
            # forged token being accepted as an authenticated principal.
            assert "protected" not in r.text.lower()
            combined = r.text + open(s.log_path).read()
            assert ("jwt-secret is empty" in combined
                    or r.status_code in (401, 403)
                    or "error" in r.text.lower()), r.text[:300]
