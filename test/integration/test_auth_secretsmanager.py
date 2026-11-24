"""`auth.from-aws-secretmanager` must not hand its secrets to SQL templates (#254).

The Secrets-Manager auth source used to bootstrap itself inside the MAIN DuckDB,
the one every endpoint template runs on:

1. its `init` SQL ran `CREATE OR REPLACE SECRET <id> (TYPE S3, KEY_ID '..', SECRET '..')`
   there, with `secret-id` / `secret-key` pasted in unescaped;
2. the fetched user list (usernames, passwords, roles) was stored in a plain
   table `<secret-table>(j JSON)` there.

So any template could `SELECT * FROM auth_<name>` or list `duckdb_secrets()`.

A fake Secrets Manager (`GetSecretValue` over the AWS JSON 1.1 protocol) stands
in for AWS. It verifies the SigV4 signature of every request against the secret
key it knows for the access key id, so a test can tell which credentials signed
the call - and that they were the ones configured, byte for byte. flAPI reaches
it through `AWS_ENDPOINT_URL_SECRETS_MANAGER`, falling back to
`AWS_ENDPOINT_URL` - the standard AWS SDK variables. AWS SDK for C++ 1.11.428
(vcpkg) does not read them by itself, so flAPI has to apply them to the
client's `endpointOverride`.
"""
import base64
import hashlib
import hmac
import http.server
import json
import os
import re
import signal
import subprocess
import sys
import threading
import time

import pytest
import requests

from otel_helpers import flapi_binary, free_port
from test_remote_templates import ENV, put, stop, workdir  # noqa: F401
from test_remote_templates_s3 import FakeS3, bucket, s3_env  # noqa: F401

pytestmark = [pytest.mark.standalone_server,
              pytest.mark.skipif(sys.platform == "win32", reason="spawns POSIX process groups (os.setsid / killpg)")]

PASSWORD = "PWCANARY123"
SECRET_KEY = "KEYCANARY456"
ACCESS_KEY_ID = "AKIDCANARY789"
ENV_ACCESS_KEY_ID = "AKIDFROMENV111"
ENV_SECRET_KEY = "ENVKEYCANARY222"
REGION = "eu-central-1"
SECRET_NAME = "flapi/users"
SECRET_TABLE = "auth_flapi_users"   # ConfigManager::secretNameToTableName
SECRET_ID = "flapi_users"           # ConfigManager::secretNameToSecretId
BOB_PASSWORD = "BOBPLAIN321"
TOKEN = "cfg-token-254"


def pbkdf2_hash(password, iterations=1000, salt=b"0123456789abcdef"):
    """`$pbkdf2-sha256$<iter>$<b64-salt>$<b64-hash>`, the format of password_hasher.hpp."""
    derived = hashlib.pbkdf2_hmac("sha256", password.encode(), salt, iterations, 32)
    b64 = lambda raw: base64.urlsafe_b64encode(raw).decode().rstrip("=")  # noqa: E731
    return f"$pbkdf2-sha256${iterations}${b64(salt)}${b64(derived)}"


USERS = {"auth": [{"username": "alice", "password": PASSWORD, "roles": ["reader"]},
                  {"username": "bob", "password": pbkdf2_hash(BOB_PASSWORD), "roles": ["reader"]}]}

SIGV4 = re.compile(r"AWS4-HMAC-SHA256 Credential=(?P<akid>[^/]+)/(?P<date>\d{8})/(?P<region>[^/]+)/"
                   r"(?P<service>[^/]+)/aws4_request, ?SignedHeaders=(?P<signed>[^,]+), ?Signature=(?P<sig>[0-9a-f]{64})")


def sigv4_problem(method, raw_path, headers, body, keys):
    """None when `headers` carry a valid AWS SigV4 signature by a known key, else why not."""
    auth = headers.get("authorization", "")
    m = SIGV4.fullmatch(auth.strip())
    if not m:
        return f"no SigV4 Authorization header: {auth[:80]!r}"
    if m["akid"] not in keys:
        return f"unknown access key id {m['akid']!r}"
    signed = m["signed"].split(";")
    if "host" not in signed:
        return "host is not a signed header"
    payload_hash = headers.get("x-amz-content-sha256") or hashlib.sha256(body).hexdigest()
    if payload_hash != "UNSIGNED-PAYLOAD" and payload_hash != hashlib.sha256(body).hexdigest():
        return "x-amz-content-sha256 does not match the body"
    path, _, query = raw_path.partition("?")
    canonical_query = "&".join(sorted(p for p in query.split("&") if p))
    canonical_headers = "".join(f"{h}:{' '.join(headers.get(h, '').split())}\n" for h in signed)
    canonical_request = "\n".join([method, path or "/", canonical_query, canonical_headers,
                                   m["signed"], payload_hash])
    scope = f"{m['date']}/{m['region']}/{m['service']}/aws4_request"
    to_sign = "\n".join(["AWS4-HMAC-SHA256", headers.get("x-amz-date", ""), scope,
                         hashlib.sha256(canonical_request.encode()).hexdigest()])
    key = ("AWS4" + keys[m["akid"]]).encode()
    for part in (m["date"], m["region"], m["service"], "aws4_request"):
        key = hmac.new(key, part.encode(), hashlib.sha256).digest()
    expected = hmac.new(key, to_sign.encode(), hashlib.sha256).hexdigest()
    if not hmac.compare_digest(expected, m["sig"]):
        return "signature does not verify"
    return None


class FakeSecretsManager(http.server.BaseHTTPRequestHandler):
    """AWS Secrets Manager, GetSecretValue only. Verifies SigV4 and records each call."""
    protocol_version = "HTTP/1.1"
    secrets = {}
    keys = {}       # access key id -> secret access key
    calls = []

    def log_message(self, *args):
        pass

    def _send(self, code, payload):
        body = json.dumps(payload).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/x-amz-json-1.1")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        raw = self.rfile.read(int(self.headers.get("Content-Length") or 0))
        headers = {k.lower(): v for k, v in self.headers.items()}
        target = headers.get("x-amz-target", "")
        try:
            body = json.loads(raw or b"{}")
        except ValueError:
            body = {}
        FakeSecretsManager.calls.append({
            "target": target, "body": body, "headers": headers,
            "authorization": headers.get("authorization", ""),
            "sig_problem": sigv4_problem("POST", self.path, headers, raw, FakeSecretsManager.keys)})
        if target != "secretsmanager.GetSecretValue":
            return self._send(400, {"__type": "UnknownOperationException", "message": target})
        secret_id = body.get("SecretId")
        if secret_id not in FakeSecretsManager.secrets:
            return self._send(400, {"__type": "ResourceNotFoundException",
                                    "message": "Secrets Manager can't find the specified secret."})
        self._send(200, {"ARN": f"arn:aws:secretsmanager:{REGION}:000000000000:secret:x",
                         "Name": secret_id,
                         "SecretString": FakeSecretsManager.secrets[secret_id],
                         "VersionId": "v1"})

    def do_GET(self):
        FakeSecretsManager.calls.append({"target": f"GET {self.path}", "body": {}, "headers": {},
                                         "authorization": "", "sig_problem": "GET"})
        self._send(404, {"__type": "NotFound"})


@pytest.fixture
def secretsmanager():
    FakeSecretsManager.secrets = {SECRET_NAME: json.dumps(USERS)}
    FakeSecretsManager.keys = {ACCESS_KEY_ID: SECRET_KEY, ENV_ACCESS_KEY_ID: ENV_SECRET_KEY}
    FakeSecretsManager.calls = []
    httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), FakeSecretsManager)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    yield f"http://127.0.0.1:{httpd.server_address[1]}"
    httpd.shutdown()


def sm_env(endpoint, var="AWS_ENDPOINT_URL_SECRETS_MANAGER", **extra):
    """No ambient AWS identity, no instance metadata: only the fake can answer."""
    env = {k: v for k, v in ENV.items() if not k.startswith("AWS_")}
    env.update({var: endpoint,
                "AWS_EC2_METADATA_DISABLED": "true",
                "AWS_CONFIG_FILE": "/nonexistent/aws-config",
                "AWS_SHARED_CREDENTIALS_FILE": "/nonexistent/aws-credentials"})
    env.update(extra)
    return env


def sm_block(secret_key=SECRET_KEY, indent="  ", with_keys=True, init=None):
    lines = ["from-aws-secretmanager:", f"  secret-name: {SECRET_NAME}", f"  region: {REGION}"]
    if with_keys:
        lines += [f"  secret-id: {ACCESS_KEY_ID}",
                  f"  secret-key: {json.dumps(secret_key)}"]   # a JSON string is a valid YAML scalar
    if init is not None:
        lines += [f"  init: {json.dumps(init)}"]
    return "".join(f"{indent}{line}\n" for line in lines)


def protected(workdir, name, roles=None, **sm):
    auth = "auth:\n  enabled: true\n  type: basic\n"
    if roles is not None:
        auth += f"  roles: {json.dumps(roles)}\n"
    put(workdir, f"sqls/{name}.yaml",
        f"url-path: /{name}\nmethod: GET\ntemplate-source: ok.sql\nconnection: [inmem]\n"
        + auth + sm_block(indent="  ", **sm))


def write_project(workdir, secret_key=SECRET_KEY, top_level_auth="", extra=""):
    put(workdir, "flapi.yaml",
        "project-name: sm-auth\ntemplate:\n  path: ./sqls\nconnections:\n  inmem:\n    properties:\n"
        "      database: ':memory:'\nrate_limit:\n  enabled: false\n" + top_level_auth + extra)
    put(workdir, "sqls/ok.sql", "SELECT 1 AS ok")
    protected(workdir, "protected", secret_key=secret_key)
    # Two unauthenticated endpoints an author could write - or an attacker who can
    # create one through the config service.
    put(workdir, "sqls/users.yaml",
        "url-path: /peek-users\nmethod: GET\ntemplate-source: users.sql\nconnection: [inmem]\n"
        "auth:\n  enabled: false\n")
    put(workdir, "sqls/users.sql", f"SELECT * FROM {SECRET_TABLE}")
    put(workdir, "sqls/secrets.yaml",
        "url-path: /peek-secrets\nmethod: GET\ntemplate-source: secrets.sql\nconnection: [inmem]\n"
        "auth:\n  enabled: false\n")
    put(workdir, "sqls/secrets.sql", "SELECT name FROM duckdb_secrets()")


def launch(workdir, env, log_level="info", extra_args=()):
    port = free_port()
    log_path = os.path.join(workdir, f"server-{port}.log")
    log = open(log_path, "w")
    proc = subprocess.Popen(
        [flapi_binary(), "-c", os.path.join(workdir, "flapi.yaml"), "-p", str(port), "--log-level", log_level,
         *extra_args],
        cwd=workdir, stdout=log, stderr=subprocess.STDOUT, preexec_fn=os.setsid, env=env)

    def output():
        log.flush()
        return open(log_path).read()

    return proc, port, output


def start(workdir, env, log_level="info", extra_args=()):
    """Start flapi; fail clearly (not hang) when it does not come up."""
    proc, port, output = launch(workdir, env, log_level, extra_args)

    def why():
        return (f"\nfake Secrets Manager saw {len(FakeSecretsManager.calls)} call(s): "
                f"{[c['target'] for c in FakeSecretsManager.calls]}\n--- flapi log (tail) ---\n{output()[-3000:]}")

    deadline = time.time() + 60
    while time.time() < deadline:
        if proc.poll() is not None:
            pytest.fail("flapi exited during startup" + why())
        try:
            if requests.get(f"http://127.0.0.1:{port}/health/live", timeout=1).status_code == 200:
                return proc, port, output
        except requests.RequestException:
            time.sleep(0.3)
    os.killpg(proc.pid, signal.SIGKILL)
    pytest.fail("flapi never became live within 60s" + why())


def fetched_from_fake():
    return [c for c in FakeSecretsManager.calls
            if c["target"] == "secretsmanager.GetSecretValue" and c["body"].get("SecretId") == SECRET_NAME]


def assert_signed_by(akid):
    """Every GetSecretValue was signed - validly - by `akid`, for the configured region, with no token."""
    calls = fetched_from_fake()
    assert calls, "the user list was not fetched"
    for call in calls:
        auth = call["authorization"]
        assert re.search(rf"Credential={re.escape(akid)}/\d{{8}}/{REGION}/secretsmanager/aws4_request", auth), \
            f"not signed by {akid} for {REGION}/secretsmanager: {auth[:160]!r}"
        assert "x-amz-security-token" not in call["headers"], \
            f"a session token was sent although none is configured: {call['headers'].get('x-amz-security-token')!r}"
        assert call["sig_problem"] is None, f"SigV4 check failed: {call['sig_problem']}"


def get(port, path, auth=None):
    return requests.get(f"http://127.0.0.1:{port}{path}", auth=auth, timeout=30)


@pytest.mark.parametrize("endpoint_var", ["AWS_ENDPOINT_URL_SECRETS_MANAGER", "AWS_ENDPOINT_URL"])
def test_basic_auth_against_a_secrets_manager_user(secretsmanager, workdir, endpoint_var):
    write_project(workdir)
    proc, port, output = start(workdir, sm_env(secretsmanager, endpoint_var))
    try:
        assert fetched_from_fake(), "the user list was not fetched from the endpoint in " + endpoint_var
        assert_signed_by(ACCESS_KEY_ID)
        ok = get(port, "/protected", ("alice", PASSWORD))
        assert ok.status_code == 200, ok.text + output()[-2000:]
        bad = get(port, "/protected", ("alice", "wrong"))
        assert bad.status_code == 401, bad.text
    finally:
        stop(proc)


def test_the_service_specific_endpoint_wins_over_the_global_one(secretsmanager, workdir):
    write_project(workdir)
    env = sm_env(secretsmanager, AWS_ENDPOINT_URL="http://127.0.0.1:9")
    proc, port, output = start(workdir, env)
    try:
        assert fetched_from_fake(), "AWS_ENDPOINT_URL_SECRETS_MANAGER did not take precedence"
        assert get(port, "/protected", ("alice", PASSWORD)).status_code == 200
    finally:
        stop(proc)


def test_ignore_configured_endpoint_urls_is_honoured(secretsmanager, workdir):
    write_project(workdir)
    env = sm_env(secretsmanager, AWS_IGNORE_CONFIGURED_ENDPOINT_URLS="true")
    proc, port, output = launch(workdir, env)
    try:
        # It must not reach the fake: it goes to the real regional endpoint
        # instead, where these credentials fail and startup stops - or, in a
        # sandbox without a network, the connection fails. Either way, not here.
        deadline = time.time() + 60
        while time.time() < deadline and proc.poll() is None:
            try:
                if requests.get(f"http://127.0.0.1:{port}/health/live", timeout=1).status_code == 200:
                    break
            except requests.RequestException:
                time.sleep(0.3)
        assert not fetched_from_fake(), \
            "AWS_IGNORE_CONFIGURED_ENDPOINT_URLS=true, yet the configured endpoint was used"
    finally:
        stop(proc)


def test_a_template_cannot_read_the_fetched_user_list(secretsmanager, workdir):
    write_project(workdir)
    proc, port, output = start(workdir, sm_env(secretsmanager))
    try:
        assert fetched_from_fake(), "precondition: the user list must have been fetched"
        r = get(port, "/peek-users")
        # Either the table does not exist where templates run (an error), or it
        # holds nothing of the secret. A 200 carrying the password is the leak.
        assert not (r.status_code == 200 and PASSWORD in r.text), \
            f"an unauthenticated template read the Secrets-Manager user list: {r.text[:500]}"
        assert PASSWORD not in r.text, r.text[:500]
    finally:
        stop(proc)


def test_a_template_cannot_see_the_auth_bootstrap_secret(secretsmanager, workdir):
    write_project(workdir)
    proc, port, output = start(workdir, sm_env(secretsmanager))
    try:
        r = get(port, "/peek-secrets")
        assert r.status_code == 200, r.text
        names = [row["name"] for row in r.json()["data"]]
        assert SECRET_ID not in names, \
            f"the Secrets-Manager bootstrap credentials are a DuckDB secret templates can list: {names}"
    finally:
        stop(proc)


def test_the_schema_api_does_not_list_the_user_table(secretsmanager, workdir):
    write_project(workdir)
    proc, port, output = start(workdir, sm_env(secretsmanager),
                               extra_args=("--config-service", "--config-service-token", TOKEN))
    try:
        assert fetched_from_fake(), "precondition: the user list must have been fetched"
        r = requests.get(f"http://127.0.0.1:{port}/api/v1/_config/schema",
                         headers={"Authorization": f"Bearer {TOKEN}"}, timeout=30)
        assert r.status_code == 200, r.text[:500]
        assert SECRET_TABLE not in r.text, f"the schema API lists the auth user table: {r.text[:800]}"
    finally:
        stop(proc)


def test_a_secret_key_with_an_apostrophe_still_authenticates(secretsmanager, workdir):
    FakeSecretsManager.keys[ACCESS_KEY_ID] = "abc'def"
    write_project(workdir, secret_key="abc'def")
    proc, port, output = start(workdir, sm_env(secretsmanager))
    try:
        assert fetched_from_fake(), "the user list was not fetched"
        assert_signed_by(ACCESS_KEY_ID)
        r = get(port, "/protected", ("alice", PASSWORD))
        assert r.status_code == 200, r.text
    finally:
        stop(proc)


def test_roles_from_the_secret_are_enforced(secretsmanager, workdir):
    write_project(workdir)
    protected(workdir, "admin-only", roles=["admin"])
    protected(workdir, "reader-only", roles=["reader"])
    proc, port, output = start(workdir, sm_env(secretsmanager))
    try:
        r = get(port, "/admin-only", ("alice", PASSWORD))
        assert r.status_code == 403, f"alice holds only `reader`: {r.status_code} {r.text}"
        r = get(port, "/reader-only", ("alice", PASSWORD))
        assert r.status_code == 200, r.text + output()[-2000:]
    finally:
        stop(proc)


def test_a_pbkdf2_password_in_the_secret_verifies(secretsmanager, workdir):
    write_project(workdir)
    proc, port, output = start(workdir, sm_env(secretsmanager))
    try:
        r = get(port, "/protected", ("bob", BOB_PASSWORD))
        assert r.status_code == 200, r.text + output()[-2000:]
        assert get(port, "/protected", ("bob", "wrong")).status_code == 401
    finally:
        stop(proc)


def test_a_secret_shared_by_several_endpoints_is_fetched_once(secretsmanager, workdir):
    top = "auth:\n  enabled: true\n  type: basic\n" + sm_block(indent="  ")
    write_project(workdir, top_level_auth=top)
    protected(workdir, "second")
    # No `auth:` of its own: the top-level block applies.
    put(workdir, "sqls/inherits.yaml",
        "url-path: /inherits\nmethod: GET\ntemplate-source: ok.sql\nconnection: [inmem]\n")
    proc, port, output = start(workdir, sm_env(secretsmanager))
    try:
        for path in ("/protected", "/second", "/inherits"):
            r = get(port, path, ("alice", PASSWORD))
            assert r.status_code == 200, f"{path}: {r.status_code} {r.text}" + output()[-2000:]
        assert get(port, "/inherits").status_code == 401, "the top-level auth did not apply"
        n = len(fetched_from_fake())
        assert n == 1, f"one secret, {n} GetSecretValue calls"
    finally:
        stop(proc)


def test_a_custom_init_still_authenticates_without_reaching_templates(secretsmanager, workdir):
    init = (f"CREATE OR REPLACE SECRET {SECRET_ID} (TYPE S3, KEY_ID '{ACCESS_KEY_ID}', "
            f"SECRET '{SECRET_KEY}', REGION '{REGION}');")
    write_project(workdir)
    protected(workdir, "protected", with_keys=False, init=init)
    proc, port, output = start(workdir, sm_env(secretsmanager))
    try:
        assert_signed_by(ACCESS_KEY_ID)
        r = get(port, "/protected", ("alice", PASSWORD))
        assert r.status_code == 200, r.text + output()[-2000:]
        r = get(port, "/peek-secrets")
        assert r.status_code == 200, r.text
        names = [row["name"] for row in r.json()["data"]]
        assert SECRET_ID not in names, f"the custom init's secret is visible to templates: {names}"
    finally:
        stop(proc)


def test_without_keys_the_sdk_credential_chain_is_used(secretsmanager, workdir):
    write_project(workdir)
    protected(workdir, "protected", with_keys=False)
    env = sm_env(secretsmanager, AWS_ACCESS_KEY_ID=ENV_ACCESS_KEY_ID, AWS_SECRET_ACCESS_KEY=ENV_SECRET_KEY)
    proc, port, output = start(workdir, env)
    try:
        assert_signed_by(ENV_ACCESS_KEY_ID)
        r = get(port, "/protected", ("alice", PASSWORD))
        assert r.status_code == 200, r.text + output()[-2000:]
    finally:
        stop(proc)


def test_the_bootstrap_credentials_are_not_logged(workdir):
    # The parser logs the generated init SQL at debug level - KEY_ID and SECRET in
    # clear - two lines above a "Secret Key: *****[n]" that meant to redact it.
    write_project(workdir)
    r = subprocess.run(
        [flapi_binary(), "-c", os.path.join(workdir, "flapi.yaml"), "--validate-config", "--log-level", "debug"],
        cwd=workdir, capture_output=True, text=True, timeout=120, env=sm_env("http://127.0.0.1:9"))
    out = r.stdout + r.stderr
    assert "Secrets Manager" in out, "precondition: the auth block was parsed\n" + out[-2000:]
    leaked = [line for line in out.splitlines() if SECRET_KEY in line or ACCESS_KEY_ID in line]
    assert not leaked, "secret-key / secret-id written to the log:\n" + "\n".join(leaked)


@pytest.mark.parametrize("secret_key", [SECRET_KEY, "abc'defKEYCANARY456"], ids=["plain", "apostrophe"])
def test_a_running_server_does_not_log_the_credentials(secretsmanager, workdir, secret_key):
    FakeSecretsManager.keys[ACCESS_KEY_ID] = secret_key
    write_project(workdir, secret_key=secret_key)
    proc, port, output = launch(workdir, sm_env(secretsmanager), log_level="debug")
    try:
        deadline = time.time() + 60
        while time.time() < deadline and proc.poll() is None:
            try:
                if requests.get(f"http://127.0.0.1:{port}/health/live", timeout=1).status_code == 200:
                    get(port, "/protected", ("alice", PASSWORD))
                    break
            except requests.RequestException:
                time.sleep(0.3)
    finally:
        stop(proc)
    out = output()
    assert "Secrets Manager" in out, "precondition: the auth block was parsed\n" + out[-2000:]
    leaked = [line for line in out.splitlines()
              if "CANARY456" in line or ACCESS_KEY_ID in line or PASSWORD in line]
    assert not leaked, "credentials written to the log:\n" + "\n".join(leaked)


SECRETS_SQL = "SELECT name, persistent, secret_string FROM duckdb_secrets() ORDER BY name"
SETTINGS_SQL = "SELECT name, value FROM duckdb_settings() WHERE name LIKE 's3_%'"


def test_remote_config_credentials_are_not_handed_to_templates(bucket, workdir):  # noqa: F811
    """Regression guard: what a template can see of the credentials that fetched a remote config.

    The config is read by a separate bootstrap DuckDB, before the main one exists.
    The same environment credentials are ALSO installed into the main DuckDB, on
    purpose, as the secret `zz_flapi_s3`, so that a template's request-time
    `read_parquet('s3://...')` works. That one is expected to be listed, and is
    redacted. What must not reach a template: any other secret (the bootstrap's
    own), the secret value through `duckdb_secrets().secret_string`, or the
    value through the legacy `s3_*` settings httpfs mirrors from the environment.
    """
    root, endpoint = bucket
    put(root, "mybucket/cfg/flapi.yaml",
        "project-name: remote-cfg\ntemplate:\n  path: ./sqls\nconnections:\n  inmem:\n    properties:\n"
        "      database: ':memory:'\nrate_limit:\n  enabled: false\n")
    put(root, "mybucket/cfg/sqls/secrets.yaml",
        "url-path: /peek-secrets\nmethod: GET\ntemplate-source: secrets.sql\nconnection: [inmem]\n")
    put(root, "mybucket/cfg/sqls/secrets.sql", SECRETS_SQL)
    put(root, "mybucket/cfg/sqls/settings.yaml",
        "url-path: /peek-settings\nmethod: GET\ntemplate-source: settings.sql\nconnection: [inmem]\n")
    put(root, "mybucket/cfg/sqls/settings.sql", SETTINGS_SQL)

    env = {**ENV, **s3_env(endpoint), "AWS_SECRET_ACCESS_KEY": SECRET_KEY}
    port = free_port()
    log_path = os.path.join(workdir, "remote.log")
    proc = subprocess.Popen(
        [flapi_binary(), "-c", "s3://mybucket/cfg/flapi.yaml", "-p", str(port), "--log-level", "info"],
        cwd=workdir, stdout=open(log_path, "w"), stderr=subprocess.STDOUT, preexec_fn=os.setsid, env=env)
    try:
        deadline = time.time() + 60
        while True:
            assert proc.poll() is None, open(log_path).read()[-3000:]
            assert time.time() < deadline, "never became live\n" + open(log_path).read()[-3000:]
            try:
                if requests.get(f"http://127.0.0.1:{port}/health/live", timeout=1).status_code == 200:
                    break
            except requests.RequestException:
                time.sleep(0.3)
        assert any("flapi.yaml" in r for r in FakeS3.requests_seen), FakeS3.requests_seen
        assert any("Credential=AKIATESTKEY/" in a for a in FakeS3.auth_seen), \
            "precondition: the remote config was fetched with the environment credentials"

        r = requests.get(f"http://127.0.0.1:{port}/peek-secrets", timeout=30)
        assert r.status_code == 200, r.text
        # Persistent secrets come from the user's ~/.duckdb/stored_secrets, not
        # from flAPI; a secret flAPI created is temporary.
        names = [row["name"] for row in r.json()["data"] if not row["persistent"]]
        assert names == ["zz_flapi_s3"], f"only the request-time S3 secret may be visible: {names}"
        assert SECRET_KEY not in r.text, "duckdb_secrets() returned the secret value unredacted"

        r = requests.get(f"http://127.0.0.1:{port}/peek-settings", timeout=30)
        assert r.status_code == 200, r.text
        assert SECRET_KEY not in r.text, "the secret value is readable through duckdb_settings()"
    finally:
        stop(proc)
