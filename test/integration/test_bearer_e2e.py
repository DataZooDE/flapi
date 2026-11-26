"""HS256 bearer tokens, end to end, on REST and MCP alike (#281).

REST (AuthMiddleware) and MCP (MCPAuthHandler) each had their own copy of the
bearer check, and they disagreed:

- an unset `jwt-issuer` meant `iss` must be "" on REST, and "flapi" on MCP;
- REST answered 401 to a token without a `roles` claim, MCP accepted it;
- REST never filled `auth.email`;
- neither had an audience check, and a token without `exp` never expired.

Both now go through one verifier. This file starts the real binary with several
bearer endpoints and MCP bearer auth, signs tokens itself, and checks that both
surfaces give the same answer.
"""
import base64
import hashlib
import hmac
import json
import os
import signal
import subprocess
import tempfile
import time

import pytest
import requests

from otel_helpers import flapi_binary, free_port

pytestmark = pytest.mark.standalone_server

SECRET = "a-long-enough-bearer-test-secret"


def _b64(raw: bytes) -> str:
    return base64.urlsafe_b64encode(raw).rstrip(b"=").decode()


def token(secret=SECRET, header=None, drop=(), **claims) -> str:
    now = int(time.time())
    body = {"sub": "alice", "iat": now, "exp": now + 600, "roles": ["analyst"]}
    body.update(claims)
    for name in drop:
        body.pop(name, None)
    hdr = header or {"alg": "HS256", "typ": "JWT"}
    signing_input = (_b64(json.dumps(hdr, separators=(",", ":")).encode()) + "." +
                     _b64(json.dumps(body, separators=(",", ":")).encode()))
    sig = _b64(hmac.new(secret.encode(), signing_input.encode(), hashlib.sha256).digest())
    return f"{signing_input}.{sig}"


WHO_SQL = ("SELECT '{{{ auth.username }}}' AS who, '{{{ auth.roles }}}' AS roles, "
           "'{{{ auth.email }}}' AS email, '{{{ auth.type }}}' AS auth_type\n")


class BearerServer:
    def __init__(self):
        self.tmp = tempfile.mkdtemp(prefix="flapi_bearer_e2e_")
        self.port = free_port()
        self.base_url = f"http://127.0.0.1:{self.port}"
        self.log_path = os.path.join(self.tmp, "server.log")
        sqls = os.path.join(self.tmp, "sqls")
        os.makedirs(sqls)
        endpoints = {
            # No jwt-issuer: the issuer is not checked (and startup says so).
            "open-issuer": "  jwt-secret: " + SECRET + "\n",
            "strict": ("  jwt-secret: " + SECRET + "\n  jwt-issuer: flapi-tests\n"
                       "  jwt-audience: orders-api\n"),
            "audiences": "  jwt-secret: " + SECRET + "\n  jwt-audience: [a-api, b-api]\n",
            "no-exp": "  jwt-secret: " + SECRET + "\n  jwt-require-exp: false\n",
        }
        for name, auth in endpoints.items():
            with open(os.path.join(sqls, f"{name}.yaml"), "w") as f:
                f.write(f"url-path: /{name}\nmethod: GET\ntemplate-source: who.sql\n"
                        "connection: [inmem]\n"
                        "auth:\n  enabled: true\n  type: bearer\n" + auth)
        with open(os.path.join(sqls, "tool.yaml"), "w") as f:
            f.write("url-path: /tool\nmethod: GET\ntemplate-source: who.sql\n"
                    "connection: [inmem]\nauth:\n  enabled: false\n"
                    "mcp-tool:\n  name: whoami\n  description: Shows the caller.\n")
        with open(os.path.join(sqls, "who.sql"), "w") as f:
            f.write(WHO_SQL)
        with open(os.path.join(self.tmp, "flapi.yaml"), "w") as f:
            f.write("project-name: bearer-e2e\n"
                    "project-description: bearer tokens on REST and MCP\n"
                    f"http-port: {self.port}\n"
                    "template:\n  path: ./sqls\n"
                    "connections:\n  inmem:\n    properties:\n      database: ':memory:'\n"
                    "mcp:\n  enabled: true\n  auth:\n    enabled: true\n    type: bearer\n"
                    f"    jwt-secret: {SECRET}\n    jwt-audience: mcp-api\n")
        self.proc = None

    def start(self):
        env = {**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"}
        self.proc = subprocess.Popen(
            [flapi_binary(), "-c", os.path.join(self.tmp, "flapi.yaml"),
             "-p", str(self.port), "--log-level", "debug"],
            stdout=open(self.log_path, "w"), stderr=subprocess.STDOUT,
            cwd=self.tmp, env=env, preexec_fn=os.setsid)
        deadline = time.time() + 90
        while time.time() < deadline:
            if self.proc.poll() is not None:
                break
            try:
                if requests.get(f"{self.base_url}/health/live", timeout=2).status_code == 200:
                    return self
            except requests.RequestException:
                pass
            time.sleep(0.2)
        pytest.fail(f"flapi did not start:\n{self.log()[-3000:]}")

    def stop(self):
        if self.proc:
            try:
                os.killpg(os.getpgid(self.proc.pid), signal.SIGKILL)
            except ProcessLookupError:
                pass
            self.proc.wait(timeout=30)

    def log(self) -> str:
        with open(self.log_path) as f:
            return f.read()

    def get(self, path, tok=None):
        headers = {"Authorization": f"Bearer {tok}"} if tok else {}
        return requests.get(f"{self.base_url}{path}", headers=headers, timeout=15)

    def mcp(self, tok=None):
        headers = {"Content-Type": "application/json"}
        if tok:
            headers["Authorization"] = f"Bearer {tok}"
        return requests.post(f"{self.base_url}/mcp/jsonrpc", headers=headers, timeout=15,
                             json={"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                                   "params": {"name": "whoami", "arguments": {}}})


@pytest.fixture(scope="module")
def server():
    srv = BearerServer().start()
    yield srv
    srv.stop()


def _row(response) -> dict:
    body = response.json()
    rows = body["data"] if isinstance(body, dict) else body
    assert len(rows) == 1, body
    return rows[0]


def _mcp_ok(r) -> dict:
    assert r.status_code == 200, f"{r.status_code} {r.text[:300]}"
    body = r.json()
    assert "error" not in body and body["result"].get("isError") is not True, body
    return body


def _mcp_refused(r):
    assert r.status_code == 401, f"{r.status_code} {r.text[:300]}"
    assert "result" not in r.json()


class TestIssuer:
    def test_an_unset_issuer_accepts_any_iss_on_rest(self, server):
        r = server.get("/open-issuer", token(iss="https://idp.example.com"))
        assert r.status_code == 200, f"{r.text[:200]}\n{server.log()[-1500:]}"

    def test_an_unset_issuer_accepts_a_token_without_iss_on_rest(self, server):
        assert server.get("/open-issuer", token()).status_code == 200

    def test_an_unset_issuer_accepts_any_iss_on_mcp(self, server):
        # MCP used to default jwt-issuer to "flapi".
        _mcp_ok(server.mcp(token(iss="https://idp.example.com", aud="mcp-api")))

    def test_a_set_issuer_must_match(self, server):
        good = token(iss="flapi-tests", aud="orders-api")
        assert server.get("/strict", good).status_code == 200
        assert server.get("/strict", token(iss="someone-else", aud="orders-api")).status_code == 401
        assert server.get("/strict", token(aud="orders-api")).status_code == 401

    def test_startup_warns_about_the_unchecked_issuer(self, server):
        log = server.log()
        assert "AUTH_JWT_ISSUER_UNCHECKED" in log, log[-2000:]


class TestAudience:
    def test_the_configured_audience_is_accepted(self, server):
        assert server.get("/strict", token(iss="flapi-tests", aud="orders-api")).status_code == 200

    def test_an_audience_list_containing_ours_is_accepted(self, server):
        tok = token(iss="flapi-tests", aud=["other", "orders-api"])
        assert server.get("/strict", tok).status_code == 200

    def test_a_wrong_audience_is_refused(self, server):
        assert server.get("/strict", token(iss="flapi-tests", aud="other-api")).status_code == 401

    def test_a_missing_audience_is_refused(self, server):
        assert server.get("/strict", token(iss="flapi-tests")).status_code == 401

    def test_any_of_several_configured_audiences(self, server):
        assert server.get("/audiences", token(aud="b-api")).status_code == 200
        assert server.get("/audiences", token(aud="c-api")).status_code == 401

    def test_mcp_checks_its_audience(self, server):
        _mcp_ok(server.mcp(token(aud="mcp-api")))
        _mcp_refused(server.mcp(token(aud="orders-api")))
        _mcp_refused(server.mcp(token()))


class TestExpiry:
    def test_a_token_without_exp_is_refused_on_rest(self, server):
        assert server.get("/open-issuer", token(drop=("exp",))).status_code == 401

    def test_a_token_without_exp_is_refused_on_mcp(self, server):
        _mcp_refused(server.mcp(token(aud="mcp-api", drop=("exp",))))

    def test_jwt_require_exp_false_accepts_a_token_without_exp(self, server):
        assert server.get("/no-exp", token(drop=("exp",))).status_code == 200

    def test_jwt_require_exp_false_still_refuses_an_expired_token(self, server):
        assert server.get("/no-exp", token(exp=int(time.time()) - 3600)).status_code == 401

    def test_an_expired_token_is_refused(self, server):
        assert server.get("/open-issuer", token(exp=int(time.time()) - 3600)).status_code == 401
        _mcp_refused(server.mcp(token(aud="mcp-api", exp=int(time.time()) - 3600)))

    def test_nbf_in_the_future_is_refused(self, server):
        later = int(time.time()) + 3600
        assert server.get("/open-issuer", token(nbf=later)).status_code == 401
        _mcp_refused(server.mcp(token(aud="mcp-api", nbf=later)))

    def test_iat_in_the_future_is_refused(self, server):
        later = int(time.time()) + 3600
        assert server.get("/open-issuer", token(iat=later)).status_code == 401
        _mcp_refused(server.mcp(token(aud="mcp-api", iat=later)))


class TestClaims:
    def test_roles_are_optional_on_rest(self, server):
        r = server.get("/open-issuer", token(drop=("roles",)))
        assert r.status_code == 200, r.text[:200]
        assert _row(r)["roles"] == ""

    def test_roles_are_optional_on_mcp(self, server):
        _mcp_ok(server.mcp(token(aud="mcp-api", drop=("roles",))))

    def test_the_principal_reaches_the_template(self, server):
        r = server.get("/open-issuer", token(sub="bob", email="bob@example.com",
                                             roles=["analyst", "viewer"]))
        assert r.status_code == 200, r.text[:200]
        row = _row(r)
        assert row["who"] == "bob"
        assert row["roles"] == "analyst,viewer"
        assert row["email"] == "bob@example.com", row
        assert row["auth_type"] == "bearer"

    def test_the_email_reaches_the_template_on_mcp(self, server):
        body = _mcp_ok(server.mcp(token(aud="mcp-api", sub="carol", email="carol@example.com")))
        text = json.dumps(body["result"])
        assert "carol" in text and "carol@example.com" in text, body

    def test_a_token_without_sub_is_refused(self, server):
        assert server.get("/open-issuer", token(drop=("sub",))).status_code == 401
        _mcp_refused(server.mcp(token(aud="mcp-api", drop=("sub",))))


class TestSignature:
    def test_a_wrong_key_is_refused(self, server):
        assert server.get("/open-issuer", token(secret="not-the-secret")).status_code == 401
        _mcp_refused(server.mcp(token(secret="not-the-secret", aud="mcp-api")))

    def test_alg_none_is_refused(self, server):
        head, payload, _ = token(header={"alg": "none", "typ": "JWT"}).split(".")
        assert server.get("/open-issuer", f"{head}.{payload}.").status_code == 401
        _mcp_refused(server.mcp(f"{head}.{payload}."))

    def test_no_token_is_refused(self, server):
        assert server.get("/open-issuer").status_code == 401
        _mcp_refused(server.mcp())
