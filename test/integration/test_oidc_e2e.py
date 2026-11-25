"""OIDC end to end: a real flapi binary against a mock identity provider.

Issue #273. With `auth.type: oidc`, every token was refused, the correct ones
included. `OIDCAuthHandler::decodeJWT` parsed the header and payload with
`crow::json::load` and returned *copies* of the parsed values. A copied crow
`rvalue` does not own the buffer it points into, and the originals freed that
buffer when the function returned. The later `header.has("alg")` read freed
memory, and the server logged

    OIDC error: JWT header missing 'alg' field

for a token whose header was {"alg":"RS256","typ":"JWT","kid":"k1"}.

The suite had no end-to-end OIDC success case, which is how this shipped:
every existing OIDC test checked a refusal, and a handler that refuses
everything passes all of them. This file runs a mock issuer on 127.0.0.1
(discovery document + JWKS with an RSA key), starts the real binary with
`auth: {type: oidc}` for REST and `mcp.auth: {type: oidc}` for MCP, and signs
RS256 tokens itself. The accepting cases are the point; the refusals make
sure the fix did not get there by accepting everything.
"""
import base64
import http.server
import json
import os
import signal
import subprocess
import tempfile
import threading
import time

import pytest
import requests
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import padding, rsa

from otel_helpers import flapi_binary, free_port

pytestmark = pytest.mark.standalone_server

CLIENT_ID = "flapi-e2e"
AUDIENCE = "flapi-api"


def _b64url(raw: bytes) -> str:
    return base64.urlsafe_b64encode(raw).rstrip(b"=").decode()


def _uint_b64url(value: int) -> str:
    return _b64url(value.to_bytes((value.bit_length() + 7) // 8, "big"))


class _Key:
    def __init__(self, kid: str):
        self.kid = kid
        self.private = rsa.generate_private_key(public_exponent=65537, key_size=2048)

    def jwk(self) -> dict:
        numbers = self.private.public_key().public_numbers()
        return {"kty": "RSA", "use": "sig", "alg": "RS256", "kid": self.kid,
                "n": _uint_b64url(numbers.n), "e": _uint_b64url(numbers.e)}

    def sign(self, signing_input: bytes) -> bytes:
        return self.private.sign(signing_input, padding.PKCS1v15(), hashes.SHA256())


class MockIssuer:
    """An OIDC provider reduced to what a resource server reads from it."""

    def __init__(self):
        self.port = free_port()
        self.url = f"http://127.0.0.1:{self.port}"
        self.keys = [_Key("k1")]
        self.jwks_fetches = 0
        issuer = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):  # noqa: N802 (http.server API)
                if self.path == "/.well-known/openid-configuration":
                    body = {
                        "issuer": issuer.url,
                        "jwks_uri": f"{issuer.url}/jwks",
                        "authorization_endpoint": f"{issuer.url}/authorize",
                        "token_endpoint": f"{issuer.url}/token",
                        "response_types_supported": ["code"],
                        "subject_types_supported": ["public"],
                        "id_token_signing_alg_values_supported": ["RS256"],
                    }
                elif self.path == "/jwks":
                    issuer.jwks_fetches += 1
                    body = {"keys": [k.jwk() for k in issuer.keys]}
                else:
                    self.send_error(404)
                    return
                raw = json.dumps(body).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(raw)))
                self.end_headers()
                self.wfile.write(raw)

            def log_message(self, *args):
                pass

        self.httpd = http.server.ThreadingHTTPServer(("127.0.0.1", self.port), Handler)
        self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)

    def start(self):
        self.thread.start()
        return self

    def stop(self):
        self.httpd.shutdown()
        self.httpd.server_close()

    def token(self, key=None, header=None, drop=(), **overrides) -> str:
        """An RS256 token that is valid unless the caller says otherwise."""
        key = key or self.keys[0]
        now = int(time.time())
        claims = {"iss": self.url, "aud": AUDIENCE, "sub": "user-42",
                  "iat": now, "exp": now + 600, "roles": ["analyst", "viewer"]}
        claims.update(overrides)
        for name in drop:
            claims.pop(name, None)
        hdr = header or {"alg": "RS256", "typ": "JWT", "kid": key.kid}
        signing_input = (_b64url(json.dumps(hdr, separators=(",", ":")).encode()) + "." +
                         _b64url(json.dumps(claims, separators=(",", ":")).encode()))
        return signing_input + "." + _b64url(key.sign(signing_input.encode()))


class FlapiServer:
    def __init__(self, issuer: MockIssuer):
        self.tmp = tempfile.mkdtemp(prefix="flapi_oidc_e2e_")
        self.port = free_port()
        self.base_url = f"http://127.0.0.1:{self.port}"
        self.log_path = os.path.join(self.tmp, "server.log")
        oidc = (f"    issuer-url: {issuer.url}\n"
                f"    client-id: {CLIENT_ID}\n"
                f"    allowed-audiences: [{AUDIENCE}]\n")
        sqls = os.path.join(self.tmp, "sqls")
        os.makedirs(sqls)
        with open(os.path.join(sqls, "whoami.yaml"), "w") as f:
            f.write("url-path: /whoami\nmethod: GET\n"
                    "template-source: whoami.sql\nconnection: [inmem]\n"
                    "mcp-tool:\n  name: whoami\n  description: Shows the caller.\n"
                    "  allowed-roles: [analyst]\n")
        with open(os.path.join(sqls, "whoami.sql"), "w") as f:
            f.write("SELECT '{{{ auth.username }}}' AS who, "
                    "'{{{ auth.roles }}}' AS roles, "
                    "'{{{ auth.type }}}' AS auth_type\n")
        with open(os.path.join(self.tmp, "flapi.yaml"), "w") as f:
            f.write("project-name: oidc-e2e\n"
                    "project-description: OIDC against a mock issuer\n"
                    f"http-port: {self.port}\n"
                    "template:\n  path: ./sqls\n"
                    "connections:\n  inmem:\n    properties:\n      database: ':memory:'\n"
                    "auth:\n  enabled: true\n  type: oidc\n  oidc:\n" + oidc +
                    "mcp:\n  enabled: true\n  auth:\n    enabled: true\n    type: oidc\n"
                    "    oidc:\n" + "\n".join("  " + line for line in oidc.splitlines()) + "\n")
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

    def get(self, token=None):
        headers = {"Authorization": f"Bearer {token}"} if token else {}
        return requests.get(f"{self.base_url}/whoami", headers=headers, timeout=15)

    def mcp(self, method, params, token=None):
        headers = {"Content-Type": "application/json"}
        if token:
            headers["Authorization"] = f"Bearer {token}"
        return requests.post(f"{self.base_url}/mcp/jsonrpc", headers=headers, timeout=15,
                             json={"jsonrpc": "2.0", "id": 1, "method": method, "params": params})


@pytest.fixture(scope="module")
def issuer():
    idp = MockIssuer().start()
    yield idp
    idp.stop()


@pytest.fixture(scope="module")
def server(issuer):
    srv = FlapiServer(issuer).start()
    yield srv
    srv.stop()


def _row(response) -> dict:
    body = response.json()
    rows = body["data"] if isinstance(body, dict) else body
    assert len(rows) == 1, body
    return rows[0]


class TestOidcRestAccepts:
    def test_a_valid_token_is_accepted(self, server, issuer):
        r = server.get(issuer.token())
        assert r.status_code == 200, f"{r.status_code} {r.text[:200]}\n{server.log()[-2000:]}"

    def test_the_principal_reaches_the_template(self, server, issuer):
        r = server.get(issuer.token(sub="alice"))
        assert r.status_code == 200, r.text[:200]
        row = _row(r)
        assert row["who"] == "alice"
        assert "analyst" in row["roles"] and "viewer" in row["roles"], row
        assert row["auth_type"] == "oidc", row

    def test_an_audience_list_containing_ours_is_accepted(self, server, issuer):
        r = server.get(issuer.token(aud=["someone-else", AUDIENCE]))
        assert r.status_code == 200, r.text[:200]

    def test_a_rotated_in_key_is_picked_up_by_refetching_the_jwks(self, server, issuer):
        rotated = _Key("k2")
        issuer.keys.append(rotated)
        try:
            r = server.get(issuer.token(key=rotated, sub="rotated"))
            assert r.status_code == 200, r.text[:200]
            assert _row(r)["who"] == "rotated"
        finally:
            issuer.keys.remove(rotated)


class TestOidcRestRefuses:
    def test_no_token(self, server):
        assert server.get().status_code == 401

    def test_a_bad_signature(self, server, issuer):
        token = issuer.token()
        head, payload, sig = token.split(".")
        forged_payload = _b64url(json.dumps(
            {"iss": issuer.url, "aud": AUDIENCE, "sub": "admin",
             "exp": int(time.time()) + 600, "roles": ["admin"]}).encode())
        assert server.get(f"{head}.{forged_payload}.{sig}").status_code == 401

    def test_a_token_signed_by_a_key_the_issuer_does_not_publish(self, server, issuer):
        stranger = _Key("k1")  # same kid, different key
        assert server.get(issuer.token(key=stranger)).status_code == 401

    def test_the_wrong_issuer(self, server, issuer):
        assert server.get(issuer.token(iss="http://127.0.0.1:1/evil")).status_code == 401

    def test_the_wrong_audience(self, server, issuer):
        assert server.get(issuer.token(aud="another-api")).status_code == 401

    def test_an_expired_token(self, server, issuer):
        # Well past the default 300 s clock skew.
        long_ago = int(time.time()) - 3600
        r = server.get(issuer.token(iat=long_ago - 600, exp=long_ago))
        assert r.status_code == 401

    def test_a_token_without_exp(self, server, issuer):
        assert server.get(issuer.token(drop=("exp",))).status_code == 401

    def test_a_token_without_sub(self, server, issuer):
        assert server.get(issuer.token(drop=("sub",))).status_code == 401

    def test_an_unknown_kid_is_refused_after_a_jwks_refetch(self, server, issuer):
        ghost = _Key("ghost")
        before = issuer.jwks_fetches
        assert server.get(issuer.token(key=ghost)).status_code == 401
        assert issuer.jwks_fetches > before, "an unknown kid must trigger a JWKS refetch"

    def test_alg_none(self, server, issuer):
        token = issuer.token(header={"alg": "none", "typ": "JWT", "kid": "k1"})
        head, payload, _ = token.split(".")
        assert server.get(f"{head}.{payload}.").status_code == 401

    def test_garbage(self, server):
        assert server.get("not.a.jwt").status_code == 401


class TestOidcMcp:
    def test_tools_call_with_a_valid_token(self, server, issuer):
        r = server.mcp("tools/call", {"name": "whoami", "arguments": {}},
                       token=issuer.token(sub="mcp-user"))
        assert r.status_code == 200, f"{r.status_code} {r.text[:300]}"
        body = r.json()
        assert "error" not in body, body
        assert body["result"].get("isError") is not True, body
        assert "mcp-user" in json.dumps(body["result"]), body

    def test_the_oidc_roles_gate_the_tool(self, server, issuer):
        # Authenticated, but without the role the tool's allowed-roles names.
        r = server.mcp("tools/call", {"name": "whoami", "arguments": {}},
                       token=issuer.token(sub="no-role", roles=["viewer"]))
        assert r.status_code == 403, f"{r.status_code} {r.text[:300]}"
        assert "no-role" not in r.text

    def test_tools_call_with_a_wrong_audience_is_refused(self, server, issuer):
        r = server.mcp("tools/call", {"name": "whoami", "arguments": {}},
                       token=issuer.token(aud="another-api"))
        assert r.status_code == 401, f"{r.status_code} {r.text[:300]}"
        assert "error" in r.json() and "result" not in r.json()

    def test_tools_call_with_an_expired_token_is_refused(self, server, issuer):
        long_ago = int(time.time()) - 3600
        r = server.mcp("tools/call", {"name": "whoami", "arguments": {}},
                       token=issuer.token(iat=long_ago - 600, exp=long_ago))
        assert r.status_code == 401, f"{r.status_code} {r.text[:300]}"

    def test_tools_call_without_a_token_is_refused(self, server):
        r = server.mcp("tools/call", {"name": "whoami", "arguments": {}})
        assert r.status_code == 401, f"{r.status_code} {r.text[:300]}"
        assert "error" in r.json() and "result" not in r.json()
