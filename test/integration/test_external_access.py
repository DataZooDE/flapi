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
remote base prefix, the DuckLake data-path, the extension directory, `embed://`
and the user's list) and then `enable_external_access=false`. The policy is
logged once at startup: `external access: restricted (...)` / `external access: full`.

Every failure message says which of two things went wrong: "SERVER DID NOT START"
(the config was refused - a red for the wrong reason) or "NOT REFUSED" (the server
ran, the read went through).
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

# What DuckDB says when enable_external_access=false refuses something:
#   Permission Error: Cannot access file "/etc/passwd" - file system operations are disabled by configuration
#   Permission Error: Loading external extensions is disabled through configuration
#   Permission Error: Cannot download extension from "..." - external access is disabled by configuration
REFUSAL = re.compile(r"Permission Error|PermissionException|disabled by configuration|"
                     r"disabled through configuration|external access", re.IGNORECASE)

OWN_CSV = "id,name\n1,alpha\n2,beta\n"


# --------------------------------------------------------------------------- helpers

class Honeypot(http.server.BaseHTTPRequestHandler):
    """An http server whose only job is to count who knocked."""
    hits = []

    def log_message(self, *args):
        pass

    def _handle(self):
        Honeypot.hits.append(f"{self.command} {self.path}")
        body = b"honeypot\n"
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    do_GET = do_HEAD = _handle


class HoneypotServer(http.server.ThreadingHTTPServer):
    """Counts every accepted connection too: httpfs bumps an http:// extension
    repository to https://, so a download attempt arrives as a TLS handshake that
    never reaches do_GET."""

    def verify_request(self, request, client_address):
        Honeypot.hits.append(f"CONNECT from {client_address[0]}")
        return True


def start_server(config, cwd, env=None, extra=()):
    """Start flapi; on failure the message starts with SERVER DID NOT START."""
    port = free_port()
    log_path = os.path.join(cwd, f"server-{port}.log")
    log = open(log_path, "w")
    proc = subprocess.Popen(
        [flapi_binary(), "-c", config, "-p", str(port), "--log-level", "info", *extra],
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


def assert_refused(s, path, what):
    r, log = get(s, path)
    detail = f"\nstatus={r.status_code} body={r.text[:500]}\nlog:\n{log[-2000:]}"
    assert r.status_code != 200, f"NOT REFUSED: {what} succeeded (200){detail}"
    assert REFUSAL.search(r.text) or REFUSAL.search(log), (
        f"NOT REFUSED BY THE POLICY: {what} failed, but not with a permission/external-access error{detail}")


def assert_allowed(s, path, what):
    r, log = get(s, path)
    assert r.status_code == 200, (
        f"ALLOWED OPERATION BROKEN: {what} -> {r.status_code} {r.text[:500]}\nlog:\n{log[-2000:]}")
    return r


def endpoint(url, sql_file, paginated=True, extra=""):
    yaml = f"url-path: {url}\nmethod: GET\ntemplate-source: {sql_file}\nconnection: [inmem]\n"
    if not paginated:
        # Statements (ATTACH, LOAD, INSTALL) cannot be wrapped in the pagination subquery.
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

@pytest.fixture(scope="module")
def restricted():
    """One local server with `external-access: restricted`, the honeypot and an S3 fake."""
    root = tempfile.mkdtemp(prefix="flapi_extacc_")
    outside = tempfile.mkdtemp(prefix="flapi_extacc_outside_")

    Honeypot.hits = []
    hp = HoneypotServer(("127.0.0.1", 0), Honeypot)
    threading.Thread(target=hp.serve_forever, daemon=True).start()
    hp_url = f"http://127.0.0.1:{hp.server_address[1]}"

    s3_root = tempfile.mkdtemp(prefix="flapi_extacc_s3_")
    FakeS3.root = s3_root
    FakeS3.requests_seen = []
    FakeS3.auth_seen = []
    put(s3_root, "otherbucket/x.csv", "secret\nfrom-otherbucket\n")
    fake = http.server.ThreadingHTTPServer(("127.0.0.1", 0), FakeS3)
    threading.Thread(target=fake.serve_forever, daemon=True).start()
    s3_endpoint = f"http://127.0.0.1:{fake.server_address[1]}"

    put(root, "flapi.yaml", local_config(hardening("restricted"), ducklake=True, mcp=True))
    write_own(root)
    write_passwd(root)
    put(root, "sqls/http.yaml", endpoint("/http", "http.sql"))
    put(root, "sqls/http.sql", f"SELECT * FROM read_text('{hp_url}/x')")
    put(root, "sqls/s3other.yaml", endpoint("/s3other", "s3other.sql"))
    put(root, "sqls/s3other.sql", "SELECT * FROM 's3://otherbucket/x.csv'")
    attach_target = os.path.join(outside, "evil.duckdb")
    put(root, "sqls/attach.yaml", endpoint("/attach", "attach.sql", paginated=False))
    put(root, "sqls/attach.sql", f"ATTACH '{attach_target}' AS evil")
    put(root, "sqls/load.yaml", endpoint("/load", "load.sql", paginated=False))
    # fts is installed in the default extension directory but flAPI never loads it.
    put(root, "sqls/load.sql", "LOAD fts")
    put(root, "sqls/install.yaml", endpoint("/install", "install.sql", paginated=False))
    put(root, "sqls/install.sql", f"FORCE INSTALL spatial FROM '{hp_url}/repo'")
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
        s.root, s.hp_url, s.attach_target = root, hp_url, attach_target
        yield s
    finally:
        if s is not None:
            stop(s.proc)
        hp.shutdown()
        fake.shutdown()
        for d in (root, outside, s3_root):
            shutil.rmtree(d, ignore_errors=True)


class TestRestrictedLocal:
    def test_startup_logs_the_restricted_policy(self, restricted):
        out = restricted.output()
        assert "external access: restricted" in out, (
            "POLICY NOT APPLIED: no `external access: restricted` line at startup\n"
            + out[-3000:])

    def test_own_csv_is_served(self, restricted):
        r = assert_allowed(restricted, "/own", "reading the project's own CSV")
        assert [row["name"] for row in r.json()["data"]] == ["alpha", "beta"], r.text

    def test_reading_etc_passwd_is_refused(self, restricted):
        assert_refused(restricted, "/passwd", "read_csv('/etc/passwd')")

    def test_reading_a_url_is_refused_and_nothing_is_fetched(self, restricted):
        before = len(Honeypot.hits)
        assert_refused(restricted, "/http", "read_text('http://127.0.0.1:.../x')")
        assert Honeypot.hits[before:] == [], f"NOT REFUSED: the URL was fetched: {Honeypot.hits[before:]}"

    def test_reading_another_bucket_is_refused_and_nothing_is_fetched(self, restricted):
        before = len(FakeS3.requests_seen)
        assert_refused(restricted, "/s3other", "FROM 's3://otherbucket/x.csv'")
        seen = [x for x in FakeS3.requests_seen[before:] if "otherbucket" in x]
        assert seen == [], f"NOT REFUSED: otherbucket was requested: {seen}"

    def test_attaching_a_file_outside_the_allowed_directories_is_refused(self, restricted):
        assert_refused(restricted, "/attach", "ATTACH of a file outside the allowed directories")
        assert not os.path.exists(restricted.attach_target), (
            f"NOT REFUSED: ATTACH created {restricted.attach_target}")

    def test_loading_an_extension_flapi_did_not_preload_is_refused(self, restricted):
        assert_refused(restricted, "/load", "LOAD fts")

    def test_installing_from_a_foreign_repository_is_refused_and_nothing_is_fetched(self, restricted):
        before = len(Honeypot.hits)
        assert_refused(restricted, "/install", "INSTALL spatial FROM <foreign repository>")
        assert Honeypot.hits[before:] == [], (
            f"NOT REFUSED: the extension repository was contacted: {Honeypot.hits[before:]}")

    def test_cache_refresh_still_works_and_the_cache_answers(self, restricted):
        r = requests.post(f"{restricted.base}/api/v1/_config/endpoints/-cached/cache/refresh",
                          headers=AUTH, timeout=120)
        assert r.status_code == 200, (
            f"ALLOWED OPERATION BROKEN: cache refresh -> {r.status_code} {r.text[:500]}\n"
            + restricted.output()[-3000:])
        got = assert_allowed(restricted, "/cached", "reading the DuckLake cache")
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
            out = s.output()
            assert "external access: full" in out, "no `external access: full` line at startup\n" + out[-3000:]
            assert "external access: restricted" not in out, out[-3000:]
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

def remote_bucket(root, block):
    """s3://mybucket/flapi.yaml with templates under s3://mybucket/sqls and data at the bucket root."""
    put(root, "mybucket/flapi.yaml",
        "project-name: remote-external-access\ntemplate:\n  path: ./sqls\n"
        "connections:\n  inmem:\n    properties:\n      database: ':memory:'\n"
        "rate_limit:\n  enabled: false\n" + block)
    put(root, "mybucket/data.csv", "id,origin\n1,mybucket\n")
    put(root, "mybucket/sqls/data.yaml", endpoint("/data", "data.sql"))
    put(root, "mybucket/sqls/data.sql", "SELECT * FROM read_csv('s3://mybucket/data.csv')")
    put(root, "mybucket/sqls/passwd.yaml", endpoint("/passwd", "passwd.sql"))
    put(root, "mybucket/sqls/passwd.sql", "SELECT * FROM read_csv('/etc/passwd', header=false, sep=':')")
    put(root, "mybucket/sqls/other.yaml", endpoint("/other", "other.sql"))
    put(root, "mybucket/sqls/other.sql", "SELECT * FROM read_csv('s3://otherbucket/x.csv')")
    put(root, "otherbucket/x.csv", "secret\nfrom-otherbucket\n")


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
            out = s.output()
            assert "external access: restricted" in out, (
                "a remote config did not log `external access: restricted` at startup\n" + out[-3000:])
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
            out = s.output()
            assert "external access: full" in out, (
                "`external-access: full` was not logged for a remote config\n" + out[-3000:])
            assert "external access: restricted" not in out, out[-3000:]
        finally:
            stop(s.proc)
