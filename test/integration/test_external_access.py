"""External-access hardening for templates (#253).

A template is SQL the operator did not necessarily write: with a remote config
(s3://, gs://, az://, http(s)://) whoever can write to the bucket controls every
template, and before this a template could read any local file
(`read_csv('/etc/passwd')`), any URL, any other bucket, ATTACH any file and LOAD
any extension.

flapi.yaml grows a top-level block (top level, because every scalar under
`duckdb:` is passed to DuckDB as a setting):

    duckdb-hardening:
      external-access: auto        # auto (default) | full | restricted
      allowed-directories: []      # extra prefixes
      allowed-paths: []            # extra exact files

`auto` means `restricted` for a config loaded from a remote origin and `full` for
a local one, so local configs see no change. `restricted` preloads the extensions
flAPI needs, sets DuckDB `allowed_directories` (template path or the config's
remote base prefix, the DuckLake data-path, `embed://` and the user's list), then
`enable_external_access=false` and `lock_configuration=true`; flAPI itself refuses
INSTALL/LOAD and secret DDL in templates. The policy is logged once at startup:
`external access: restricted (...)` / `external access: full (...)`.

Every failure message says which of two things went wrong: "SERVER DID NOT START"
(the config was refused - a red for the wrong reason) or "NOT REFUSED" (the server
ran, the read went through).

A refusal is matched on the log lines of THAT request only: the 500 body carries a
`request_id`, and every log line written while the request ran carries it too.
"""
import http.server
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
from types import SimpleNamespace

import pytest
import requests

from otel_helpers import flapi_binary, free_port
from test_remote_templates import ENV, put, stop, workdir  # noqa: F401
from test_remote_templates_s3 import FakeS3, bucket, s3_env  # noqa: F401

pytestmark = [pytest.mark.standalone_server,
              pytest.mark.skipif(sys.platform == "win32", reason="spawns POSIX process groups (os.setsid / killpg)")]

TOKEN = "external-access-token"
AUTH = {"Authorization": f"Bearer {TOKEN}"}

# What a refusal by the policy says:
#   Permission Error: Cannot access file "/etc/passwd" - file system operations are disabled by configuration
#   Permission Error: Loading external extensions is disabled through configuration
#   Permission Error: Cannot download extension from "..." - external access is disabled by configuration
#   Invalid Input Error: Cannot change configuration option X - the configuration has been locked
#   flAPI: ... refused in a template: external access is restricted (duckdb-hardening ...)
REFUSAL = re.compile(r"Permission Error|PermissionException|disabled by configuration|"
                     r"disabled through configuration|configuration has been locked|"
                     r"external access is restricted", re.IGNORECASE)

POLICY_LINE = re.compile(r"external access: (restricted|full) \(")

OWN_CSV = "id,name\n1,alpha\n2,beta\n"


# --------------------------------------------------------------------------- helpers

def make_honeypot():
    """An http server whose only job is to count who knocked. Its own `hits` list."""
    hits = []

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def _handle(self):
            hits.append(f"{self.command} {self.path}")
            body = b"honeypot\n"
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            if self.command != "HEAD":
                self.wfile.write(body)

        do_GET = do_HEAD = do_PUT = do_POST = _handle

    class Server(http.server.ThreadingHTTPServer):
        """Counts every accepted connection too: httpfs bumps an http:// extension
        repository to https://, so a download attempt arrives as a TLS handshake that
        never reaches do_GET."""

        def verify_request(self, request, client_address):
            hits.append(f"CONNECT from {client_address[0]}")
            return True

    server = Server(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return SimpleNamespace(server=server, hits=hits, port=server.server_address[1],
                           url=f"http://127.0.0.1:{server.server_address[1]}")


class RestrictedS3(FakeS3):
    """The module-scoped fixture's own fake: the function-scoped `bucket` fixture
    resets FakeS3's class attributes, which must not touch this record."""
    root = None
    requests_seen = []
    auth_seen = []


def start_server(config, cwd, env=None, extra=(), binary=None):
    """Start flapi; on failure the message starts with SERVER DID NOT START."""
    port = free_port()
    log_path = os.path.join(cwd, f"server-{port}.log")
    log = open(log_path, "w")
    proc = subprocess.Popen(
        [binary or flapi_binary(), *(["-c", config] if config else []), "-p", str(port),
         "--log-level", "info", *extra],
        cwd=cwd, stdout=log, stderr=subprocess.STDOUT, preexec_fn=os.setsid, env={**ENV, **(env or {})})

    def output():
        log.flush()
        with open(log_path) as f:
            return f.read()

    deadline = time.time() + 90
    while time.time() < deadline:
        if proc.poll() is not None:
            pytest.fail("SERVER DID NOT START (exited; red for the wrong reason, not a refused read):\n"
                        + output()[-4000:])
        try:
            if requests.get(f"http://127.0.0.1:{port}/health/live", timeout=1).status_code == 200:
                return SimpleNamespace(proc=proc, port=port, base=f"http://127.0.0.1:{port}", output=output)
        except requests.RequestException:
            time.sleep(0.3)
    os.killpg(proc.pid, signal.SIGKILL)
    pytest.fail("SERVER DID NOT START (never became live):\n" + output()[-4000:])


def get(s, path):
    """GET path; returns (response, log lines written while it ran)."""
    before = len(s.output())
    r = requests.get(f"{s.base}{path}", timeout=60)
    time.sleep(0.2)  # let the error line reach the log
    return r, s.output()[before:]


RECORD_START = re.compile(r"^\(\d{4}-\d{2}-\d{2}T", re.MULTILINE)


def log_records(text):
    """The log as records: a DuckDB error spans lines (`LINE 1: ...` and a caret line),
    and the request_id is appended to the record's LAST line only."""
    starts = [m.start() for m in RECORD_START.finditer(text)] or [0]
    return [text[a:b] for a, b in zip(starts, starts[1:] + [len(text)])]


def request_lines(s, request_id, timeout=5.0):
    """The log records carrying `request_id`, polled until one is a refusal or `timeout` passes."""
    deadline = time.time() + timeout
    while True:
        lines = [record for record in log_records(s.output()) if request_id in record]
        if any(REFUSAL.search(line) for line in lines) or time.time() >= deadline:
            return lines
        time.sleep(0.1)


def assert_refused(s, path, what):
    """The request failed AND its own log lines carry a policy refusal. Returns those lines."""
    r = requests.get(f"{s.base}{path}", timeout=60)
    detail = f"\nstatus={r.status_code} body={r.text[:500]}"
    assert r.status_code != 200, f"NOT REFUSED: {what} succeeded (200){detail}"
    try:
        request_id = r.json().get("request_id")
    except ValueError:
        request_id = None
    assert request_id, f"NOT REFUSED BY THE POLICY: {what} failed without a request_id to find it by{detail}"
    lines = request_lines(s, request_id)
    joined = "\n".join(lines)
    assert any(REFUSAL.search(line) for line in lines), (
        f"NOT REFUSED BY THE POLICY: {what} failed, but its log lines carry no permission/external-access "
        f"refusal{detail}\nlines for {request_id}:\n{joined[-3000:]}")
    return lines


def assert_allowed(s, path, what):
    r, log = get(s, path)
    assert r.status_code == 200, (
        f"ALLOWED OPERATION BROKEN: {what} -> {r.status_code} {r.text[:500]}\nlog:\n{log[-2000:]}")
    return r


def assert_policy_line_once(out, mode):
    found = POLICY_LINE.findall(out)
    assert found == [mode], (
        f"POLICY NOT LOGGED EXACTLY ONCE: expected one `external access: {mode} (...)` line, "
        f"got {found}\n" + out[-3000:])


def endpoint(url, sql_file, paginated=True, extra=""):
    yaml = f"url-path: {url}\nmethod: GET\ntemplate-source: {sql_file}\nconnection: [inmem]\n"
    if not paginated:
        # Statements (ATTACH, LOAD, INSTALL, SET) cannot be wrapped in the pagination subquery.
        yaml += "with-pagination: false\n"
    return yaml + extra


def hardening(mode=None, allowed_dirs=()):
    if mode is None and not allowed_dirs:
        return ""
    text = "duckdb-hardening:\n"
    if mode is not None:
        text += f"  external-access: {mode}\n"
    if allowed_dirs:
        text += "  allowed-directories:\n" + "".join(f"    - '{d}'\n" for d in allowed_dirs)
    return text


def local_config(block, ducklake=False, mcp=False):
    text = ("project-name: external-access\ntemplate:\n  path: ./sqls\n"
            "connections:\n  inmem:\n    properties:\n      database: ':memory:'\n"
            "rate_limit:\n  enabled: false\n")
    if ducklake:
        text += ("ducklake:\n  enabled: true\n  alias: cache\n  metadata-path: ./cache.ducklake\n"
                 "  data-path: ./cache_data\n  warmup: never\n")
    if mcp:
        text += "mcp:\n  enabled: true\n"
    return text + block


def write_own(root):
    """/own reads a CSV that lives inside the project (and inside template.path)."""
    put(root, "sqls/own.csv", OWN_CSV)
    put(root, "sqls/own.yaml", endpoint("/own", "own.sql",
                                        extra="mcp-tool:\n  name: get_own\n  description: own rows\n"))
    put(root, "sqls/own.sql", f"SELECT * FROM read_csv('{os.path.join(root, 'sqls', 'own.csv')}') ORDER BY id")


def write_passwd(root):
    put(root, "sqls/passwd.yaml", endpoint("/passwd", "passwd.sql"))
    put(root, "sqls/passwd.sql", "SELECT * FROM read_csv('/etc/passwd', header=false, sep=':')")


def rpc(base, method, params=None, session=None, req_id=1):
    headers = {"Content-Type": "application/json"}
    if session:
        headers["Mcp-Session-Id"] = session
    return requests.post(f"{base}/mcp/jsonrpc", timeout=60, headers=headers,
                         data=json.dumps({"jsonrpc": "2.0", "id": req_id, "method": method,
                                          "params": params or {}}))


# --------------------------------------------------------------------------- local, restricted

LOCKED_SETTINGS = {
    "lock_extaccess": "SET enable_external_access=true",
    "lock_alloweddirs": "SET allowed_directories=['/']",
    "lock_proxy": "SET http_proxy='http://127.0.0.1:1'",
}


@pytest.fixture(scope="module")
def restricted():
    """One local server with `external-access: restricted`, a honeypot and its own S3 fake."""
    root = tempfile.mkdtemp(prefix="flapi_extacc_")
    outside = tempfile.mkdtemp(prefix="flapi_extacc_outside_")
    secret_dir = os.path.join(outside, "secrets")
    os.makedirs(secret_dir)

    hp = make_honeypot()

    s3_root = tempfile.mkdtemp(prefix="flapi_extacc_s3_")
    RestrictedS3.root = s3_root
    put(s3_root, "otherbucket/x.csv", "secret\nfrom-otherbucket\n")
    fake = http.server.ThreadingHTTPServer(("127.0.0.1", 0), RestrictedS3)
    threading.Thread(target=fake.serve_forever, daemon=True).start()
    s3_endpoint = f"http://127.0.0.1:{fake.server_address[1]}"

    put(root, "flapi.yaml", local_config(hardening("restricted"), ducklake=True, mcp=True))
    write_own(root)
    write_passwd(root)
    put(root, "sqls/http.yaml", endpoint("/http", "http.sql"))
    put(root, "sqls/http.sql", f"SELECT * FROM read_text('{hp.url}/x')")
    put(root, "sqls/s3other.yaml", endpoint("/s3other", "s3other.sql"))
    put(root, "sqls/s3other.sql", "SELECT * FROM 's3://otherbucket/x.csv'")
    attach_target = os.path.join(outside, "evil.duckdb")
    put(root, "sqls/attach.yaml", endpoint("/attach", "attach.sql", paginated=False))
    put(root, "sqls/attach.sql", f"ATTACH '{attach_target}' AS evil")
    put(root, "sqls/load.yaml", endpoint("/load", "load.sql", paginated=False))
    # fts is installed in the default extension directory but flAPI never loads it.
    put(root, "sqls/load.sql", "LOAD fts")
    put(root, "sqls/install.yaml", endpoint("/install", "install.sql", paginated=False))
    put(root, "sqls/install.sql", f"FORCE INSTALL spatial FROM '{hp.url}/repo'")
    # The official repository is reachable even with external access off (DuckDB's
    # rule), so only flAPI can refuse this one. fts is already installed: no download.
    put(root, "sqls/installcore.yaml", endpoint("/installcore", "installcore.sql", paginated=False))
    put(root, "sqls/installcore.sql", "INSTALL fts FROM core")
    # The configuration lock.
    put(root, "sqls/secretdir.yaml", endpoint("/secretdir", "secretdir.sql", paginated=False))
    put(root, "sqls/secretdir.sql",
        f"SET secret_directory='{secret_dir}'; CREATE PERSISTENT SECRET s (TYPE s3, KEY_ID 'a', SECRET 'b')")
    for name, sql in LOCKED_SETTINGS.items():
        put(root, f"sqls/{name}.yaml", endpoint(f"/{name}", f"{name}.sql", paginated=False))
        put(root, f"sqls/{name}.sql", sql)
    # Writes outside the allowed directories.
    copy_target = os.path.join(outside, "x.csv")
    put(root, "sqls/copyout.yaml", endpoint("/copyout", "copyout.sql", paginated=False))
    put(root, "sqls/copyout.sql", f"COPY (SELECT 1 AS x) TO '{copy_target}'")
    # Scanner extensions loaded by default (sqlite, postgres) open files and sockets
    # themselves, past DuckDB's file system.
    sqlite_target = os.path.join(outside, "evil.sqlite")
    put(root, "sqls/sqliteattach.yaml", endpoint("/sqliteattach", "sqliteattach.sql", paginated=False))
    put(root, "sqls/sqliteattach.sql", f"ATTACH '{sqlite_target}' AS evil_sqlite (TYPE sqlite)")
    put(root, "sqls/pgattach.yaml", endpoint("/pgattach", "pgattach.sql", paginated=False))
    put(root, "sqls/pgattach.sql",
        f"ATTACH 'host=127.0.0.1 port={hp.port} dbname=x user=x connect_timeout=2' AS evil_pg (TYPE postgres)")
    put(root, "sqls/glob.yaml", endpoint("/glob", "glob.sql"))
    put(root, "sqls/glob.sql", "SELECT * FROM glob('/etc/*')")
    # A DuckLake cache populated from the project's own CSV.
    put(root, "sqls/cached.yaml", endpoint("/cached", "cached.sql") +
        "cache:\n  enabled: true\n  table: own_cache\n  schema: main\n  template-file: cached_populate.sql\n")
    put(root, "sqls/cached.sql",
        "SELECT * FROM {{{cache.catalog}}}.{{{cache.schema}}}.{{{cache.table}}} ORDER BY id")
    put(root, "sqls/cached_populate.sql",
        "CREATE OR REPLACE TABLE {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS "
        f"SELECT * FROM read_csv('{os.path.join(root, 'sqls', 'own.csv')}');\n")

    s = None
    try:
        s = start_server("flapi.yaml", root, env=s3_env(s3_endpoint),
                         extra=("--config-service", "--config-service-token", TOKEN))
        s.root, s.hp, s.attach_target = root, hp, attach_target
        s.secret_dir, s.copy_target, s.sqlite_target = secret_dir, copy_target, sqlite_target
        yield s
    finally:
        if s is not None:
            stop(s.proc)
        hp.server.shutdown()
        fake.shutdown()
        for d in (root, outside, s3_root):
            shutil.rmtree(d, ignore_errors=True)


class TestRestrictedLocal:
    def test_startup_logs_the_restricted_policy_exactly_once(self, restricted):
        assert_policy_line_once(restricted.output(), "restricted")

    def test_own_csv_is_served(self, restricted):
        r = assert_allowed(restricted, "/own", "reading the project's own CSV")
        assert [row["name"] for row in r.json()["data"]] == ["alpha", "beta"], r.text

    def test_reading_etc_passwd_is_refused(self, restricted):
        assert_refused(restricted, "/passwd", "read_csv('/etc/passwd')")

    def test_glob_outside_the_allowed_directories_is_refused(self, restricted):
        assert_refused(restricted, "/glob", "glob('/etc/*')")

    def test_reading_a_url_is_refused_and_nothing_is_fetched(self, restricted):
        before = len(restricted.hp.hits)
        assert_refused(restricted, "/http", "read_text('http://127.0.0.1:.../x')")
        assert restricted.hp.hits[before:] == [], f"NOT REFUSED: the URL was fetched: {restricted.hp.hits[before:]}"

    def test_reading_another_bucket_is_refused_and_nothing_is_fetched(self, restricted):
        before = len(RestrictedS3.requests_seen)
        assert_refused(restricted, "/s3other", "FROM 's3://otherbucket/x.csv'")
        seen = [x for x in RestrictedS3.requests_seen[before:] if "otherbucket" in x]
        assert seen == [], f"NOT REFUSED: otherbucket was requested: {seen}"

    def test_attaching_a_file_outside_the_allowed_directories_is_refused(self, restricted):
        assert_refused(restricted, "/attach", "ATTACH of a file outside the allowed directories")
        assert not os.path.exists(restricted.attach_target), (
            f"NOT REFUSED: ATTACH created {restricted.attach_target}")

    def test_copy_to_a_file_outside_the_allowed_directories_is_refused(self, restricted):
        assert_refused(restricted, "/copyout", "COPY ... TO a file outside the allowed directories")
        assert not os.path.exists(restricted.copy_target), f"NOT REFUSED: COPY wrote {restricted.copy_target}"

    def test_attaching_a_sqlite_file_outside_is_refused(self, restricted):
        assert_refused(restricted, "/sqliteattach", "ATTACH '<outside>/evil.sqlite' (TYPE sqlite)")
        assert not os.path.exists(restricted.sqlite_target), f"NOT REFUSED: created {restricted.sqlite_target}"

    def test_attaching_a_postgres_server_is_refused_and_nothing_connects(self, restricted):
        before = len(restricted.hp.hits)
        assert_refused(restricted, "/pgattach", "ATTACH 'host=127.0.0.1 ...' (TYPE postgres)")
        assert restricted.hp.hits[before:] == [], (
            f"NOT REFUSED: the postgres scanner connected out: {restricted.hp.hits[before:]}")

    def test_loading_an_extension_flapi_did_not_preload_is_refused(self, restricted):
        assert_refused(restricted, "/load", "LOAD fts")

    def test_load_is_refused_by_flapi_before_duckdb_runs_it(self, restricted):
        lines = "\n".join(assert_refused(restricted, "/load", "LOAD fts"))
        assert "external access" in lines and "duckdb-hardening" in lines, (
            "NOT REFUSED BY FLAPI: LOAD reached DuckDB instead of being refused by flAPI\n" + lines[-2000:])

    def test_install_from_core_is_refused_by_flapi(self, restricted):
        lines = "\n".join(assert_refused(restricted, "/installcore", "INSTALL fts FROM core"))
        assert "external access" in lines and "duckdb-hardening" in lines, (
            "NOT REFUSED BY FLAPI: INSTALL ... FROM core reached DuckDB (whose rule allows the official "
            "repository with external access off)\n" + lines[-2000:])

    def test_installing_from_a_foreign_repository_is_refused_and_nothing_is_fetched(self, restricted):
        before = len(restricted.hp.hits)
        assert_refused(restricted, "/install", "INSTALL spatial FROM <foreign repository>")
        assert restricted.hp.hits[before:] == [], (
            f"NOT REFUSED: the extension repository was contacted: {restricted.hp.hits[before:]}")

    def test_secret_directory_cannot_be_moved_to_write_a_secret_outside(self, restricted):
        assert_refused(restricted, "/secretdir", "SET secret_directory + CREATE PERSISTENT SECRET")
        assert os.listdir(restricted.secret_dir) == [], (
            f"NOT REFUSED: a secret was written outside: {os.listdir(restricted.secret_dir)}")

    @pytest.mark.parametrize("name", sorted(LOCKED_SETTINGS))
    def test_global_settings_are_locked(self, restricted, name):
        assert_refused(restricted, f"/{name}", LOCKED_SETTINGS[name])

    def test_cache_refresh_still_works_and_the_cache_answers(self, restricted):
        r = requests.post(f"{restricted.base}/api/v1/_config/endpoints/-cached/cache/refresh",
                          headers=AUTH, timeout=120)
        assert r.status_code == 200, (
            f"ALLOWED OPERATION BROKEN: cache refresh -> {r.status_code} {r.text[:500]}\n"
            + restricted.output()[-3000:])
        got = assert_allowed(restricted, "/cached", "reading the DuckLake cache")
        assert [row["name"] for row in got.json()["data"]] == ["alpha", "beta"], got.text

    def test_cache_gc_and_compact_still_work(self, restricted):
        r = requests.post(f"{restricted.base}/api/v1/_config/endpoints/-cached/cache/refresh",
                          headers=AUTH, timeout=120)
        assert r.status_code == 200, r.text[:500]
        r = requests.post(f"{restricted.base}/api/v1/_config/endpoints/-cached/cache/gc",
                          headers=AUTH, timeout=120)
        assert r.status_code == 200, (
            f"ALLOWED OPERATION BROKEN: cache gc -> {r.status_code} {r.text[:500]}\n" + restricted.output()[-3000:])
        r = requests.post(f"{restricted.base}/api/v1/_config/cache/compact", headers=AUTH, json={}, timeout=120)
        assert r.status_code == 200, (
            f"ALLOWED OPERATION BROKEN: cache compact -> {r.status_code} {r.text[:500]}\n"
            + restricted.output()[-3000:])
        got = assert_allowed(restricted, "/cached", "reading the DuckLake cache after gc/compact")
        assert [row["name"] for row in got.json()["data"]] == ["alpha", "beta"], got.text

    def test_mcp_tool_call_still_works(self, restricted):
        init = rpc(restricted.base, "initialize", {"protocolVersion": "2025-03-26", "capabilities": {},
                                                    "clientInfo": {"name": "t", "version": "1"}})
        assert init.status_code == 200, init.text
        session = init.headers.get("Mcp-Session-Id")
        r = rpc(restricted.base, "tools/call", {"name": "get_own", "arguments": {}}, session=session, req_id=2)
        assert r.status_code == 200, f"ALLOWED OPERATION BROKEN: MCP tools/call -> {r.status_code} {r.text}"
        body = r.json()
        assert "error" not in body and not body.get("result", {}).get("isError"), (
            f"ALLOWED OPERATION BROKEN: MCP tools/call errored: {r.text[:1000]}")
        assert "alpha" in r.text and "beta" in r.text, r.text


# --------------------------------------------------------------------------- local, no block (no regression)

class TestLocalDefaultIsFull:
    def test_local_config_without_the_block_can_still_read_any_file(self, workdir):
        put(workdir, "flapi.yaml", local_config(""))
        write_passwd(workdir)
        s = start_server("flapi.yaml", workdir)
        try:
            r, log = get(s, "/passwd")
            assert r.status_code == 200, (
                f"REGRESSION: a local config without `duckdb-hardening` lost file access: "
                f"{r.status_code} {r.text[:500]}\n{log[-2000:]}")
            assert any("root" in str(row) for row in r.json()["data"]), r.text[:500]
        finally:
            stop(s.proc)

    def test_startup_logs_full_for_a_local_config(self, workdir):
        put(workdir, "flapi.yaml", local_config(""))
        write_own(workdir)
        s = start_server("flapi.yaml", workdir)
        try:
            assert_policy_line_once(s.output(), "full")
        finally:
            stop(s.proc)


# --------------------------------------------------------------------------- user allowed-directories

class TestAllowedDirectories:
    def test_an_extra_allowed_directory_is_readable_and_its_neighbour_is_not(self, workdir):
        extra = tempfile.mkdtemp(prefix="flapi_extacc_allowed_")
        other = tempfile.mkdtemp(prefix="flapi_extacc_denied_")
        try:
            put(extra, "in.csv", "v\nallowed\n")
            put(other, "out.csv", "v\ndenied\n")
            put(workdir, "flapi.yaml", local_config(hardening("restricted", [extra.rstrip("/") + "/"])))
            put(workdir, "sqls/in.yaml", endpoint("/in", "in.sql"))
            put(workdir, "sqls/in.sql", f"SELECT * FROM read_csv('{os.path.join(extra, 'in.csv')}')")
            put(workdir, "sqls/out.yaml", endpoint("/out", "out.sql"))
            put(workdir, "sqls/out.sql", f"SELECT * FROM read_csv('{os.path.join(other, 'out.csv')}')")
            s = start_server("flapi.yaml", workdir)
            try:
                r = assert_allowed(s, "/in", "a read inside allowed-directories")
                assert r.json()["data"] == [{"v": "allowed"}], r.text
                assert_refused(s, "/out", "a read outside allowed-directories")
            finally:
                stop(s.proc)
        finally:
            shutil.rmtree(extra, ignore_errors=True)
            shutil.rmtree(other, ignore_errors=True)


# --------------------------------------------------------------------------- remote config on S3

ESCAPES = {
    "traversal": "s3://mybucket/../otherbucket/x.csv",
    "traversal_encoded": "s3://mybucket/%2e%2e/otherbucket/x.csv",
    "prefix_boundary": "s3://mybucket-evil/x.csv",
}


def remote_bucket(root, block, template_path="./sqls", template_bucket="mybucket/sqls"):
    """s3://mybucket/flapi.yaml with templates under s3://mybucket/sqls and data at the bucket root."""
    put(root, "mybucket/flapi.yaml",
        f"project-name: remote-external-access\ntemplate:\n  path: {template_path}\n"
        "connections:\n  inmem:\n    properties:\n      database: ':memory:'\n"
        "rate_limit:\n  enabled: false\n" + block)
    put(root, "mybucket/data.csv", "id,origin\n1,mybucket\n")
    put(root, f"{template_bucket}/data.yaml", endpoint("/data", "data.sql"))
    put(root, f"{template_bucket}/data.sql", "SELECT * FROM read_csv('s3://mybucket/data.csv')")
    put(root, f"{template_bucket}/passwd.yaml", endpoint("/passwd", "passwd.sql"))
    put(root, f"{template_bucket}/passwd.sql", "SELECT * FROM read_csv('/etc/passwd', header=false, sep=':')")
    put(root, f"{template_bucket}/other.yaml", endpoint("/other", "other.sql"))
    put(root, f"{template_bucket}/other.sql", "SELECT * FROM read_csv('s3://otherbucket/x.csv')")
    for name, url in ESCAPES.items():
        put(root, f"{template_bucket}/{name}.yaml", endpoint(f"/{name}", f"{name}.sql"))
        put(root, f"{template_bucket}/{name}.sql", f"SELECT * FROM read_csv('{url}')")
    put(root, "otherbucket/x.csv", "secret\nfrom-otherbucket\n")
    put(root, "mybucket-evil/x.csv", "secret\nfrom-mybucket-evil\n")


class TestRemoteConfig:
    def test_remote_config_defaults_to_restricted(self, bucket, workdir):
        root, s3_endpoint = bucket
        remote_bucket(root, "")
        s = start_server("s3://mybucket/flapi.yaml", workdir, env=s3_env(s3_endpoint))
        try:
            r = assert_allowed(s, "/data", "reading s3://mybucket/data.csv from the config's own bucket")
            assert r.json()["data"] == [{"id": 1, "origin": "mybucket"}], r.text
            assert_refused(s, "/passwd", "read_csv('/etc/passwd') under a remote config")
            before = len(FakeS3.requests_seen)
            assert_refused(s, "/other", "read_csv('s3://otherbucket/x.csv') under a remote config")
            seen = [x for x in FakeS3.requests_seen[before:] if "otherbucket" in x]
            assert seen == [], f"NOT REFUSED: otherbucket was requested: {seen}"
            assert_policy_line_once(s.output(), "restricted")
        finally:
            stop(s.proc)

    def test_traversal_and_prefix_boundary_escapes_are_refused(self, bucket, workdir):
        root, s3_endpoint = bucket
        remote_bucket(root, "")
        s = start_server("s3://mybucket/flapi.yaml", workdir, env=s3_env(s3_endpoint))
        try:
            failures = []
            for name, url in ESCAPES.items():
                before = len(FakeS3.requests_seen)
                try:
                    assert_refused(s, f"/{name}", f"read_csv('{url}')")
                except AssertionError as e:
                    failures.append(str(e)[:800])
                seen = [x for x in FakeS3.requests_seen[before:]
                        if "otherbucket" in x or "mybucket-evil" in x or ".." in x or "%2e" in x.lower()]
                if seen:
                    failures.append(f"NOT REFUSED: {url} reached the bucket: {seen}")
            assert not failures, "\n\n".join(failures)
        finally:
            stop(s.proc)

    def test_create_secret_cannot_redirect_the_allowed_bucket(self, bucket, workdir):
        """A temporary secret is instance-wide: one created by a template would redirect
        every endpoint's s3://mybucket reads to an endpoint of the author's choosing."""
        root, s3_endpoint = bucket
        remote_bucket(root, "")
        hp = make_honeypot()
        put(root, "mybucket/sqls/redirect.yaml", endpoint("/redirect", "redirect.sql", paginated=False))
        put(root, "mybucket/sqls/redirect.sql",
            "CREATE SECRET redirect (TYPE s3, KEY_ID 'k', SECRET 's', REGION 'eu-central-1', "
            f"ENDPOINT '127.0.0.1:{hp.port}', URL_STYLE path, USE_SSL false, SCOPE 's3://mybucket/'); "
            "SELECT * FROM read_csv('s3://mybucket/data.csv')")
        s = start_server("s3://mybucket/flapi.yaml", workdir, env=s3_env(s3_endpoint))
        try:
            redirect_error = None
            try:
                assert_refused(s, "/redirect", "CREATE SECRET (ENDPOINT <honeypot>) in a template")
            except AssertionError as e:
                redirect_error = str(e)
            # Whatever the first request did, the next read of the allowed bucket must
            # still go to the real one.
            requests.get(f"{s.base}/data", timeout=60)
            assert redirect_error is None and hp.hits == [], (
                f"NOT REFUSED: the secret redirect {'reached the honeypot: ' + str(hp.hits) if hp.hits else ''}"
                f"\n{redirect_error or ''}")
        finally:
            stop(s.proc)
            hp.server.shutdown()

    def test_reload_of_a_remote_endpoint_stays_restricted(self, bucket, workdir):
        """A remote template.path is read-only through the config service: reload answers 501
        and the endpoint keeps its template. Whichever it does, a template edited in the
        bucket to read /etc/passwd must never be served."""
        root, s3_endpoint = bucket
        remote_bucket(root, "")
        s = start_server("s3://mybucket/flapi.yaml", workdir, env=s3_env(s3_endpoint),
                         extra=("--config-service", "--config-service-token", TOKEN))
        try:
            assert_allowed(s, "/data", "the first read")
            put(root, "mybucket/sqls/data.sql", "SELECT * FROM read_csv('/etc/passwd', header=false, sep=':')")
            r = requests.post(f"{s.base}/api/v1/_config/endpoints/-data/reload", headers=AUTH, timeout=60)
            assert r.status_code in (200, 501), f"reload -> {r.status_code} {r.text[:500]}"
            got = requests.get(f"{s.base}/data", timeout=60)
            if got.status_code == 200:
                assert got.json()["data"] == [{"id": 1, "origin": "mybucket"}], (
                    f"NOT REFUSED: the edited template was served after reload ({r.status_code}): {got.text[:500]}")
            else:
                assert_refused(s, "/data", "a template edited to read /etc/passwd, after a reload")
        finally:
            stop(s.proc)

    def test_template_path_in_another_bucket(self, bucket, workdir):
        root, s3_endpoint = bucket
        remote_bucket(root, "", template_path="s3://tplbucket/sqls", template_bucket="tplbucket/sqls")
        s = start_server("s3://mybucket/flapi.yaml", workdir, env=s3_env(s3_endpoint))
        try:
            r = assert_allowed(s, "/data", "a template from s3://tplbucket reading s3://mybucket/data.csv")
            assert r.json()["data"] == [{"id": 1, "origin": "mybucket"}], r.text
            assert_refused(s, "/passwd", "read_csv('/etc/passwd') with templates in another bucket")
        finally:
            stop(s.proc)

    def test_remote_config_with_full_reads_local_files_again(self, bucket, workdir):
        root, s3_endpoint = bucket
        remote_bucket(root, hardening("full"))
        s = start_server("s3://mybucket/flapi.yaml", workdir, env=s3_env(s3_endpoint))
        try:
            r, log = get(s, "/passwd")
            assert r.status_code == 200, (
                f"`external-access: full` should allow /etc/passwd: {r.status_code} {r.text[:500]}\n{log[-2000:]}")
        finally:
            stop(s.proc)

    def test_remote_config_with_full_logs_full(self, bucket, workdir):
        root, s3_endpoint = bucket
        remote_bucket(root, hardening("full"))
        s = start_server("s3://mybucket/flapi.yaml", workdir, env=s3_env(s3_endpoint))
        try:
            assert_policy_line_once(s.output(), "full")
        finally:
            stop(s.proc)


# --------------------------------------------------------------------------- packed bundle

class TestPackedBundle:
    def test_a_restricted_bundle_reads_its_embedded_data(self, workdir):
        fixture = os.path.join(workdir, "fixture")
        put(fixture, "flapi.yaml",
            "project-name: bundled-restricted\ntemplate:\n  path: ./sqls\nconnections: {}\n"
            "rate_limit:\n  enabled: false\n" + hardening("restricted"))
        put(fixture, "data/x.csv", "city\nBerlin\nVienna\n")
        put(fixture, "sqls/x.yaml", "url-path: /x\nmethod: GET\ntemplate-source: x.sql\n")
        put(fixture, "sqls/x.sql", "SELECT * FROM read_csv('embed://data/x.csv', header=true) ORDER BY city")
        put(fixture, "sqls/passwd.yaml", "url-path: /passwd\nmethod: GET\ntemplate-source: passwd.sql\n")
        put(fixture, "sqls/passwd.sql", "SELECT * FROM read_csv('/etc/passwd', header=false, sep=':')")
        bundled = os.path.join(workdir, "flapi-bundled")
        packed = subprocess.run([flapi_binary(), "pack", "--in", fixture, "--out", bundled],
                                capture_output=True, text=True, timeout=120, env=ENV)
        assert packed.returncode == 0, packed.stdout + packed.stderr
        shutil.rmtree(fixture)
        clean = os.path.join(workdir, "clean")
        os.makedirs(clean)
        s = start_server(None, clean, binary=bundled)
        try:
            assert_policy_line_once(s.output(), "restricted")
            r = assert_allowed(s, "/x", "read_csv('embed://data/x.csv') in a restricted bundle")
            assert [row["city"] for row in r.json()["data"]] == ["Berlin", "Vienna"], r.text
            assert_refused(s, "/passwd", "read_csv('/etc/passwd') in a restricted bundle")
        finally:
            stop(s.proc)
