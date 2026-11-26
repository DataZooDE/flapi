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
from cryptography.hazmat.primitives.asymmetric import ec, padding, rsa
from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature

from otel_helpers import flapi_binary, free_port

pytestmark = pytest.mark.standalone_server

CLIENT_ID = "flapi-e2e"
NOAUD_CLIENT = "flapi-noaud"
AUDIENCE = "flapi-api"


def _b64url(raw: bytes) -> str:
    return base64.urlsafe_b64encode(raw).rstrip(b"=").decode()


def _uint_b64url(value: int) -> str:
    return _b64url(value.to_bytes((value.bit_length() + 7) // 8, "big"))


_HASHES = {"256": hashes.SHA256, "384": hashes.SHA384, "512": hashes.SHA512}


class _Key:
    """An RSA key. `alg` is what its JWK names (None: the JWK names none)."""

    def __init__(self, kid: str, alg="RS256"):
        self.kid = kid
        self.alg = alg
        self.default_alg = alg or "RS256"
        self.private = rsa.generate_private_key(public_exponent=65537, key_size=2048)

    def jwk(self) -> dict:
        numbers = self.private.public_key().public_numbers()
        jwk = {"kty": "RSA", "use": "sig", "kid": self.kid,
               "n": _uint_b64url(numbers.n), "e": _uint_b64url(numbers.e)}
        if self.alg:
            jwk["alg"] = self.alg
        return jwk

    def sign(self, signing_input: bytes, alg=None) -> bytes:
        alg = alg or self.default_alg
        digest = _HASHES[alg[2:]]()
        if alg.startswith("PS"):
            pad = padding.PSS(mgf=padding.MGF1(digest), salt_length=digest.digest_size)
            return self.private.sign(signing_input, pad, digest)
        return self.private.sign(signing_input, padding.PKCS1v15(), digest)


class _EcKey:
    """An EC key: P-256 (ES256), P-384 (ES384) or P-521 (ES512)."""

    CURVES = {"P-256": (ec.SECP256R1, "ES256", 32), "P-384": (ec.SECP384R1, "ES384", 48),
              "P-521": (ec.SECP521R1, "ES512", 66)}

    def __init__(self, kid: str, crv="P-256", with_alg=True):
        self.kid = kid
        self.crv = crv
        curve, self.default_alg, self.size = self.CURVES[crv]
        self.with_alg = with_alg
        self.private = ec.generate_private_key(curve())

    def jwk(self) -> dict:
        numbers = self.private.public_key().public_numbers()
        jwk = {"kty": "EC", "use": "sig", "kid": self.kid, "crv": self.crv,
               "x": _b64url(numbers.x.to_bytes(self.size, "big")),
               "y": _b64url(numbers.y.to_bytes(self.size, "big"))}
        if self.with_alg:
            jwk["alg"] = self.default_alg
        return jwk

    def sign(self, signing_input: bytes, alg=None) -> bytes:
        alg = alg or self.default_alg
        der = self.private.sign(signing_input, ec.ECDSA(_HASHES[alg[2:]]()))
        r, s_ = decode_dss_signature(der)
        # JOSE carries r || s, each the curve size, not DER.
        return r.to_bytes(self.size, "big") + s_.to_bytes(self.size, "big")


class MockIssuer:
    """An OIDC provider reduced to what a resource server reads from it."""

    def __init__(self):
        self.port = free_port()
        self.url = f"http://127.0.0.1:{self.port}"
        self.keys = [_Key("k1"), _EcKey("ec256"), _EcKey("ec384", "P-384"),
                     _EcKey("ec521", "P-521", with_alg=False), _Key("ps1", alg="PS256"),
                     _Key("rsa-noalg", alg=None)]
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

    def key(self, kid):
        return next(k for k in self.keys if k.kid == kid)

    def token(self, key=None, header=None, drop=(), alg=None, **overrides) -> str:
        """A token signed with `key` (default k1, RS256) that is valid unless the caller says otherwise."""
        key = key or self.keys[0]
        now = int(time.time())
        claims = {"iss": self.url, "aud": AUDIENCE, "sub": "user-42",
                  "iat": now, "exp": now + 600, "roles": ["analyst", "viewer"]}
        claims.update(overrides)
        for name in drop:
            claims.pop(name, None)
        sign_alg = alg or key.default_alg
        hdr = header or {"alg": sign_alg, "typ": "JWT", "kid": key.kid}
        signing_input = (_b64url(json.dumps(hdr, separators=(",", ":")).encode()) + "." +
                         _b64url(json.dumps(claims, separators=(",", ":")).encode()))
        return signing_input + "." + _b64url(key.sign(signing_input.encode(), sign_alg))


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
                    "'{{{ auth.groups }}}' AS groups, "
                    "'{{{ auth.type }}}' AS auth_type\n")
        # Endpoints with an OIDC block of their own (#281), each with its own
        # client id so that no two share an issuer and client.
        extra = {
            # No allowed-audiences: `aud` must then contain the client id.
            "noaud": (f"issuer-url: {issuer.url}\nclient-id: {NOAUD_CLIENT}\n"
                      "scopes: [openid, profile]\n"),
            # An explicit username-claim beats the keycloak preset's preferred_username.
            "kc": (f"provider-type: keycloak\nissuer-url: {issuer.url}\nclient-id: flapi-kc\n"
                   f"allowed-audiences: [{AUDIENCE}]\nusername-claim: sub\n"),
            # A claim NAME with dots in it (Auth0-style namespaced claim).
            "ns": (f"issuer-url: {issuer.url}\nclient-id: flapi-ns\n"
                   f"allowed-audiences: [{AUDIENCE}]\nroles-claim: https://flapi.example/roles\n"),
            # A real nested path still works.
            "nested": (f"issuer-url: {issuer.url}\nclient-id: flapi-nested\n"
                       f"allowed-audiences: [{AUDIENCE}]\nrole-claim-path: realm_access.roles\n"),
        }
        for name, block in extra.items():
            with open(os.path.join(sqls, f"{name}.yaml"), "w") as f:
                f.write(f"url-path: /{name}\nmethod: GET\ntemplate-source: whoami.sql\n"
                        "connection: [inmem]\nauth:\n  enabled: true\n  type: oidc\n  oidc:\n" +
                        "".join(f"    {line}\n" for line in block.splitlines()))
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

    def get(self, token=None, path="/whoami"):
        headers = {"Authorization": f"Bearer {token}"} if token else {}
        return requests.get(f"{self.base_url}{path}", headers=headers, timeout=15)

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


class TestOidcClaims:
    """Claim mapping and audience defaults (#281)."""

    def test_without_allowed_audiences_the_client_id_is_the_audience(self, server, issuer):
        assert server.get(issuer.token(aud=NOAUD_CLIENT), path="/noaud").status_code == 200
        assert server.get(issuer.token(aud=["x", NOAUD_CLIENT]), path="/noaud").status_code == 200

    def test_without_allowed_audiences_another_audience_is_refused(self, server, issuer):
        r = server.get(issuer.token(aud="another-app"), path="/noaud")
        assert r.status_code == 401, r.text[:200]
        assert server.get(issuer.token(drop=("aud",)), path="/noaud").status_code == 401

    def test_an_explicit_username_claim_beats_the_preset(self, server, issuer):
        r = server.get(issuer.token(sub="the-sub", preferred_username="the-preferred"), path="/kc")
        assert r.status_code == 200, f"{r.text[:200]}\n{server.log()[-1500:]}"
        assert _row(r)["who"] == "the-sub"

    def test_a_claim_name_with_dots_is_read_literally(self, server, issuer):
        tok = issuer.token(drop=("roles",), **{"https://flapi.example/roles": ["ns-admin", "ns-read"]})
        r = server.get(tok, path="/ns")
        assert r.status_code == 200, r.text[:200]
        assert _row(r)["roles"] == "ns-admin,ns-read"

    def test_a_nested_role_path_still_works(self, server, issuer):
        r = server.get(issuer.token(realm_access={"roles": ["kc-admin"]}), path="/nested")
        assert r.status_code == 200, r.text[:200]
        assert _row(r)["roles"] == "kc-admin"

    def test_groups_reach_the_template(self, server, issuer):
        r = server.get(issuer.token(groups=["team-a", "team-b"]))
        assert r.status_code == 200, r.text[:200]
        assert _row(r)["groups"] == "team-a,team-b"

    def test_groups_reach_the_template_over_mcp(self, server, issuer):
        r = server.mcp("tools/call", {"name": "whoami", "arguments": {}},
                       token=issuer.token(sub="mcp-groups", groups=["team-mcp"]))
        assert r.status_code == 200, f"{r.status_code} {r.text[:300]}"
        assert "team-mcp" in json.dumps(r.json()["result"]), r.text[:300]

    def test_a_group_with_a_comma_is_refused(self, server, issuer):
        tok = issuer.token(groups=["team-a,admin"])
        assert server.get(tok).status_code == 401
        r = server.mcp("tools/call", {"name": "whoami", "arguments": {}}, token=tok)
        assert r.status_code == 401, f"{r.status_code} {r.text[:300]}"

    def test_a_group_with_a_control_character_is_refused(self, server, issuer):
        tok = issuer.token(groups=["team-a\nOR 1=1"])
        assert server.get(tok).status_code == 401
        r = server.mcp("tools/call", {"name": "whoami", "arguments": {}}, token=tok)
        assert r.status_code == 401, f"{r.status_code} {r.text[:300]}"

    def test_nbf_in_the_future_is_refused(self, server, issuer):
        later = int(time.time()) + 3600
        assert server.get(issuer.token(nbf=later)).status_code == 401

    def test_iat_in_the_future_is_refused(self, server, issuer):
        later = int(time.time()) + 3600
        assert server.get(issuer.token(iat=later, exp=later + 600)).status_code == 401

    def test_unused_keys_are_reported_at_startup(self, server):
        assert "`scopes` is parsed but has no effect" in server.log()


def _tamper(token: str) -> str:
    """The same signature over a payload that now claims `admin`."""
    head, payload, sig = token.split(".")
    claims = json.loads(base64.urlsafe_b64decode(payload + "=" * (-len(payload) % 4)))
    claims["roles"] = ["admin"]
    return f"{head}.{_b64url(json.dumps(claims).encode())}.{sig}"


class TestOidcAlgorithms:
    """ES256/384/512 and PS256/384/512 next to RS*, and alg/key agreement."""

    @pytest.mark.parametrize("kid,alg", [
        ("ec256", "ES256"), ("ec384", "ES384"), ("ec521", "ES512"),
        ("ps1", "PS256"), ("rsa-noalg", "PS384"), ("rsa-noalg", "PS512"),
        ("rsa-noalg", "RS384"), ("k1", "RS256"),
    ])
    def test_a_valid_token_is_accepted(self, server, issuer, kid, alg):
        r = server.get(issuer.token(key=issuer.key(kid), alg=alg, sub=f"{kid}-{alg}"))
        assert r.status_code == 200, f"{kid}/{alg}: {r.status_code} {r.text[:200]}\n{server.log()[-1500:]}"
        assert _row(r)["who"] == f"{kid}-{alg}"

    def test_es256_over_mcp(self, server, issuer):
        r = server.mcp("tools/call", {"name": "whoami", "arguments": {}},
                       token=issuer.token(key=issuer.key("ec256"), sub="ec-mcp"))
        assert r.status_code == 200, f"{r.status_code} {r.text[:300]}"
        assert "ec-mcp" in json.dumps(r.json()["result"])

    @pytest.mark.parametrize("kid,alg", [("ec256", "ES256"), ("ec384", "ES384"), ("ps1", "PS256")])
    def test_a_tampered_token_is_refused(self, server, issuer, kid, alg):
        tok = _tamper(issuer.token(key=issuer.key(kid), alg=alg))
        assert server.get(tok).status_code == 401

    @pytest.mark.parametrize("kid,alg,why", [
        ("k1", "PS256", "the JWK says RS256"),
        ("ps1", "RS256", "the JWK says PS256"),
        ("ec256", "ES384", "the JWK says ES256"),
        ("ec521", "ES256", "a P-521 key cannot verify ES256"),
        ("ec256", "RS256", "an EC key cannot verify RS256"),
        ("rsa-noalg", "ES256", "an RSA key cannot verify ES256"),
    ])
    def test_an_alg_the_key_does_not_allow_is_refused(self, server, issuer, kid, alg, why):
        key = issuer.key(kid)
        if alg[:2] in (("ES",) if isinstance(key, _EcKey) else ("RS", "PS")):
            # A genuine signature with `alg`: only the key's own rules refuse it.
            tok = issuer.token(key=key, alg=alg)
        else:
            # The key cannot produce `alg` at all: the header claims it anyway.
            tok = issuer.token(key=key, header={"alg": alg, "typ": "JWT", "kid": kid})
        assert server.get(tok).status_code == 401, why

    def test_hs256_with_the_public_key_as_secret_is_refused(self, server, issuer):
        import hashlib
        import hmac
        key = issuer.key("rsa-noalg")
        secret = json.dumps(key.jwk()).encode()
        head = _b64url(json.dumps({"alg": "HS256", "typ": "JWT", "kid": key.kid}).encode())
        now = int(time.time())
        body = _b64url(json.dumps({"iss": issuer.url, "aud": AUDIENCE, "sub": "x",
                                   "exp": now + 600}).encode())
        sig = _b64url(hmac.new(secret, f"{head}.{body}".encode(), hashlib.sha256).digest())
        assert server.get(f"{head}.{body}.{sig}").status_code == 401
