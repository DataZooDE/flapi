"""`auth.*` template values stay inside the SQL literal the template wrote.

`auth.username`, `auth.email`, `auth.roles` and `auth.type` come from the
identity provider - a Basic username, a JWT `sub`/`roles`, OIDC claims - and
are rendered into the template, never bound as prepared parameters. The
documented row-filter pattern is

    WHERE owner = '{{{ auth.username }}}'

so an identity containing a single quote must match as that literal string,
not change the statement. An identity with a control character is refused,
and a template placing auth.* anywhere but a plain '...' literal is refused at
load, by --validate-config, by the config service and at render time.

These tests run the real binary over REST (Basic and Bearer), MCP tools/call
and resources/read (Basic and Bearer) and the config service, against a table
holding rows of several owners.
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

PASSWORD = "correct-horse"
ATTACKER = "alice' OR '1'='1"
JWT_SECRET = "auth-quoting-jwt-secret-long-enough-for-hs256"
JWT_ISSUER = "auth-quoting"
CONFIG_TOKEN = "auth-quoting-config-token"

# bob owns two rows, carol one, O'Brien one. Nobody owns anything as ATTACKER.
ROWS = [(1, "bob"), (2, "bob"), (3, "carol"), (4, "O'Brien")]
SAFE_TEMPLATE = "SELECT id, owner FROM docs WHERE owner = '{{{ auth.username }}}' ORDER BY id\n"

QUERY_TEMPLATE = ("SELECT * FROM query('SELECT id, owner FROM docs "
                  "WHERE owner = ''{{{ auth.username }}}'' ORDER BY id')\n")
MACRO_TEMPLATE = ("SELECT * FROM tenant_rows('SELECT id, owner FROM docs "
                  "WHERE owner = $${{{ auth.username }}}$$ ORDER BY id')\n")
OUTER_FILTER_TEMPLATE = ("SELECT * FROM query('SELECT id, owner FROM docs') "
                         "WHERE owner = '{{{ auth.username }}}' ORDER BY id\n")

# Positions where a doubled quote does not keep a value inside its literal.
UNSAFE_TEMPLATES = {
    "unquoted": "SELECT id, owner FROM docs WHERE owner = {{{ auth.username }}}\n",
    "line_comment": "SELECT id, owner FROM docs WHERE owner = 'nobody' -- {{{ auth.username }}}\n",
    "block_comment": "SELECT id, owner FROM docs /* {{{ auth.username }}} */ WHERE owner = 'nobody'\n",
    "e_string": "SELECT id, owner FROM docs WHERE owner = E'{{{ auth.username }}}'\n",
    "identifier": "SELECT id, owner AS \"{{{ auth.username }}}\" FROM docs WHERE owner = 'nobody'\n",
    "dollar": "SELECT id, owner FROM docs WHERE owner = $${{{ auth.username }}}$$\n",
    "section": "{{#auth}}SELECT id, owner FROM docs WHERE owner = {{{username}}}{{/auth}}\n",
    # SQL text that another function executes: the doubled quote becomes a
    # plain quote when that function parses it.
    "query_fn": QUERY_TEMPLATE,
    "query_concat": ("SELECT * FROM query(concat('SELECT id, owner FROM docs WHERE owner = ''', "
                     "'{{{ auth.username }}}', '''')) ORDER BY id\n"),
    # SQL text recognised by its content, and executing calls across comments.
    "macro_dollar": MACRO_TEMPLATE,
    "macro_concat": ("SELECT * FROM tenant_rows('SELECT id, owner FROM docs WHERE owner = $q$' || "
                     "'{{{ auth.username }}}' || '$q$ ORDER BY id')\n"),
    "query_comment": ("SELECT * FROM query/**/('SELECT id, owner FROM docs WHERE owner = $$' || "
                      "'{{{ auth.username }}}' || '$$')\n"),
    "query_quoted": ("SELECT * FROM \"query\"('SELECT id, owner FROM docs WHERE owner = $q$' || "
                     "'{{{ auth.username }}}' || '$q$')\n"),
    # A Mustache tag between two SQL characters hides them from a source scan.
    "comment_tag_e": "SELECT id, owner FROM docs WHERE owner = E{{! c }}'{{{ auth.username }}}'\n",
    "section_e": ("SELECT id, owner FROM docs WHERE owner = "
                  "{{#auth.authenticated}}E{{/auth.authenticated}}'{{{ auth.username }}}'\n"),
    "dollar_tag": "SELECT id, owner FROM docs WHERE owner = ${{! }}${{{ auth.username }}}$$\n",
    "dash_tag": "SELECT id, owner FROM docs WHERE owner = 'nobody' -{{! }}- '{{{ auth.username }}}'\n",
    "slash_tag": "SELECT id, owner FROM docs WHERE owner = 'nobody' /{{! }}* '{{{ auth.username }}}' */\n",
}


def _yaml_str(value: str) -> str:
    # JSON strings are valid YAML double-quoted scalars.
    return json.dumps(value)


def _sql_literal(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


def _b64url(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode()


def _jwt(sub: str, roles=("reader",)) -> str:
    now = int(time.time())
    header = _b64url(json.dumps({"alg": "HS256", "typ": "JWT"}).encode())
    payload = _b64url(json.dumps({
        "iss": JWT_ISSUER, "sub": sub, "roles": list(roles),
        "iat": now, "exp": now + 3600,
    }).encode())
    signature = hmac.new(JWT_SECRET.encode(), f"{header}.{payload}".encode(),
                         hashlib.sha256).digest()
    return f"{header}.{payload}.{_b64url(signature)}"


def _bearer(sub: str) -> dict:
    return {"Authorization": f"Bearer {_jwt(sub)}"}


def _users_yaml(indent: str) -> str:
    out = ""
    for name in ("bob", "carol", "O'Brien", ATTACKER):
        out += (f"{indent}- username: {_yaml_str(name)}\n"
                f"{indent}  password: {PASSWORD}\n"
                f"{indent}  roles: [reader]\n")
    return out


def _write_project(tmp: str, port: int, mcp_auth: str, template: str = SAFE_TEMPLATE) -> None:
    sqls = os.path.join(tmp, "sqls")
    os.makedirs(sqls, exist_ok=True)
    with open(os.path.join(sqls, "docs.sql"), "w") as f:
        f.write(template)

    # REST, Basic auth, and the same template as an MCP tool.
    with open(os.path.join(sqls, "docs.yaml"), "w") as f:
        f.write("url-path: /docs\nmethod: GET\n"
                "template-source: docs.sql\nconnection: [inmem]\n"
                "mcp-tool:\n  name: my_docs\n  description: The caller's documents.\n"
                "  allowed-roles:\n    - reader\n"
                "auth:\n  enabled: true\n  type: basic\n  users:\n"
                + _users_yaml("    "))

    # REST, Bearer JWT: the identity is the token's `sub`.
    with open(os.path.join(sqls, "jdocs.yaml"), "w") as f:
        f.write("url-path: /jdocs\nmethod: GET\n"
                "template-source: docs.sql\nconnection: [inmem]\n"
                "auth:\n  enabled: true\n  type: bearer\n"
                f"  jwt-secret: {JWT_SECRET}\n  jwt-issuer: {JWT_ISSUER}\n")

    # The same template as an MCP resource.
    with open(os.path.join(sqls, "rdocs.yaml"), "w") as f:
        f.write("url-path: /rdocs\nmethod: GET\n"
                "template-source: docs.sql\nconnection: [inmem]\n"
                "mcp-resource:\n  name: my_docs_resource\n"
                "  description: The caller's documents.\n"
                "  mime-type: application/json\n"
                "  allowed-roles:\n    - reader\n"
                "auth:\n  enabled: true\n  type: basic\n  users:\n"
                + _users_yaml("    "))

    if mcp_auth == "basic":
        mcp_auth_yaml = "  auth:\n    enabled: true\n    type: basic\n    users:\n" + _users_yaml("      ")
    else:
        mcp_auth_yaml = ("  auth:\n    enabled: true\n    type: bearer\n"
                         f"    jwt-secret: {JWT_SECRET}\n    jwt-issuer: {JWT_ISSUER}\n")

    values = ", ".join(f"({i}, {_sql_literal(o)})" for i, o in ROWS)
    init_sql = (f"CREATE OR REPLACE TABLE docs AS SELECT * FROM (VALUES {values}) t(id, owner); "
                # A macro that runs its argument as SQL, unknown to any name list.
                "CREATE OR REPLACE MACRO tenant_rows(q) AS TABLE SELECT * FROM query(q);")
    with open(os.path.join(tmp, "flapi.yaml"), "w") as f:
        f.write(
            "project-name: auth-quoting\n"
            "project-description: auth values stay inside their SQL literal\n"
            f"http-port: {port}\n"
            "template:\n  path: ./sqls\n"
            "connections:\n  inmem:\n"
            f"    init: {_yaml_str(init_sql)}\n"
            "    properties:\n      database: ':memory:'\n"
            "rate_limit:\n  enabled: false\n"
            "mcp:\n  enabled: true\n" + mcp_auth_yaml)


class _Server:
    def __init__(self, mcp_auth="basic", config_service=False):
        self.tmp = tempfile.mkdtemp(prefix="flapi_authquote_")
        self.port = free_port()
        self.base_url = f"http://127.0.0.1:{self.port}"
        self.log_path = os.path.join(self.tmp, "server.log")
        self.config_service = config_service
        _write_project(self.tmp, self.port, mcp_auth)
        self.proc = None

    def start(self):
        extra = ["--config-service", "--config-service-token", CONFIG_TOKEN] if self.config_service else []
        self.proc = subprocess.Popen(
            [flapi_binary(), "-c", os.path.join(self.tmp, "flapi.yaml"),
             "-p", str(self.port), "--log-level", "warning"] + extra,
            stdout=open(self.log_path, "w"), stderr=subprocess.STDOUT, cwd=self.tmp,
            env={**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"},
            preexec_fn=os.setsid)
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
        pytest.fail(f"server did not start:\n{open(self.log_path).read()[-3000:]}")

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

    def __enter__(self):
        return self.start()

    def __exit__(self, *exc):
        self.stop()


@pytest.fixture(scope="module")
def server():
    with _Server() as s:
        yield s


@pytest.fixture(scope="module")
def bearer_mcp_server():
    with _Server(mcp_auth="bearer") as s:
        yield s


def _rows(data):
    return [(row["id"], row["owner"]) for row in data]


def _rest(server, path, **kwargs):
    r = requests.get(f"{server.base_url}{path}", timeout=15, **kwargs)
    assert r.status_code == 200, (r.status_code, r.text)
    return _rows(r.json()["data"])


def _mcp_post(server, method, params, auth=None, headers=None):
    body = {"jsonrpc": "2.0", "id": 1, "method": method, "params": params}
    return requests.post(f"{server.base_url}/mcp/jsonrpc",
                         headers={"Content-Type": "application/json", **(headers or {})},
                         data=json.dumps(body), auth=auth, timeout=15)


def _mcp(server, auth=None, headers=None):
    r = _mcp_post(server, "tools/call", {"name": "my_docs", "arguments": {}},
                  auth=auth, headers=headers).json()
    assert "result" in r, r
    assert not r["result"].get("isError"), r
    return _rows(r["result"]["structuredContent"]["rows"])


def _mcp_resource(server, auth):
    r = _mcp_post(server, "resources/read", {"uri": "flapi://my_docs_resource"}, auth=auth).json()
    assert "result" in r, r
    payload = json.loads(r["result"]["contents"][0]["text"])
    data = payload["data"] if isinstance(payload, dict) and "data" in payload else payload
    return _rows(data)


class TestBasicUsername:
    def test_a_normal_user_sees_exactly_their_own_rows(self, server):
        assert _rest(server, "/docs", auth=("bob", PASSWORD)) == [(1, "bob"), (2, "bob")]
        assert _rest(server, "/docs", auth=("carol", PASSWORD)) == [(3, "carol")]

    def test_a_quote_in_a_legitimate_name_still_matches(self, server):
        assert _rest(server, "/docs", auth=("O'Brien", PASSWORD)) == [(4, "O'Brien")]

    def test_a_quote_in_the_username_does_not_widen_the_filter(self, server):
        rows = _rest(server, "/docs", auth=(ATTACKER, PASSWORD))
        assert rows == [], f"the username changed the WHERE clause: {rows!r}"


class TestBearerSubject:
    def test_a_normal_subject_sees_exactly_their_own_rows(self, server):
        assert _rest(server, "/jdocs", headers=_bearer("bob")) == [(1, "bob"), (2, "bob")]

    def test_a_quote_in_the_subject_does_not_widen_the_filter(self, server):
        rows = _rest(server, "/jdocs", headers=_bearer("x' OR 1=1 --"))
        assert rows == [], f"the JWT subject changed the WHERE clause: {rows!r}"

    @pytest.mark.parametrize("sub", ["bob\n", "bob\nOR 1=1", "bob\r", "bob\x00x", "bob\x1f"])
    def test_a_control_character_in_the_subject_is_refused(self, server, sub):
        r = requests.get(f"{server.base_url}/jdocs", headers=_bearer(sub), timeout=15)
        assert r.status_code == 401, (r.status_code, r.text)
        assert "bob" not in r.text, r.text


class TestCommaInARole:
    """auth.roles is comma-joined, and MCP allowed-roles splits the same CSV:
    a role named "reader,admin" would read as two roles."""

    def test_rest_refuses_a_role_with_a_comma(self, server):
        token = _jwt("bob", roles=("reader,admin",))
        r = requests.get(f"{server.base_url}/jdocs",
                         headers={"Authorization": f"Bearer {token}"}, timeout=15)
        assert r.status_code == 401, (r.status_code, r.text)

    def test_mcp_refuses_a_role_with_a_comma(self, bearer_mcp_server):
        token = _jwt("bob", roles=("reader,admin",))
        r = _mcp_post(bearer_mcp_server, "tools/call", {"name": "my_docs", "arguments": {}},
                      headers={"Authorization": f"Bearer {token}"})
        assert r.status_code == 401, (r.status_code, r.text)

    def test_separate_roles_still_work(self, server, bearer_mcp_server):
        token = _jwt("bob", roles=("reader", "admin"))
        assert _rest(server, "/jdocs", headers={"Authorization": f"Bearer {token}"}) == [(1, "bob"), (2, "bob")]
        assert _mcp(bearer_mcp_server, headers={"Authorization": f"Bearer {token}"}) == [(1, "bob"), (2, "bob")]


class TestMcpToolsCall:
    def test_a_normal_user_sees_exactly_their_own_rows(self, server):
        assert _mcp(server, ("bob", PASSWORD)) == [(1, "bob"), (2, "bob")]
        assert _mcp(server, ("O'Brien", PASSWORD)) == [(4, "O'Brien")]

    def test_a_quote_in_the_username_does_not_widen_the_filter(self, server):
        rows = _mcp(server, (ATTACKER, PASSWORD))
        assert rows == [], f"the MCP identity changed the WHERE clause: {rows!r}"


class TestMcpBearerSubject:
    def test_a_normal_subject_sees_exactly_their_own_rows(self, bearer_mcp_server):
        assert _mcp(bearer_mcp_server, headers=_bearer("bob")) == [(1, "bob"), (2, "bob")]

    def test_a_quote_in_the_subject_does_not_widen_the_filter(self, bearer_mcp_server):
        assert _mcp(bearer_mcp_server, headers=_bearer("x' OR 1=1 --")) == []

    @pytest.mark.parametrize("sub", ["bob\n", "bob\nOR 1=1", "bob\r", "bob\x00x", "bob\x1f"])
    def test_a_control_character_in_the_subject_is_refused(self, bearer_mcp_server, sub):
        r = _mcp_post(bearer_mcp_server, "tools/call", {"name": "my_docs", "arguments": {}},
                      headers=_bearer(sub))
        assert r.status_code == 401, (r.status_code, r.text)
        assert "result" not in r.json(), r.text
        assert "bob" not in r.text, r.text


class TestMcpResourcesRead:
    def test_a_normal_user_sees_exactly_their_own_rows(self, server):
        assert _mcp_resource(server, ("bob", PASSWORD)) == [(1, "bob"), (2, "bob")]

    def test_a_quote_in_the_username_does_not_widen_the_filter(self, server):
        assert _mcp_resource(server, (ATTACKER, PASSWORD)) == []


def _validate(tmp: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        [flapi_binary(), "-c", os.path.join(tmp, "flapi.yaml"), "--validate-config"],
        cwd=tmp, capture_output=True, text=True, timeout=120,
        env={**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"})


class TestUnsafePlacementIsAnError:
    def test_the_safe_template_validates(self):
        tmp = tempfile.mkdtemp(prefix="flapi_authquote_v_")
        _write_project(tmp, free_port(), "basic")
        result = _validate(tmp)
        assert result.returncode == 0, result.stdout + result.stderr
        assert "auth.username" not in result.stdout + result.stderr

    @pytest.mark.parametrize("kind", sorted(UNSAFE_TEMPLATES))
    def test_validate_config_fails(self, kind):
        tmp = tempfile.mkdtemp(prefix="flapi_authquote_v_")
        _write_project(tmp, free_port(), "basic", template=UNSAFE_TEMPLATES[kind])
        result = _validate(tmp)
        out = result.stdout + result.stderr
        assert result.returncode != 0, out
        assert "auth." in out, out

    @pytest.mark.parametrize("kind", ["unquoted", "line_comment", "e_string", "query_fn", "comment_tag_e"])
    def test_the_server_refuses_to_start(self, kind):
        tmp = tempfile.mkdtemp(prefix="flapi_authquote_s_")
        port = free_port()
        _write_project(tmp, port, "basic", template=UNSAFE_TEMPLATES[kind])
        proc = subprocess.run(
            [flapi_binary(), "-c", os.path.join(tmp, "flapi.yaml"), "-p", str(port),
             "--log-level", "warning"],
            cwd=tmp, capture_output=True, text=True, timeout=120,
            env={**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"})
        assert proc.returncode != 0, proc.stdout + proc.stderr


class TestSqlRunByAnotherFunction:
    """A literal handed to query() (or bigquery_query, postgres_query, ...) is
    parsed again as SQL, where the doubled quote is a plain quote again."""

    def _headers(self):
        return {"X-Config-Token": CONFIG_TOKEN, "Content-Type": "application/json"}

    def test_query_text_is_refused_live(self):
        with _Server(config_service=True) as s:
            r = requests.put(f"{s.base_url}/api/v1/_config/endpoints/-docs/template",
                             headers=self._headers(),
                             data=json.dumps({"template": QUERY_TEMPLATE}), timeout=15)
            assert r.status_code == 400, (r.status_code, r.text)
            assert "another function executes" in r.text, r.text

            # The same template edited on disk, under the running server.
            with open(os.path.join(s.tmp, "sqls", "docs.sql"), "w") as f:
                f.write(QUERY_TEMPLATE)
            time.sleep(1.1)
            for headers in (_bearer("x' OR 1=1 --"), _bearer("bob")):
                g = requests.get(f"{s.base_url}/jdocs", headers=headers, timeout=15)
                assert g.status_code != 200, g.text
                assert "carol" not in g.text, g.text
            m = _mcp_post(s, "tools/call", {"name": "my_docs", "arguments": {}},
                          auth=(ATTACKER, PASSWORD)).json()
            assert "carol" not in json.dumps(m), m
            assert "another function executes" in s.log(), s.log()[-2000:]

    def test_a_macro_running_sql_text_is_refused_live(self):
        with _Server(config_service=True) as s:
            r = requests.put(f"{s.base_url}/api/v1/_config/endpoints/-docs/template",
                             headers=self._headers(),
                             data=json.dumps({"template": MACRO_TEMPLATE}), timeout=15)
            assert r.status_code == 400, (r.status_code, r.text)
            with open(os.path.join(s.tmp, "sqls", "docs.sql"), "w") as f:
                f.write(MACRO_TEMPLATE)
            time.sleep(1.1)
            for sub in ("x$$ OR 1=1 OR $$", "bob"):
                g = requests.get(f"{s.base_url}/jdocs", headers=_bearer(sub), timeout=15)
                assert g.status_code != 200, g.text
                assert "carol" not in g.text, g.text

    def test_filtering_in_the_outer_query_works(self):
        tmp = tempfile.mkdtemp(prefix="flapi_authquote_o_")
        port = free_port()
        _write_project(tmp, port, "basic", template=OUTER_FILTER_TEMPLATE)
        assert _validate(tmp).returncode == 0
        s = _Server.__new__(_Server)
        s.tmp, s.port = tmp, port
        s.base_url = f"http://127.0.0.1:{port}"
        s.log_path = os.path.join(tmp, "server.log")
        s.config_service = False
        s.proc = None
        with s:
            assert _rest(s, "/jdocs", headers=_bearer("bob")) == [(1, "bob"), (2, "bob")]
            assert _rest(s, "/jdocs", headers=_bearer("x' OR 1=1 --")) == []
            assert _rest(s, "/docs", auth=("O'Brien", PASSWORD)) == [(4, "O'Brien")]


class TestConfigService:
    def _headers(self):
        return {"X-Config-Token": CONFIG_TOKEN, "Content-Type": "application/json"}

    def test_an_unsafe_template_is_not_written(self):
        with _Server(config_service=True) as s:
            r = requests.put(f"{s.base_url}/api/v1/_config/endpoints/-docs/template",
                             headers=self._headers(),
                             data=json.dumps({"template": UNSAFE_TEMPLATES["line_comment"]}),
                             timeout=15)
            assert r.status_code == 400, (r.status_code, r.text)
            with open(os.path.join(s.tmp, "sqls", "docs.sql")) as f:
                assert f.read() == SAFE_TEMPLATE
            assert _rest(s, "/docs", auth=("bob", PASSWORD)) == [(1, "bob"), (2, "bob")]

    def test_a_template_edited_on_disk_is_refused_at_render(self):
        # The file changed under a running server, with no reload: the
        # render-time check is what stands between it and the database.
        with _Server(config_service=True) as s:
            with open(os.path.join(s.tmp, "sqls", "docs.sql"), "w") as f:
                f.write(UNSAFE_TEMPLATES["line_comment"])
            time.sleep(1.1)  # a new mtime for the template cache's stamp
            r = requests.get(f"{s.base_url}/docs", auth=(ATTACKER, PASSWORD), timeout=15)
            assert r.status_code != 200, r.text
            assert "carol" not in r.text, r.text

            t = requests.post(f"{s.base_url}/api/v1/_config/endpoints/-docs/template/test",
                              headers=self._headers(), data=json.dumps({"parameters": {}}),
                              timeout=15)
            # The config service has no caller identity: auth.* renders empty
            # there, so nothing can be injected; it must not return rows.
            assert "carol" not in t.text, t.text
            assert '"bob"' not in t.text, t.text

            m = _mcp_post(s, "tools/call", {"name": "my_docs", "arguments": {}},
                          auth=(ATTACKER, PASSWORD)).json()
            assert "carol" not in json.dumps(m), m
