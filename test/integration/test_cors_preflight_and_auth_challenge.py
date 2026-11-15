"""CORS preflights and 401 challenges, against a real server (#211, cluster E3).

Measured before the fix, with `cors.allow-origins: [https://app.example.com]`:

    OPTIONS /ping            Origin: https://app.example.com
                             Access-Control-Request-Method: GET
                             Access-Control-Request-Headers: authorization
    -> 204, and NO Access-Control-Allow-* header at all

so a browser blocked every authenticated or JSON cross-origin call (REST and
/mcp/jsonrpc alike). Also measured:

  * a bearer endpoint answered a request without credentials with
    `WWW-Authenticate: Basic realm="flAPI"` - the wrong scheme - and a bad
    token with no challenge at all; the 401 body was plain text;
  * an endpoint `auth:` block with only `roles:` under a global auth block had
    type "" and refused every request with 401, without a word at startup.
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

ALLOWED = "https://app.example.com"
SECRET = "cors-preflight-test-secret"


def make_token(roles=("reader",), key=SECRET, issuer="flapi") -> str:
    def b64(raw: bytes) -> str:
        return base64.urlsafe_b64encode(raw).rstrip(b"=").decode()

    header = b64(json.dumps({"alg": "HS256", "typ": "JWT"}, separators=(",", ":")).encode())
    payload = b64(json.dumps(
        {"sub": "alice", "iss": issuer, "roles": list(roles),
         "exp": int(time.time()) + 3600}, separators=(",", ":")).encode())
    signing_input = f"{header}.{payload}".encode()
    sig = b64(hmac.new(key.encode(), signing_input, hashlib.sha256).digest())
    return f"{header}.{payload}.{sig}"


def _write(path: str, text: str) -> None:
    with open(path, "w") as f:
        f.write(text)


@pytest.fixture(scope="module")
def server():
    tmp = tempfile.mkdtemp(prefix="flapi_cors_preflight_")
    port = free_port()
    sqls = os.path.join(tmp, "sqls")
    os.makedirs(sqls)
    _write(os.path.join(tmp, "flapi.yaml"),
           "project-name: cors-preflight-test\n"
           "project-description: CORS preflight and 401 challenges\n"
           f"http-port: {port}\n"
           "template:\n  path: ./sqls\n"
           "connections:\n  inmem:\n    properties:\n      database: ':memory:'\n"
           "cors:\n"
           f"  allow-origins: ['{ALLOWED}']\n"
           "auth:\n"
           "  enabled: true\n"
           "  type: bearer\n"
           f"  jwt-secret: '{SECRET}'\n"
           "  jwt-issuer: flapi\n"
           "mcp:\n  enabled: true\n"
           "  auth:\n    enabled: true\n    type: bearer\n"
           f"    jwt-secret: '{SECRET}'\n"
           # Ignored under bearer: must be named in a load warning.
           "    users:\n      - username: ignored\n        password: ignored-pw\n")
    _write(os.path.join(sqls, "ping.sql"), "SELECT 1 AS ok\n")
    # Inherits the global bearer block.
    _write(os.path.join(sqls, "ping.yaml"),
           "url-path: /ping\nmethod: GET\ntemplate-source: ping.sql\nconnection: [inmem]\n")
    # Roles only: must inherit the global credentials and apply its own roles.
    _write(os.path.join(sqls, "admin.yaml"),
           "url-path: /admin-only\nmethod: GET\ntemplate-source: ping.sql\n"
           "connection: [inmem]\nauth:\n  roles: [admin]\n")
    # Its own basic block: the challenge must say Basic.
    _write(os.path.join(sqls, "basic.yaml"),
           "url-path: /basic\nmethod: GET\ntemplate-source: ping.sql\n"
           "connection: [inmem]\nauth:\n  type: basic\n"
           "  users:\n    - username: bob\n"
           "      password: bob-password\n"
           # Ignored under basic: must be named in a load warning.
           "  jwt-secret: not-read-by-basic\n")

    log_path = os.path.join(tmp, "server.log")
    env = {**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"}
    proc = subprocess.Popen(
        [flapi_binary(), "-c", os.path.join(tmp, "flapi.yaml"), "-p", str(port),
         "--log-level", "info"],
        stdout=open(log_path, "w"), stderr=subprocess.STDOUT,
        cwd=tmp, env=env, preexec_fn=os.setsid)
    base_url = f"http://127.0.0.1:{port}"
    deadline = time.time() + 90
    try:
        while True:
            if proc.poll() is not None or time.time() > deadline:
                pytest.fail(f"server did not start:\n{open(log_path).read()[-3000:]}")
            try:
                if requests.get(f"{base_url}/health/live", timeout=2).status_code == 200:
                    break
            except requests.RequestException:
                pass
            time.sleep(0.2)
        yield {"base_url": base_url, "log_path": log_path}
    finally:
        try:
            os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
        except ProcessLookupError:
            pass
        proc.wait(timeout=30)


def preflight(base_url, path, method, headers, origin=ALLOWED):
    return requests.options(
        f"{base_url}{path}",
        headers={"Origin": origin,
                 "Access-Control-Request-Method": method,
                 "Access-Control-Request-Headers": headers},
        timeout=10)


def header_list(response, name):
    return [h.strip().lower() for h in response.headers.get(name, "").split(",") if h.strip()]


class TestPreflight:
    def test_rest_preflight_grants_the_allowed_origin(self, server):
        r = preflight(server["base_url"], "/ping", "GET", "authorization, content-type")
        assert r.status_code in (200, 204), r.text
        assert r.headers.get("Access-Control-Allow-Origin") == ALLOWED, dict(r.headers)
        assert "get" in header_list(r, "Access-Control-Allow-Methods"), dict(r.headers)
        allowed_headers = header_list(r, "Access-Control-Allow-Headers")
        assert "authorization" in allowed_headers, dict(r.headers)
        assert "content-type" in allowed_headers, dict(r.headers)
        assert int(r.headers.get("Access-Control-Max-Age", "0")) > 0, dict(r.headers)
        assert "origin" in header_list(r, "Vary"), dict(r.headers)

    def test_mcp_preflight_grants_the_allowed_origin(self, server):
        r = preflight(server["base_url"], "/mcp/jsonrpc", "POST",
                      "authorization, content-type, mcp-session-id")
        assert r.status_code in (200, 204), r.text
        assert r.headers.get("Access-Control-Allow-Origin") == ALLOWED, dict(r.headers)
        assert "post" in header_list(r, "Access-Control-Allow-Methods"), dict(r.headers)
        allowed_headers = header_list(r, "Access-Control-Allow-Headers")
        for h in ("authorization", "content-type", "mcp-session-id"):
            assert h in allowed_headers, dict(r.headers)

    def test_preflight_without_requested_headers_still_lists_authorization(self, server):
        # `*` does not cover Authorization (Fetch spec); with auth on, it is
        # listed by name.
        r = requests.options(f"{server['base_url']}/ping",
                             headers={"Origin": ALLOWED,
                                      "Access-Control-Request-Method": "GET"},
                             timeout=10)
        assert r.headers.get("Access-Control-Allow-Origin") == ALLOWED, dict(r.headers)
        allowed_headers = header_list(r, "Access-Control-Allow-Headers")
        for h in ("authorization", "content-type", "x-config-token", "mcp-session-id"):
            assert h in allowed_headers, dict(r.headers)

    def test_preflight_from_a_disallowed_origin_is_not_granted(self, server):
        r = preflight(server["base_url"], "/ping", "GET", "authorization",
                      origin="https://evil.example.com")
        assert "Access-Control-Allow-Origin" not in r.headers, dict(r.headers)
        assert "Access-Control-Allow-Headers" not in r.headers, dict(r.headers)

    def test_actual_request_carries_the_origin_and_vary(self, server):
        r = requests.get(f"{server['base_url']}/ping",
                         headers={"Origin": ALLOWED,
                                  "Authorization": f"Bearer {make_token()}"},
                         timeout=10)
        assert r.status_code == 200, r.text
        assert r.headers.get("Access-Control-Allow-Origin") == ALLOWED
        assert "origin" in header_list(r, "Vary"), dict(r.headers)


class TestUnauthorizedChallenge:
    def test_bearer_endpoint_without_credentials_challenges_bearer(self, server):
        r = requests.get(f"{server['base_url']}/ping", timeout=10)
        assert r.status_code == 401
        challenge = r.headers.get("WWW-Authenticate", "")
        assert challenge.startswith("Bearer"), challenge
        assert "invalid_token" not in challenge, challenge
        body = r.json()
        assert body["error"] == "Unauthorized" and body["message"], body

    def test_bad_bearer_token_gets_an_invalid_token_challenge(self, server):
        r = requests.get(f"{server['base_url']}/ping",
                         headers={"Authorization": f"Bearer {make_token(key='wrong')}"},
                         timeout=10)
        assert r.status_code == 401
        challenge = r.headers.get("WWW-Authenticate", "")
        assert challenge.startswith("Bearer") and 'error="invalid_token"' in challenge, challenge
        assert r.json()["error"] == "Unauthorized"

    def test_basic_endpoint_challenges_basic(self, server):
        r = requests.get(f"{server['base_url']}/basic", timeout=10)
        assert r.status_code == 401
        assert r.headers.get("WWW-Authenticate", "").startswith("Basic"), dict(r.headers)
        r = requests.get(f"{server['base_url']}/basic", auth=("bob", "wrong"), timeout=10)
        assert r.status_code == 401
        assert r.headers.get("WWW-Authenticate", "").startswith("Basic"), dict(r.headers)
        assert r.json()["error"] == "Unauthorized"


class TestRolesOnlyBlockInheritsCredentials:
    def test_a_token_with_the_role_is_admitted(self, server):
        r = requests.get(f"{server['base_url']}/admin-only",
                         headers={"Authorization": f"Bearer {make_token(roles=['admin'])}"},
                         timeout=10)
        assert r.status_code == 200, r.text

    def test_a_token_without_the_role_is_forbidden(self, server):
        r = requests.get(f"{server['base_url']}/admin-only",
                         headers={"Authorization": f"Bearer {make_token(roles=['reader'])}"},
                         timeout=10)
        assert r.status_code == 403, r.text

    def test_no_token_is_challenged_with_the_inherited_scheme(self, server):
        r = requests.get(f"{server['base_url']}/admin-only", timeout=10)
        assert r.status_code == 401
        assert r.headers.get("WWW-Authenticate", "").startswith("Bearer"), dict(r.headers)


class TestLoadWarnings:
    def test_keys_the_scheme_ignores_are_named(self, server):
        log = open(server["log_path"]).read()
        assert "Endpoint GET /basic: auth type 'basic' ignores jwt-secret" in log, log[-3000:]
        assert "mcp.auth: auth type 'bearer' ignores users" in log, log[-3000:]
        assert "One authentication scheme applies per block" in log

    def test_roles_only_block_logs_its_inheritance(self, server):
        log = open(server["log_path"]).read()
        assert "Endpoint GET /admin-only: `auth:` names no type or credentials" in log, log[-3000:]
