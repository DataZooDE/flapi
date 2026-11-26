"""Exit-gate fixes (#211, cluster G2), against a real server.

* Basic auth: a successful PBKDF2 check is remembered (a wrong password never
  is), so only the first request with a password pays ~165 ms; a rotated
  password reloaded from its file invalidates it.
* Metrics latency is measured from the start of the request, so it includes
  authentication; latency values are rounded to microseconds.
* An unknown `auth.type` stops the server at load and is refused on reload.
* MCP tools/call errors carry a generic message and the request id, not
  template paths or SQL; bad credentials are "not accepted", not "required".
* Every Crow "Response:" line carries its request id; with `log-format: json`
  the first startup line is JSON too.
* REST write RETURNING rows and MCP structuredContent keep column order; a
  0-row MCP write result has `data: []`.
* The plugin's .mcp.json headers (X-Config-Token + an empty `Authorization:
  Bearer `) work against a server with mcp.auth on and off.
"""
import base64
import hashlib
import json
import os
import re
import shutil
import subprocess
import tempfile
import time
from types import SimpleNamespace

import jwt
import pytest
import requests

from otel_helpers import flapi_binary, free_port

pytestmark = pytest.mark.standalone_server

TOKEN = "g2-config-token"
JWT_SECRET = "g2-mcp-jwt-secret-long-enough-for-hs256"
CFG_AUTH = {"X-Config-Token": TOKEN}
MCP_HEADERS = {"Accept": "application/json, text/event-stream", "Content-Type": "application/json"}


def pbkdf2(password: str, iterations: int = 600_000) -> str:
    salt = os.urandom(16)
    digest = hashlib.pbkdf2_hmac("sha256", password.encode(), salt, iterations)
    b64 = lambda b: base64.b64encode(b).decode().rstrip("=")  # noqa: E731
    return f"$pbkdf2-sha256${iterations}${b64(salt)}${b64(digest)}"


def config(mcp_auth: bool) -> str:
    text = """\
project-name: g2-test
project-description: g2
template:
  path: ./sqls
connections:
  db:
    init: |
      CREATE TABLE IF NOT EXISTS items (zeta INTEGER, alpha VARCHAR, mid DOUBLE);
    properties:
      x: '1'
log-format: json
rate_limit:
  enabled: false
mcp:
  enabled: true
"""
    if mcp_auth:
        text += f"""\
  auth:
    enabled: true
    type: bearer
    jwt-secret: {JWT_SECRET}
    jwt-issuer: g2
"""
    return text


def basic_endpoint(stored: str) -> str:
    return f"""\
url-path: /basic
method: GET
template-source: rows.sql
connection: [db]
auth:
  type: basic
  users:
    - username: alice
      password: '{stored}'
"""


FILES = {
    "rows.sql": "SELECT 3 AS zeta, 'a' AS alpha, 1.5 AS mid, 4 AS beta\n",
    "open.yaml": """\
url-path: /open
method: GET
template-source: rows.sql
connection: [db]
mcp-tool:
  name: open_tool
  description: rows in a fixed column order
""",
    "broken.yaml": """\
url-path: /broken
method: GET
template-source: broken.sql
connection: [db]
mcp-tool:
  name: broken_tool
  description: reads a file that does not exist
""",
    "broken.sql": "SELECT * FROM read_csv('/nonexistent/g2/secret-path.csv')\n",
    "write.yaml": """\
url-path: /items
method: POST
template-source: write.sql
connection: [db]
operation:
  type: write
  returns-data: true
request:
  - field-name: alpha
    field-in: body
    required: true
    validators:
      - type: string
mcp-tool:
  name: write_tool
  description: insert one row
""",
    "write.sql": "INSERT INTO items VALUES (7, '{{{ params.alpha }}}', 2.25) RETURNING zeta, alpha, mid\n",
    "none.yaml": """\
url-path: /items-none
method: POST
template-source: none.sql
connection: [db]
operation:
  type: write
  returns-data: true
mcp-tool:
  name: none_tool
  description: deletes nothing
""",
    "none.sql": "DELETE FROM items WHERE zeta = -1 RETURNING zeta, alpha\n",
}


def _wait(base, proc, log_path):
    for _ in range(300):
        if proc.poll() is not None:
            raise RuntimeError("server exited: " + open(log_path).read()[-3000:])
        try:
            if requests.get(f"{base}/health/live", timeout=2).status_code == 200:
                return
        except requests.RequestException:
            pass
        time.sleep(0.2)
    raise RuntimeError("server did not start")


def start(mcp_auth: bool, password: str = "secret"):
    root = tempfile.mkdtemp(prefix="flapi_g2_")
    sqls = os.path.join(root, "sqls")
    os.makedirs(sqls)
    with open(os.path.join(root, "flapi.yaml"), "w") as f:
        f.write(config(mcp_auth))
    for name, body in FILES.items():
        if mcp_auth and name == "open.yaml":
            # Under mcp.auth a tool needs allowed-roles to be callable at all.
            body += "  allowed-roles: [agent]\n"
        with open(os.path.join(sqls, name), "w") as f:
            f.write(body)
    with open(os.path.join(sqls, "basic.yaml"), "w") as f:
        f.write(basic_endpoint(pbkdf2(password)))
    port = free_port()
    log_path = os.path.join(root, "server.log")
    log = open(log_path, "w")
    proc = subprocess.Popen([flapi_binary(), "-c", "flapi.yaml", "-p", str(port), "--config-service",
                             "--config-service-token", TOKEN], cwd=root, stdout=log, stderr=subprocess.STDOUT,
                            env={**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"})
    base = f"http://127.0.0.1:{port}"
    _wait(base, proc, log_path)
    return SimpleNamespace(base=base, root=root, sqls=sqls, proc=proc, log_path=log_path, log=log)


def stop(server):
    server.proc.terminate()
    server.proc.wait(timeout=30)
    server.log.close()
    shutil.rmtree(server.root, ignore_errors=True)


@pytest.fixture(scope="module")
def server():
    s = start(mcp_auth=False)
    try:
        yield s
    finally:
        stop(s)


@pytest.fixture(scope="module")
def mcp_auth_server():
    s = start(mcp_auth=True)
    try:
        yield s
    finally:
        stop(s)


def log_lines(server):
    with open(server.log_path) as f:
        return f.read().splitlines()


def mcp(server, method, params, headers=None):
    body = {"jsonrpc": "2.0", "id": 1, "method": method, "params": params}
    return requests.post(f"{server.base}/mcp/jsonrpc", data=json.dumps(body),
                         headers={**MCP_HEADERS, **(headers or {})}, timeout=60)


def timed_get(url, **kwargs):
    t0 = time.perf_counter()
    r = requests.get(url, timeout=60, **kwargs)
    return r, time.perf_counter() - t0


def test_basic_auth_remembers_only_successful_verifications(server):
    r, first = timed_get(f"{server.base}/basic", auth=("alice", "secret"))
    assert r.status_code == 200, r.text
    hits = []
    for _ in range(3):
        r, elapsed = timed_get(f"{server.base}/basic", auth=("alice", "secret"))
        assert r.status_code == 200
        hits.append(elapsed)
    # The first request derives PBKDF2 (600k iterations); the rest do not.
    assert max(hits) < first / 3, (first, hits)

    wrong = []
    for _ in range(2):
        r, elapsed = timed_get(f"{server.base}/basic", auth=("alice", "nope"))
        assert r.status_code == 401
        wrong.append(elapsed)
    # A wrong password is checked in full every time - never remembered.
    assert min(wrong) > first / 3, (first, wrong)


def test_metrics_latency_includes_authentication_and_is_rounded(server):
    requests.post(f"{server.base}/api/v1/_config/metrics/reset", headers=CFG_AUTH, timeout=10)
    # A fresh password is not in the verification cache: this request pays PBKDF2.
    r, client_s = timed_get(f"{server.base}/basic", auth=("alice", "nope"))
    assert r.status_code == 401
    raw = requests.get(f"{server.base}/api/v1/_config/metrics?endpoint=/basic", headers=CFG_AUTH,
                       timeout=10).text
    series = [s for s in json.loads(raw)["requests"]["series"] if s.get("route") == "/basic"]
    assert series, raw
    # Measured from the start of the request: the PBKDF2 check is inside it.
    assert series[0]["latency_ms"]["max"] >= client_s * 1000 * 0.5, (client_s, series)
    # No latency value has more than three decimals in the JSON text.
    for number in re.findall(r'"(?:avg|p50|p95|p99|max)":(-?[0-9.eE+-]+)', raw):
        if "." in number:
            assert len(number.split(".")[1]) <= 3, number


def test_rotated_password_reloaded_from_its_file_invalidates_the_cache(server):
    assert requests.get(f"{server.base}/basic", auth=("alice", "secret"), timeout=60).status_code == 200
    path = os.path.join(server.sqls, "basic.yaml")
    original = open(path).read()
    try:
        with open(path, "w") as f:
            f.write(basic_endpoint(pbkdf2("rotated", iterations=1000)))
        r = requests.post(f"{server.base}/api/v1/_config/endpoints/reload-file", headers=CFG_AUTH,
                          json={"file": path}, timeout=30)
        assert r.status_code == 200, r.text
        assert requests.get(f"{server.base}/basic", auth=("alice", "secret"), timeout=60).status_code == 401
        assert requests.get(f"{server.base}/basic", auth=("alice", "rotated"), timeout=60).status_code == 200
    finally:
        with open(path, "w") as f:
            f.write(original)
        requests.post(f"{server.base}/api/v1/_config/endpoints/reload-file", headers=CFG_AUTH,
                      json={"file": path}, timeout=30)


def test_unknown_auth_type_is_refused_on_reload(server):
    path = os.path.join(server.sqls, "typo.yaml")
    with open(path, "w") as f:
        f.write(basic_endpoint("pw").replace("/basic", "/typo").replace("type: basic", "type: basci"))
    try:
        r = requests.post(f"{server.base}/api/v1/_config/endpoints/reload-file", headers=CFG_AUTH,
                          json={"file": path}, timeout=30)
        assert r.status_code == 400, r.text
        assert "AUTH_UNKNOWN_TYPE" in r.text and "basci" in r.text, r.text
        assert "basic, bearer, jwt (alias of bearer), oidc" in r.text, r.text
    finally:
        os.remove(path)


def test_unknown_auth_type_stops_the_server_at_load(tmp_path):
    (tmp_path / "sqls").mkdir()
    (tmp_path / "flapi.yaml").write_text(config(False))
    (tmp_path / "sqls" / "rows.sql").write_text(FILES["rows.sql"])
    (tmp_path / "sqls" / "typo.yaml").write_text(basic_endpoint("pw").replace("type: basic", "type: basci"))
    proc = subprocess.run([flapi_binary(), "-c", "flapi.yaml", "--validate-config", "--no-telemetry"],
                          cwd=tmp_path, capture_output=True, text=True, timeout=120)
    output = proc.stdout + proc.stderr
    assert proc.returncode != 0, output
    assert "AUTH_UNKNOWN_TYPE" in output and "GET /basic" in output and "'basci'" in output, output


def test_response_lines_carry_the_request_id(server):
    r1 = requests.get(f"{server.base}/open", timeout=30)
    r2 = requests.get(f"{server.base}/basic", auth=("alice", "nope"), timeout=60)
    r3 = requests.get(f"{server.base}/basic", timeout=30)
    assert (r1.status_code, r2.status_code, r3.status_code) == (200, 401, 401)
    wanted = {r.headers["X-Request-Id"] for r in (r1, r2, r3)}
    time.sleep(0.5)
    found = set()
    for line in log_lines(server):
        if line.startswith("{"):
            record = json.loads(line)
            if record["message"].startswith("Response: ") and record.get("request_id") in wanted:
                found.add(record["request_id"])
    assert found == wanted, (wanted, found)


def test_json_log_format_applies_from_the_first_line(server):
    lines = [line for line in log_lines(server) if line.strip()]
    assert lines, "no log output"
    first = json.loads(lines[0])
    assert "Loading configuration" in first["message"], first


def test_rest_write_returning_keeps_column_order(server):
    r = requests.post(f"{server.base}/items", json={"alpha": "q"}, timeout=30)
    assert r.status_code == 201, r.text
    assert r.text == '{"rows_affected":1,"data":[{"zeta":7,"alpha":"q","mid":2.25}]}', r.text
    r = requests.post(f"{server.base}/items-none", timeout=30)
    assert r.text == '{"rows_affected":0,"data":[]}', r.text


def test_mcp_structured_content_keeps_column_order(server):
    r = mcp(server, "tools/call", {"name": "open_tool", "arguments": {}})
    assert r.status_code == 200, r.text
    assert '"rows":[{"zeta":3,"alpha":"a","mid":1.5,"beta":4}]' in r.text, r.text
    r = mcp(server, "tools/call", {"name": "write_tool", "arguments": {"alpha": "z"}})
    assert '"data":[{"zeta":7,"alpha":"z","mid":2.25}]' in r.text, r.text
    r = mcp(server, "tools/call", {"name": "none_tool", "arguments": {}})
    result = r.json()["result"]
    assert result["structuredContent"]["rows"] == {"rows_affected": 0, "data": []}, result


def test_mcp_tool_errors_do_not_leak_paths_or_sql(server):
    r = mcp(server, "tools/call", {"name": "broken_tool", "arguments": {}})
    assert r.status_code == 200, r.text
    result = r.json()["result"]
    assert result["isError"] is True
    text = result["content"][0]["text"]
    assert "/nonexistent" not in r.text and "read_csv" not in r.text, r.text
    match = re.search(r"request_id (req-[0-9a-f]+)", text)
    assert match, text
    request_id = match.group(1)
    time.sleep(0.3)
    detail = [line for line in log_lines(server) if request_id in line and "secret-path.csv" in line]
    assert detail, "the detail must stay in the server log under the request id"


def test_mcp_argument_errors_stay_actionable(server):
    r = mcp(server, "tools/call", {"name": "write_tool", "arguments": {}})
    result = r.json()["result"]
    assert result["isError"] is True
    assert "alpha" in result["content"][0]["text"], result


def test_mcp_bad_jwt_is_not_accepted_rather_than_required(mcp_auth_server):
    r = mcp(mcp_auth_server, "tools/call", {"name": "open_tool", "arguments": {}},
            {"Authorization": "Bearer not-a-valid-token"})
    assert r.status_code == 401
    assert r.json()["error"]["message"] == "The credentials sent were not accepted.", r.text

    r = mcp(mcp_auth_server, "tools/call", {"name": "open_tool", "arguments": {}})
    assert r.status_code == 401
    assert r.json()["error"]["message"] == "Authentication required for method: tools/call", r.text


def plugin_headers(mcp_token=""):
    """What plugins/flapi/.mcp.json sends, with its ${VAR:-} expanded."""
    with open(os.path.join(os.path.dirname(__file__), "..", "..", "plugins", "flapi", ".mcp.json")) as f:
        headers = json.load(f)["mcpServers"]["flapi"]["headers"]
    values = {"FLAPI_CONFIG_SERVICE_TOKEN": TOKEN, "FLAPI_MCP_TOKEN": mcp_token}
    return {k: re.sub(r"\$\{(\w+):-\}", lambda m: values.get(m.group(1), ""), v) for k, v in headers.items()}


def test_plugin_headers_work_without_mcp_auth(server):
    headers = plugin_headers()
    assert headers["X-Config-Token"] == TOKEN
    assert headers["Authorization"].strip() == "Bearer"
    r = mcp(server, "tools/list", {}, headers)
    assert r.status_code == 200, r.text
    r = mcp(server, "tools/call", {"name": "open_tool", "arguments": {}}, headers)
    assert r.status_code == 200 and "isError" not in r.json()["result"], r.text
    # A config tool authenticates with the X-Config-Token header.
    r = mcp(server, "tools/call", {"name": "flapi_get_project_config", "arguments": {}}, headers)
    assert r.status_code == 200, r.text
    assert "error" not in r.json(), r.text


def test_plugin_headers_work_with_mcp_auth(mcp_auth_server):
    token = jwt.encode({"sub": "agent", "iss": "g2", "roles": ["agent"], "exp": int(time.time()) + 600},
                       JWT_SECRET, algorithm="HS256")
    headers = plugin_headers(token)
    r = mcp(mcp_auth_server, "tools/call", {"name": "open_tool", "arguments": {}}, headers)
    assert r.status_code == 200 and "isError" not in r.json()["result"], r.text
    r = mcp(mcp_auth_server, "tools/call", {"name": "flapi_get_project_config", "arguments": {}}, headers)
    assert r.status_code == 200 and "error" not in r.json(), r.text
    # FLAPI_MCP_TOKEN unset: the empty bearer is no credential at all.
    r = mcp(mcp_auth_server, "tools/call", {"name": "open_tool", "arguments": {}}, plugin_headers())
    assert r.status_code == 401
    assert "Authentication required" in r.json()["error"]["message"], r.text
