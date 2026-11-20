"""Routing semantics that authorization depends on (#211, exit-gate cluster H).

The exit gate found protected data served without credentials, each time
because two endpoints could answer one request and the open one won:

* `method: get` beside a protected `GET` on the same route loaded silently
  (the duplicate check compared methods case-sensitively, routing did not);
* overlapping routes (`/customers/7` vs `/customers/:id`, `/a/:x/b` vs
  `/:z/:x/b`) were resolved first-match in directory order, so file creation
  order - and a reload, which moved the file to the end - decided;
* `url-path: /customers/{cid}` passed validation and then hung every request.

Every bypass is reproduced here against the real binary, and every protected
URL must answer 401 without credentials. Each server runs over its own
temporary project; tmpfs lists files in creation order, so the order the files
are written in is the order the old loader saw them.
"""

import base64
import hashlib
import json
import os
import subprocess
import tempfile
import time

import pytest
import requests

from otel_helpers import flapi_binary, free_port

TOKEN = "precedence-token-1234"
H = {"Authorization": f"Bearer {TOKEN}", "Content-Type": "application/json"}

BASE_CONFIG = (
    "project-name: p\nproject-description: d\n"
    "template:\n  path: ./sqls\n"
    "connections:\n  inmem:\n    properties:\n      database: ':memory:'\n"
    "mcp:\n  enabled: true\n"
)


def protected(path, method="GET", extra="", user="u", password="p"):
    return (f"url-path: {path}\nmethod: {method}\ntemplate-source: secret.sql\nconnection: [inmem]\n"
            "auth:\n  enabled: true\n  type: basic\n  roles: [admin]\n  users:\n"
            f"    - username: {user}\n      password: {password}\n      roles: [admin]\n" + extra)


def pbkdf2_hash(password, salt, iterations=2000):
    """The `$pbkdf2-sha256$<iter>$<salt>$<hash>` format flapi verifies (base64url, no padding)."""
    enc = lambda b: base64.urlsafe_b64encode(b).decode().rstrip("=")
    key = hashlib.pbkdf2_hmac("sha256", password.encode(), salt, iterations, 32)
    return f"$pbkdf2-sha256${iterations}${enc(salt)}${enc(key)}"


def open_endpoint(path, method="GET", extra=""):
    return (f"url-path: {path}\nmethod: {method}\ntemplate-source: open.sql\nconnection: [inmem]\n"
            "auth:\n  enabled: false\n" + extra)


def _project(files):
    """`files` is a list of (relative path, text), written in that order."""
    root = tempfile.mkdtemp(prefix="flapi_precedence_")
    os.makedirs(os.path.join(root, "sqls"))
    with open(os.path.join(root, "flapi.yaml"), "w") as f:
        f.write(BASE_CONFIG)
    with open(os.path.join(root, "sqls", "secret.sql"), "w") as f:
        f.write("SELECT 'PROTECTED' AS marker\n")
    with open(os.path.join(root, "sqls", "open.sql"), "w") as f:
        f.write("SELECT 'OPEN' AS marker\n")
    for rel, text in files:
        with open(os.path.join(root, rel), "w") as f:
            f.write(text)
        time.sleep(0.02)
    return root


def _spawn(root, port):
    log = os.path.join(root, "server.log")
    proc = subprocess.Popen(
        [flapi_binary(), "-c", os.path.join(root, "flapi.yaml"), "-p", str(port),
         "--config-service", "--config-service-token", TOKEN, "--log-level", "info"],
        stdout=open(log, "w"), stderr=subprocess.STDOUT, cwd=root,
        env={**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"})
    return proc, log


class _Server:
    def __init__(self, files):
        self.root = _project(files)
        self.port = free_port()
        self.base = f"http://127.0.0.1:{self.port}"
        self.proc, self.log = _spawn(self.root, self.port)
        for _ in range(300):
            if self.proc.poll() is not None:
                pytest.fail("server exited:\n" + self.output()[-3000:])
            try:
                if requests.get(f"{self.base}/health/live", timeout=2).status_code == 200:
                    return
            except requests.RequestException:
                pass
            time.sleep(0.2)
        pytest.fail("server did not start:\n" + self.output()[-3000:])

    def output(self):
        with open(self.log) as f:
            return f.read()

    def get(self, path, **kw):
        return requests.get(f"{self.base}{path}", timeout=5, **kw)

    def assert_protected(self, *paths):
        for path in paths:
            r = self.get(path)
            assert r.status_code == 401, (path, r.status_code, r.text)
            assert "OPEN" not in r.text
            ok = self.get(path, auth=("u", "p"))
            assert ok.status_code == 200 and "PROTECTED" in ok.text, (path, ok.status_code, ok.text)

    def mcp(self, tool, **args):
        r = requests.post(f"{self.base}/mcp/jsonrpc", headers=H, timeout=15,
                          data=json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                                           "params": {"name": tool, "arguments": args}}))
        return r.json()

    @staticmethod
    def slug(url_path):
        # PathUtils::pathToSlug; ':' passes through and is percent-encoded.
        return url_path.replace("~", "~0").replace("-", "~1").replace("/", "-").replace(":", "%3A")

    def stop(self):
        self.proc.terminate()
        self.proc.wait(timeout=30)


def _startup_error(files):
    root = _project(files)
    proc, log = _spawn(root, free_port())
    try:
        proc.wait(timeout=60)
    except subprocess.TimeoutExpired:
        proc.kill()
        pytest.fail("the configuration did not stop startup:\n" + open(log).read()[-3000:])
    assert proc.returncode != 0
    return open(log).read()


def _validate(files):
    root = _project(files)
    r = subprocess.run([flapi_binary(), "-c", os.path.join(root, "flapi.yaml"), "--validate-config"],
                       capture_output=True, text=True, cwd=root, timeout=60,
                       env={**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"})
    return r.returncode, r.stdout + r.stderr


def _is_error(resp):
    return "error" in resp or resp.get("result", {}).get("isError")


class TestMethodSpelling:
    @pytest.mark.parametrize("spelling", ["get", "Get", "''"])
    def test_a_lowercase_or_empty_method_beside_a_protected_get_stops_startup(self, spelling):
        out = _startup_error([("sqls/a_protected.yaml", protected("/customers/:id")),
                              ("sqls/b_open.yaml", open_endpoint("/customers/:cid", method=spelling))])
        assert "Duplicate endpoint GET /customers/:cid (same route as /customers/:id)" in out, out[-2000:]

    def test_validate_config_reports_it_too(self):
        code, out = _validate([("sqls/a.yaml", protected("/customers/:id")),
                               ("sqls/b.yaml", open_endpoint("/customers/:id", method="get"))])
        assert code != 0
        assert "Duplicate endpoint GET /customers/:id" in out, out[-2000:]

    def test_an_unsupported_method_is_a_load_error(self):
        code, out = _validate([("sqls/a.yaml", open_endpoint("/a", method="FETCH"))])
        assert code != 0
        assert "method 'FETCH' is not supported" in out and "GET, POST, PUT, PATCH, DELETE" in out, out[-2000:]

    def test_create_with_a_lowercase_method_cannot_shadow_and_is_stored_canonically(self):
        s = _Server([("sqls/a.yaml", protected("/customers/:id"))])
        try:
            body = {"url-path": "/customers/:cid", "method": "get", "template-source": "open.sql",
                    "connection": ["inmem"], "auth": {"enabled": False}}
            r = requests.post(f"{s.base}/api/v1/_config/endpoints", headers=H, json=body, timeout=10)
            assert r.status_code == 409, r.text
            assert "Duplicate endpoint GET /customers/:cid" in r.json()["error"], r.text

            resp = s.mcp("flapi_create_endpoint", path="/customers/:cid", method="get",
                         **{"template-source": "open.sql"}, connection=["inmem"], auth={"enabled": False})
            assert _is_error(resp), resp
            assert "/customers/:id" in json.dumps(resp), resp   # names the endpoint it conflicts with
            s.assert_protected("/customers/1")

            # A free path: created, and stored as GET.
            body = {"url-path": "/other", "method": "post", "template-source": "open.sql",
                    "connection": ["inmem"]}
            r = requests.post(f"{s.base}/api/v1/_config/endpoints", headers=H, json=body, timeout=10)
            assert r.status_code == 201, r.text
            with open(os.path.join(s.root, "sqls", "other.yaml")) as f:
                assert "method: POST" in f.read()
            bad = {"url-path": "/other2", "method": "fetch", "template-source": "open.sql",
                   "connection": ["inmem"]}
            r = requests.post(f"{s.base}/api/v1/_config/endpoints", headers=H, json=bad, timeout=10)
            assert r.status_code == 400 and "not supported" in r.json()["error"], r.text
        finally:
            s.stop()

    def test_update_handles_the_method_like_rest_put(self):
        s = _Server([("sqls/a.yaml", protected("/customers/:id"))])
        try:
            slug = s.slug("/customers/:id")
            # The same method in another spelling is not a change - REST and MCP agree.
            r = requests.put(f"{s.base}/api/v1/_config/endpoints/{slug}", headers=H,
                             json={"method": "get", "description": "rest"}, timeout=10)
            assert r.status_code == 200, r.text
            resp = s.mcp("flapi_update_endpoint", path="/customers/:id", config={"method": "get",
                                                                                 "description": "mcp"})
            assert not _is_error(resp), resp
            with open(os.path.join(s.root, "sqls", "a.yaml")) as f:
                text = f.read()
            assert "method: GET" in text and "mcp" in text, text
            # A real change of method, or an unsupported one, is refused by both.
            for method, reason in (("POST", "changes the endpoint's url-path, method or name"),
                                   ("fetch", "is not supported")):
                r = requests.put(f"{s.base}/api/v1/_config/endpoints/{slug}", headers=H,
                                 json={"method": method}, timeout=10)
                assert r.status_code == 400 and reason in r.json()["error"], (method, r.text)
                resp = s.mcp("flapi_update_endpoint", path="/customers/:id", config={"method": method})
                assert _is_error(resp) and reason in json.dumps(resp), (method, resp)
            s.assert_protected("/customers/1")
        finally:
            s.stop()


class TestOverlappingRoutes:
    @pytest.mark.parametrize("open_route", ["/:z/:x/b", "/a/:y/:z"])
    @pytest.mark.parametrize("open_first", [True, False])
    def test_the_more_specific_protected_route_answers_in_any_file_order(self, open_route, open_first):
        # A protected, more specific route over an open one is a split the user
        # must opt into (cluster I); with the opt-in, precedence is order-free.
        files = [("sqls/protected.yaml", protected("/a/:x/b", extra="allow-shadowing: true\n")),
                 ("sqls/open.yaml", open_endpoint(open_route))]
        s = _Server(files[::-1] if open_first else files)
        try:
            s.assert_protected("/a/1/b")
            assert "OPEN" in s.get("/zz/1/b" if open_route == "/:z/:x/b" else "/a/1/c").text
            out = s.output()
            assert "Overlapping routes with different auth: GET /a/:x/b (basic, roles [admin]) takes precedence " \
                   f"over GET {open_route} (open)" in out, out[-3000:]
            assert f"{open_route} still serves every other URL its pattern matches" in out, out[-3000:]
        finally:
            s.stop()

    @pytest.mark.parametrize("literal_first", [True, False])
    def test_a_protected_literal_over_an_open_parameter_route_stops_startup(self, literal_first):
        # Cluster I-1: `/customers/7` protected over an open `/customers/:id`
        # used to load with a WARNING, and /customers/07 and /customers/+7 - the
        # same record - were served by the open endpoint without credentials.
        files = [("sqls/c_open.yaml", open_endpoint("/customers/:id").replace("open.sql", "secret.sql")),
                 ("sqls/c_protected.yaml", protected("/customers/7"))]
        out = _startup_error(files[::-1] if literal_first else files)
        assert ("Overlapping routes with different auth: GET /customers/7 (basic, roles [admin]) takes precedence "
                "over GET /customers/:id (open) for /customers/7") in out, out[-3000:]
        assert "/customers/:id still serves every other URL its pattern matches" in out
        assert "such as /customers/07" in out
        assert "allow-shadowing: true` on /customers/7" in out
        code, vout = _validate(files)
        assert code != 0 and "Overlapping routes with different auth" in vout, vout[-2000:]

    def test_a_literal_with_its_own_guest_user_stops_startup(self):
        # Cluster I-2: same scheme and roles, different users counted as "same
        # auth" and the guest's credentials opened /customers/7.
        out = _startup_error([("sqls/a.yaml", protected("/customers/:id", password="admin-pw-123")),
                              ("sqls/b.yaml", protected("/customers/7", user="guest", password="guest-pw-456"))])
        assert "auth differs in: users" in out, out[-3000:]
        assert "admin-pw-123" not in out and "guest-pw-456" not in out

    def test_a_literal_with_a_different_jwt_secret_stops_startup(self):
        def bearer(path, secret):
            return (f"url-path: {path}\nmethod: GET\ntemplate-source: secret.sql\nconnection: [inmem]\n"
                    f"auth:\n  enabled: true\n  type: bearer\n  jwt-secret: {secret}\n  jwt-issuer: iss\n")
        out = _startup_error([("sqls/a.yaml", bearer("/customers/:id", "jwt-secret-AAAA")),
                              ("sqls/b.yaml", bearer("/customers/7", "jwt-secret-BBBB"))])
        assert "auth differs in: jwt-secret" in out, out[-3000:]
        assert "jwt-secret-AAAA" not in out and "jwt-secret-BBBB" not in out

    def test_a_create_of_a_protected_literal_over_an_open_route_is_refused(self):
        s = _Server([("sqls/c_open.yaml", open_endpoint("/customers/:id"))])
        try:
            body = {"url-path": "/customers/7", "method": "GET", "template-source": "secret.sql",
                    "connection": ["inmem"],
                    "auth": {"enabled": True, "type": "basic", "roles": ["admin"],
                             "users": [{"username": "u", "password": "p", "roles": ["admin"]}]}}
            r = requests.post(f"{s.base}/api/v1/_config/endpoints", headers=H, json=body, timeout=10)
            assert r.status_code == 409, r.text
            assert "still serves every other URL" in r.json()["error"], r.text
            assert "OPEN" in s.get("/customers/7").text   # unchanged: one open endpoint, nothing pretended
        finally:
            s.stop()

    @pytest.mark.parametrize("literal_first", [True, False])
    def test_an_open_literal_over_a_protected_parameter_route_stops_startup(self, literal_first):
        files = [("sqls/c_protected.yaml", protected("/customers/:id")),
                 ("sqls/c_open.yaml", open_endpoint("/customers/7"))]
        out = _startup_error(files[::-1] if literal_first else files)
        assert ("Overlapping routes with different auth: GET /customers/7 (open) takes precedence over "
                "GET /customers/:id (basic, roles [admin]) for /customers/7") in out, out[-3000:]
        assert "allow-shadowing: true" in out

    def test_allow_shadowing_is_the_explicit_opt_in(self):
        s = _Server([("sqls/c_protected.yaml", protected("/customers/:id")),
                     ("sqls/c_open.yaml", open_endpoint("/customers/7", extra="allow-shadowing: true\n"))])
        try:
            assert "OPEN" in s.get("/customers/7").text
            s.assert_protected("/customers/8")
            assert "(allow-shadowing: true)" in s.output()
        finally:
            s.stop()

    def test_create_of_an_open_literal_over_a_protected_route_is_refused(self):
        s = _Server([("sqls/c_protected.yaml", protected("/customers/:id"))])
        try:
            body = {"url-path": "/customers/7", "method": "GET", "template-source": "open.sql",
                    "connection": ["inmem"], "auth": {"enabled": False}}
            r = requests.post(f"{s.base}/api/v1/_config/endpoints", headers=H, json=body, timeout=10)
            assert r.status_code == 409, r.text
            assert "Overlapping routes with different auth" in r.json()["error"], r.text
            resp = s.mcp("flapi_create_endpoint", path="/customers/7", method="GET",
                         **{"template-source": "open.sql"}, connection=["inmem"])
            assert _is_error(resp) and "Overlapping routes" in json.dumps(resp), resp
            assert not os.path.exists(os.path.join(s.root, "sqls", "customers_7.yaml"))
            s.assert_protected("/customers/7", "/customers/8")
        finally:
            s.stop()

    # --- #221: the comparison is about who gets in, not how it is spelled ---

    def test_a_plaintext_password_and_its_hash_are_the_same_user(self):
        # A migration: /customers/:id still has `password: p`, /customers/7 was
        # already moved to a PBKDF2 hash of the same password. Same credentials,
        # same people - refusing that was a false positive.
        files = [("sqls/a.yaml", protected("/customers/:id", password="p")),
                 ("sqls/b.yaml", protected("/customers/7", password=pbkdf2_hash("p", b"salt-salt-salt-1")))]
        s = _Server(files)
        try:
            s.assert_protected("/customers/7", "/customers/8")
        finally:
            s.stop()

    def test_a_hash_of_a_different_password_is_still_a_different_user(self):
        out = _startup_error([("sqls/a.yaml", protected("/customers/:id", password="admin-pw-123")),
                              ("sqls/b.yaml", protected("/customers/7",
                                                        password=pbkdf2_hash("guest-pw-456", b"salt-salt-salt-1")))])
        assert "auth differs in: users" in out, out[-3000:]
        assert "admin-pw-123" not in out and "guest-pw-456" not in out

    def test_two_hashes_with_different_salts_cannot_be_proven_equal_and_stay_refused(self):
        # PBKDF2 is one-way: without the plaintext, two salted hashes of one
        # password are indistinguishable from hashes of two passwords. Fail
        # closed; the documented fix is to use the same hash string in both.
        out = _startup_error([("sqls/a.yaml", protected("/customers/:id",
                                                        password=pbkdf2_hash("p", b"salt-salt-salt-1"))),
                              ("sqls/b.yaml", protected("/customers/7",
                                                        password=pbkdf2_hash("p", b"salt-salt-salt-2")))])
        assert "auth differs in: users" in out, out[-3000:]

    def test_the_same_hash_string_in_both_endpoints_is_the_same_user(self):
        h = pbkdf2_hash("p", b"salt-salt-salt-1")
        s = _Server([("sqls/a.yaml", protected("/customers/:id", password=h)),
                     ("sqls/b.yaml", protected("/customers/7", password=h))])
        try:
            s.assert_protected("/customers/7", "/customers/8")
        finally:
            s.stop()

    @pytest.mark.parametrize("spelled_out_first", [True, False])
    def test_an_oidc_preset_and_its_spelled_out_issuer_are_the_same_provider(self, spelled_out_first):
        def oidc(path, body):
            return (f"url-path: {path}\nmethod: GET\ntemplate-source: secret.sql\nconnection: [inmem]\n"
                    "auth:\n  enabled: true\n  type: oidc\n  oidc:\n" + body)
        preset = ("    provider-type: keycloak\n    domain: kc.example.com\n    realm: acme\n"
                  "    client-id: flapi\n    allowed-audiences: [flapi]\n")
        spelled = ("    issuer-url: https://kc.example.com/realms/acme\n    client-id: flapi\n"
                   "    allowed-audiences: [flapi]\n    username-claim: preferred_username\n"
                   "    role-claim-path: realm_access.roles\n    roles-claim: roles\n"
                   "    groups-claim: groups\n    scopes: [openid, profile, email]\n")
        files = [("sqls/a.yaml", oidc("/customers/:id", preset)), ("sqls/b.yaml", oidc("/customers/7", spelled))]
        s = _Server(files[::-1] if spelled_out_first else files)
        try:
            assert s.get("/customers/7").status_code == 401
        finally:
            s.stop()

    def test_oidc_with_a_different_issuer_is_still_different(self):
        def oidc(path, issuer):
            return (f"url-path: {path}\nmethod: GET\ntemplate-source: secret.sql\nconnection: [inmem]\n"
                    f"auth:\n  enabled: true\n  type: oidc\n  oidc:\n    issuer-url: {issuer}\n"
                    "    client-id: flapi\n")
        out = _startup_error([("sqls/a.yaml", oidc("/customers/:id", "https://one.example.com/realms/a")),
                              ("sqls/b.yaml", oidc("/customers/7", "https://two.example.com/realms/a"))])
        assert "auth differs in: oidc" in out, out[-3000:]

    # --- three levels: judged against the endpoint that actually wins ---

    def test_an_open_literal_under_an_allowed_open_middle_route_starts(self):
        # /a/b/c would shadow the protected /a/:x/:y, but /a/b/:y (open,
        # allow-shadowing) already answers /a/b/c over it: no new exposure.
        # Loaded in path order, so the middle route is in place first.
        s = _Server([("sqls/a_protected.yaml", protected("/a/:x/:y")),
                     ("sqls/b_middle.yaml", open_endpoint("/a/b/:y", extra="allow-shadowing: true\n")),
                     ("sqls/c_literal.yaml", open_endpoint("/a/b/c"))])
        try:
            assert "OPEN" in s.get("/a/b/c").text
            assert "OPEN" in s.get("/a/b/z").text
            s.assert_protected("/a/q/c", "/a/x/y")
        finally:
            s.stop()

    def test_an_open_literal_the_middle_route_does_not_cover_is_still_refused(self):
        # /a/:x/c is not inside /a/b/:y, so /a/q/c would be answered by it, open,
        # instead of by the protected /a/:x/:y.
        out = _startup_error([("sqls/a_protected.yaml", protected("/a/:x/:y")),
                              ("sqls/b_middle.yaml", open_endpoint("/a/b/:y", extra="allow-shadowing: true\n")),
                              ("sqls/c_other.yaml", open_endpoint("/a/:x/c"))])
        assert "Overlapping routes with different auth" in out, out[-3000:]

    def test_a_literal_whose_auth_differs_from_the_middle_route_is_still_refused(self):
        # The middle route is open; /a/b/c is protected by another user, so it
        # would not answer what the open middle route answers today.
        out = _startup_error([("sqls/a_protected.yaml", protected("/a/:x/:y")),
                              ("sqls/b_middle.yaml", open_endpoint("/a/b/:y", extra="allow-shadowing: true\n")),
                              ("sqls/c_literal.yaml", protected("/a/b/c", user="g", password="g-pw-9"))])
        assert "Overlapping routes with different auth" in out, out[-3000:]

    # --- MCP create never opts out of shadowing ---

    @pytest.mark.parametrize("spelling", ["allow-shadowing", "allow_shadowing", "allowShadowing"])
    def test_mcp_create_cannot_opt_into_shadowing(self, spelling):
        s = _Server([("sqls/c_protected.yaml", protected("/customers/:id"))])
        try:
            resp = s.mcp("flapi_create_endpoint", path="/customers/7", method="GET",
                         **{"template-source": "open.sql", spelling: True}, connection=["inmem"],
                         auth={"enabled": False})
            assert _is_error(resp) and "Overlapping routes with different auth" in json.dumps(resp), resp
            assert not os.path.exists(os.path.join(s.root, "sqls", "customers_7.yaml"))
            s.assert_protected("/customers/7")
        finally:
            s.stop()

    def test_reloading_the_protected_file_never_flips_a_url_open(self):
        # Allowed with the opt-in on the protected literal: reloads, by slug,
        # by file and after a no-op edit, must keep /customers/7 at 401.
        s = _Server([("sqls/c_protected.yaml", protected("/customers/7", extra="allow-shadowing: true\n")),
                     ("sqls/c_open.yaml", open_endpoint("/customers/:id"))])
        try:
            s.assert_protected("/customers/7")
            protected_file = os.path.join(s.root, "sqls", "c_protected.yaml")
            for _ in range(2):
                r = requests.post(f"{s.base}/api/v1/_config/endpoints/{s.slug('/customers/7')}/reload",
                                  headers=H, timeout=10)
                assert r.status_code == 200, r.text
                s.assert_protected("/customers/7")
                with open(protected_file, "a") as f:
                    f.write("\n")
                r = requests.post(f"{s.base}/api/v1/_config/endpoints/reload-file", headers=H,
                                  json={"file": protected_file}, timeout=10)
                assert r.status_code == 200, r.text
                s.assert_protected("/customers/7")
                r = requests.post(f"{s.base}/api/v1/_config/endpoints/reload-file", headers=H,
                                  json={"file": os.path.join(s.root, "sqls", "c_open.yaml")}, timeout=10)
                assert r.status_code == 200, r.text
                s.assert_protected("/customers/7")
            assert "OPEN" in s.get("/customers/8").text
        finally:
            s.stop()

    def test_an_edit_or_update_that_opens_the_specific_route_is_refused(self):
        s = _Server([("sqls/c_any.yaml", protected("/customers/:id")),
                     ("sqls/c_seven.yaml", protected("/customers/7"))])
        try:
            seven = os.path.join(s.root, "sqls", "c_seven.yaml")
            original = open(seven).read()
            # Edited on disk, then reloaded.
            with open(seven, "w") as f:
                f.write(open_endpoint("/customers/7").replace("open.sql", "secret.sql"))
            r = requests.post(f"{s.base}/api/v1/_config/endpoints/reload-file", headers=H,
                              json={"file": seven}, timeout=10)
            assert r.status_code == 409 and "Overlapping routes" in r.text, r.text
            s.assert_protected("/customers/7")
            with open(seven, "w") as f:
                f.write(original)
            # Through the config service and the MCP tool.
            slug = s.slug("/customers/7")
            r = requests.put(f"{s.base}/api/v1/_config/endpoints/{slug}", headers=H,
                             json={"auth": {"enabled": False}}, timeout=10)
            assert r.status_code == 409 and "Overlapping routes" in r.text, r.text
            assert open(seven).read() == original, "a refused update must leave the file as it was"
            resp = s.mcp("flapi_update_endpoint", path="/customers/7", config={"auth": {"enabled": False}})
            assert _is_error(resp) and "Overlapping routes" in json.dumps(resp), resp
            s.assert_protected("/customers/7", "/customers/8")
        finally:
            s.stop()


class TestUrlPathValidation:
    def test_a_brace_parameter_is_a_load_error_with_a_hint(self):
        code, out = _validate([("sqls/a.yaml", open_endpoint("/customers/{cid}"))])
        assert code != 0
        assert "/customers/{cid}" in out and "`:cid`" in out, out[-2000:]

    @pytest.mark.parametrize("bad", ["/customers/{cid}", "/a/*", "/a(b)", "/a/[x]", "/a+b", "/a|b",
                                     "/a/../b", "/a/./b", "/a/:id/b/:id"])
    def test_create_refuses_it_and_the_server_keeps_answering(self, bad):
        s = _Server([("sqls/a.yaml", protected("/customers/:id"))])
        try:
            body = {"url-path": bad, "method": "GET", "template-source": "open.sql", "connection": ["inmem"]}
            r = requests.post(f"{s.base}/api/v1/_config/endpoints", headers=H, json=body, timeout=10)
            assert r.status_code == 400, r.text
            resp = s.mcp("flapi_create_endpoint", path=bad, **{"template-source": "open.sql"},
                         connection=["inmem"])
            assert _is_error(resp), resp
            # The server answers, promptly, everything that used to hang behind
            # a route that did not compile.
            started = time.monotonic()
            assert requests.get(f"{s.base}/health/live", timeout=2).status_code == 200
            assert requests.get(f"{s.base}/customers/1", timeout=2).status_code == 401
            assert requests.get(f"{s.base}/nothing/here", timeout=2).status_code == 404
            assert time.monotonic() - started < 2
        finally:
            s.stop()

    def test_trailing_slash_case_and_encoding_variants_do_not_bypass(self):
        s = _Server([("sqls/a.yaml", protected("/customers/:id")),
                     ("sqls/b.yaml", open_endpoint("/public/"))])
        try:
            s.assert_protected("/customers/1")
            for variant in ("/customers/1/", "/Customers/1", "//customers/1", "/customers/%3Aid"):
                r = s.get(variant)
                assert "OPEN" not in r.text and r.status_code in (401, 404), (variant, r.status_code, r.text)
            assert requests.head(f"{s.base}/customers/1", timeout=5).status_code != 200
        finally:
            s.stop()


    def test_a_non_ascii_character_is_named_whole(self):
        code, out = _validate([("sqls/a.yaml", open_endpoint("/caf\u00e9"))])
        assert code != 0
        assert "contains '\u00e9' (U+00E9)" in out, out[-2000:]


class TestMcpCreate:
    def test_create_takes_auth_warns_when_open_and_allows_another_method(self):
        s = _Server([("sqls/x_get.yaml", open_endpoint("/x"))])
        try:
            # POST /x beside GET /x is a different endpoint: it used to be refused
            # as "Endpoint already exists" (looked up by path alone).
            resp = s.mcp("flapi_create_endpoint", path="/x", method="post",
                         **{"template-source": "open.sql"}, connection=["inmem"])
            assert not _is_error(resp), resp
            result = json.loads(resp["result"]["content"][0]["text"])
            assert result["method"] == "POST" and result["auth"] == "none", result
            assert "unauthenticated" in result["warning"], result
            assert "WITHOUT authentication" in s.output()

            auth = {"enabled": True, "type": "basic", "roles": ["admin"],
                    "users": [{"username": "u", "password": "p", "roles": ["admin"]}]}
            resp = s.mcp("flapi_create_endpoint", path="/secured/:id", **{"template-source": "secret.sql"},
                         connection=["inmem"], auth=auth)
            assert not _is_error(resp), resp
            result = json.loads(resp["result"]["content"][0]["text"])
            assert result["auth"] == "basic" and "warning" not in result, result
            s.assert_protected("/secured/1")

            # The same endpoint again names the one it conflicts with.
            resp = s.mcp("flapi_create_endpoint", path="/secured/:other", **{"template-source": "open.sql"},
                         connection=["inmem"], auth={"enabled": False})
            assert _is_error(resp) and "/secured/:id" in json.dumps(resp), resp
            s.assert_protected("/secured/1")
        finally:
            s.stop()
