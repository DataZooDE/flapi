"""`auth.from-aws-secretmanager` must not hand its secrets to SQL templates (#254).

The Secrets-Manager auth source used to bootstrap itself inside the MAIN DuckDB,
the one every endpoint template runs on:

1. its `init` SQL ran `CREATE OR REPLACE SECRET <id> (TYPE S3, KEY_ID '..', SECRET '..')`
   there, with `secret-id` / `secret-key` pasted in unescaped;
2. the fetched user list (usernames, passwords, roles) was stored in a plain
   table `<secret-table>(j JSON)` there.

So any template could `SELECT * FROM auth_<name>` or list `duckdb_secrets()`.

A fake Secrets Manager (`GetSecretValue` over the AWS JSON 1.1 protocol) stands
in for AWS. flAPI reaches it through `AWS_ENDPOINT_URL_SECRETS_MANAGER`, falling
back to `AWS_ENDPOINT_URL` - the standard AWS SDK variables. AWS SDK for C++
1.11.428 (vcpkg) does not read them by itself, so flAPI has to apply them to the
client's `endpointOverride`.
"""
import http.server
import json
import os
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
SECRET_NAME = "flapi/users"
SECRET_TABLE = "auth_flapi_users"   # ConfigManager::secretNameToTableName
SECRET_ID = "flapi_users"           # ConfigManager::secretNameToSecretId
USERS = {"auth": [{"username": "alice", "password": PASSWORD, "roles": ["reader"]}]}


class FakeSecretsManager(http.server.BaseHTTPRequestHandler):
    """AWS Secrets Manager, GetSecretValue only. No signature check; records calls."""
    protocol_version = "HTTP/1.1"
    secrets = {}
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
        target = self.headers.get("X-Amz-Target", "")
        try:
            body = json.loads(raw or b"{}")
        except ValueError:
            body = {}
        FakeSecretsManager.calls.append({"target": target, "body": body,
                                         "authorization": self.headers.get("Authorization", "")})
        if target != "secretsmanager.GetSecretValue":
            return self._send(400, {"__type": "UnknownOperationException", "message": target})
        secret_id = body.get("SecretId")
        if secret_id not in FakeSecretsManager.secrets:
            return self._send(400, {"__type": "ResourceNotFoundException",
                                    "message": "Secrets Manager can't find the specified secret."})
        self._send(200, {"ARN": "arn:aws:secretsmanager:eu-central-1:000000000000:secret:x",
                         "Name": secret_id,
                         "SecretString": FakeSecretsManager.secrets[secret_id],
                         "VersionId": "v1"})

    def do_GET(self):
        FakeSecretsManager.calls.append({"target": f"GET {self.path}", "body": {}, "authorization": ""})
        self._send(404, {"__type": "NotFound"})


@pytest.fixture
def secretsmanager():
    FakeSecretsManager.secrets = {SECRET_NAME: json.dumps(USERS)}
    FakeSecretsManager.calls = []
    httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), FakeSecretsManager)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    yield f"http://127.0.0.1:{httpd.server_address[1]}"
    httpd.shutdown()


def sm_env(endpoint, var="AWS_ENDPOINT_URL_SECRETS_MANAGER"):
    """No ambient AWS identity, no instance metadata: only the fake can answer."""
    env = {k: v for k, v in ENV.items() if not k.startswith("AWS_")}
    env.update({var: endpoint,
                "AWS_EC2_METADATA_DISABLED": "true",
                "AWS_CONFIG_FILE": "/nonexistent/aws-config",
                "AWS_SHARED_CREDENTIALS_FILE": "/nonexistent/aws-credentials"})
    return env


def write_project(workdir, secret_key=SECRET_KEY):
    put(workdir, "flapi.yaml",
        "project-name: sm-auth\ntemplate:\n  path: ./sqls\nconnections:\n  inmem:\n    properties:\n"
        "      database: ':memory:'\nrate_limit:\n  enabled: false\n")
    put(workdir, "sqls/protected.yaml",
        "url-path: /protected\nmethod: GET\ntemplate-source: protected.sql\nconnection: [inmem]\n"
        "auth:\n  enabled: true\n  type: basic\n  from-aws-secretmanager:\n"
        f"    secret-name: {SECRET_NAME}\n    region: eu-central-1\n"
        f"    secret-id: {ACCESS_KEY_ID}\n"
        f"    secret-key: {json.dumps(secret_key)}\n")   # a JSON string is a valid YAML scalar
    put(workdir, "sqls/protected.sql", "SELECT 1 AS ok")
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


def start(workdir, env, log_level="info"):
    """Start flapi; fail clearly (not hang) when it does not come up."""
    port = free_port()
    log_path = os.path.join(workdir, f"server-{port}.log")
    log = open(log_path, "w")
    proc = subprocess.Popen(
        [flapi_binary(), "-c", os.path.join(workdir, "flapi.yaml"), "-p", str(port), "--log-level", log_level],
        cwd=workdir, stdout=log, stderr=subprocess.STDOUT, preexec_fn=os.setsid, env=env)

    def output():
        log.flush()
        return open(log_path).read()

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


@pytest.mark.parametrize("endpoint_var", ["AWS_ENDPOINT_URL_SECRETS_MANAGER", "AWS_ENDPOINT_URL"])
def test_basic_auth_against_a_secrets_manager_user(secretsmanager, workdir, endpoint_var):
    write_project(workdir)
    proc, port, output = start(workdir, sm_env(secretsmanager, endpoint_var))
    try:
        assert fetched_from_fake(), "the user list was not fetched from the endpoint in " + endpoint_var
        ok = requests.get(f"http://127.0.0.1:{port}/protected", auth=("alice", PASSWORD), timeout=30)
        assert ok.status_code == 200, ok.text + output()[-2000:]
        bad = requests.get(f"http://127.0.0.1:{port}/protected", auth=("alice", "wrong"), timeout=30)
        assert bad.status_code == 401, bad.text
    finally:
        stop(proc)


def test_a_template_cannot_read_the_fetched_user_list(secretsmanager, workdir):
    write_project(workdir)
    proc, port, output = start(workdir, sm_env(secretsmanager))
    try:
        assert fetched_from_fake(), "precondition: the user list must have been fetched"
        r = requests.get(f"http://127.0.0.1:{port}/peek-users", timeout=30)
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
        r = requests.get(f"http://127.0.0.1:{port}/peek-secrets", timeout=30)
        assert r.status_code == 200, r.text
        names = [row["name"] for row in r.json()["data"]]
        assert SECRET_ID not in names, \
            f"the Secrets-Manager bootstrap credentials are a DuckDB secret templates can list: {names}"
    finally:
        stop(proc)


def test_a_secret_key_with_an_apostrophe_still_authenticates(secretsmanager, workdir):
    write_project(workdir, secret_key="abc'def")
    proc, port, output = start(workdir, sm_env(secretsmanager))
    try:
        assert fetched_from_fake(), "the user list was not fetched"
        r = requests.get(f"http://127.0.0.1:{port}/protected", auth=("alice", PASSWORD), timeout=30)
        assert r.status_code == 200, r.text
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
