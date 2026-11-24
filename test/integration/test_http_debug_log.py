"""Opt-in log of every HTTP request DuckDB makes (#252).

When a template or a remote config read goes to S3/GCS/Azure/HTTP, an operator
could not see which object store requests a flAPI request caused. The switch
(`log-http-requests: true` at the top level of flapi.yaml, or
`FLAPI_LOG_HTTP_REQUESTS=1`) writes one line per DuckDB HTTP request into the
normal flAPI log:

    duckdb http <METHOD> <url> -> <status> <N>ms request_id=<id>

`<id>` is the flAPI request id (the `X-Request-Id` response header) of the
request that caused it, or `-` for requests made outside any HTTP request
(startup, the remote config read). Headers are never printed and credentials
in URLs are redacted. With the switch off DuckDB logging is never enabled.

The fake S3 (from test_remote_templates_s3) records every request it serves, so
the number of log lines for a request can be checked against the number of
requests the store actually saw while that request was being served.
"""
import concurrent.futures
import json
import os
import re
import subprocess
import sys
import time

import pytest
import requests

from otel_helpers import flapi_binary, free_port
from test_remote_templates import ENV, put, stop, workdir  # noqa: F401
from test_remote_templates_s3 import FakeS3, bucket, s3_env  # noqa: F401

pytestmark = [pytest.mark.standalone_server,
              pytest.mark.skipif(sys.platform == "win32", reason="spawns POSIX process groups (os.setsid / killpg)")]

N_OBJECTS = 8

# One endpoint whose YAML and SQL live in the bucket; the SQL reads one of eight
# CSV objects from the same bucket, chosen by ?n=, so URL -> request is known.
OBJ_ENDPOINT = """\
url-path: /obj
method: GET
with-pagination: false
request:
  - field-name: n
    field-in: query
    required: true
    validators:
      - type: int
        min: 0
        max: 7
template-source: obj.sql
connection: [inmem]
"""
OBJ_SQL = "SELECT n, tag FROM read_csv('s3://mybucket/data/obj{{{ params.n }}}.csv')"

# Extra endpoints, each a plain GET with no parameters. `{fake}` is the fake
# store's base URL, `{dead}` a port nothing listens on.
EXTRA_ENDPOINTS = {
    # All eight objects plus a multi-MB one in one scan: with `duckdb.threads: 4`
    # the reads run on DuckDB worker threads, not on the request's thread.
    "many": ("SELECT count(*) AS rows FROM read_csv(["
             + ", ".join(f"'s3://mybucket/data/obj{i}.csv'" for i in range(8))
             + ", 's3://mybucket/big/big.csv'], union_by_name = true)"),
    # A presigned-style URL: credential and signature in the query string.
    "presigned": ("SELECT tag FROM read_csv('{fake}/mybucket/data/obj0.csv"
                  "?X-Amz-Credential=AKIATESTKEY%2F20261124&X-Amz-Signature=SECRETSIG')"),
    # Credentials in the authority.
    "userinfo": "SELECT tag FROM read_csv('{fake_userinfo}/mybucket/data/obj0.csv')",
    # A missing object: the store answers 404.
    "missing": "SELECT * FROM read_csv('s3://mybucket/data/nope.csv')",
    # Nothing listens: no response at all.
    "dead": "SELECT * FROM read_csv('http://127.0.0.1:{dead}/x.csv')",
}
PASSWORD = "PWSECRET99"

# The message itself; the request id is stamped by the log handler, once.
MSG_RE = re.compile(
    r"duckdb http (?P<method>[A-Z]+) (?P<url>\S+) -> (?P<status>\d{3}|ERR) (?P<ms>\d+)ms")
RID_RE = re.compile(r"request_id=(?P<rid>[^\s\"]+)")

SECRETS = ("AKIATESTKEY", "Signature=", "SECRETSIG", "Authorization", PASSWORD)


class Parsed:
    """MSG_RE's groups plus `rid`, the request id the line was logged with."""

    def __init__(self, m, rid):
        self._groups = {**m.groupdict(), "rid": rid}

    def group(self, name):
        return self._groups[name]


def _config(log_http_requests, log_format, threads=None, init_extra=""):
    key = "" if log_http_requests is None else f"log-http-requests: {'true' if log_http_requests else 'false'}\n"
    duckdb = "" if threads is None else f"duckdb:\n  threads: {threads}\n"
    return (
        "project-name: http-debug-log\n"
        f"log-format: {log_format}\n"
        f"{key}{duckdb}"
        "template:\n  path: s3://mybucket/sqls\n"
        "connections:\n  inmem:\n    init: |\n      INSTALL httpfs;\n      LOAD httpfs;\n"
        f"{init_extra}"
        "    properties:\n      database: ':memory:'\n"
        "rate_limit:\n  enabled: false\n")


def _fill_bucket(root, endpoint):
    put(root, "mybucket/sqls/obj.yaml", OBJ_ENDPOINT)
    put(root, "mybucket/sqls/obj.sql", OBJ_SQL)
    for i in range(N_OBJECTS):
        put(root, f"mybucket/data/obj{i}.csv", f"n,tag\n{i},obj{i}\n")
    # ~6 MB, so the scan is more than one small GET.
    rows = "".join(f"{i},big{i:012d}\n" for i in range(300_000))
    put(root, "mybucket/big/big.csv", "n,tag\n" + rows)
    host = endpoint.split("://", 1)[1]
    for name, sql in EXTRA_ENDPOINTS.items():
        put(root, f"mybucket/sqls/{name}.yaml",
            f"url-path: /{name}\nmethod: GET\nwith-pagination: false\n"
            f"template-source: {name}.sql\nconnection: [inmem]\n")
        put(root, f"mybucket/sqls/{name}.sql",
            sql.format(fake=endpoint, fake_userinfo=f"http://flapiuser:{PASSWORD}@{host}", dead=_DEAD_PORT))


_DEAD_PORT = free_port()  # bound and released: nothing listens there


def _env(endpoint, env_switch):
    env = {**ENV, **s3_env(endpoint)}
    env.pop("FLAPI_LOG_HTTP_REQUESTS", None)
    if env_switch is True:
        env["FLAPI_LOG_HTTP_REQUESTS"] = "1"
    elif isinstance(env_switch, str):
        env["FLAPI_LOG_HTTP_REQUESTS"] = env_switch
    return env


def _serve(bucket_fixture, workdir, *, log_http_requests=None, env_switch=False, log_format="text",
           threads=None, init_extra="", config_arg=None):
    """Fill the bucket, boot a server against it; returns (proc, port, read_log).

    `env_switch`: True sets FLAPI_LOG_HTTP_REQUESTS=1, a string sets it to that.
    `config_arg`: what to pass to -c (default: the local flapi.yaml)."""
    root, endpoint = bucket_fixture
    _fill_bucket(root, endpoint)
    put(workdir, "flapi.yaml", _config(log_http_requests, log_format, threads, init_extra))
    env = _env(endpoint, env_switch)

    port = free_port()
    log_path = os.path.join(workdir, f"server-{port}.log")
    log = open(log_path, "w")
    proc = subprocess.Popen(
        [flapi_binary(), "-c", config_arg or os.path.join(workdir, "flapi.yaml"), "-p", str(port),
         "--log-level", "info"],
        cwd=workdir, stdout=log, stderr=subprocess.STDOUT, preexec_fn=os.setsid, env=env)

    def read_log():
        log.flush()
        with open(log_path, errors="replace") as f:
            return f.read()

    deadline = time.time() + 90
    while time.time() < deadline:
        if proc.poll() is not None:
            pytest.fail("server exited during startup:\n" + read_log()[-4000:])
        try:
            if requests.get(f"http://127.0.0.1:{port}/health/live", timeout=1).status_code == 200:
                return proc, port, read_log
        except requests.RequestException:
            time.sleep(0.3)
    stop(proc)
    pytest.fail("server never became live:\n" + read_log()[-4000:])


def _http_lines(text):
    """Every `duckdb http` line as (raw, Parsed or None).

    Text: the id is the `request_id=` the handler appends, which must occur
    exactly once on the line. JSON: the id is the record's own `request_id`
    field, and the message must not carry one."""
    out = []
    for raw in text.splitlines():
        if "duckdb http" not in raw:
            continue
        if raw.lstrip().startswith("{"):
            try:
                rec = json.loads(raw)
            except json.JSONDecodeError:
                continue  # still being written
            msg = rec.get("message", "")
            m = MSG_RE.search(msg)
            ok = m and "request_id=" not in msg and "request_id" in rec
            out.append((raw, Parsed(m, rec["request_id"]) if ok else None))
            continue
        m = MSG_RE.search(raw)
        ids = RID_RE.findall(raw)
        out.append((raw, Parsed(m, ids[0]) if m and len(ids) == 1 else None))
    return out


def _rows(r):
    body = r.json()
    return body["data"] if isinstance(body, dict) else body


def _get(port, path, params=None, timeout=60):
    """GET path; returns (response, request_id, fake requests seen while it ran)."""
    before = len(FakeS3.requests_seen)
    r = requests.get(f"http://127.0.0.1:{port}{path}", params=params, timeout=timeout)
    after = len(FakeS3.requests_seen)
    rid = r.headers.get("X-Request-Id")
    assert rid, f"the response carries no X-Request-Id: {r.status_code} {r.text}"
    return r, rid, FakeS3.requests_seen[before:after]


def _call(port, n):
    """GET /obj?n=; returns (request_id, fake requests seen while it ran)."""
    r, rid, seen = _get(port, "/obj", {"n": n}, timeout=30)
    assert r.status_code == 200, r.text
    assert _rows(r)[0]["tag"] == f"obj{n}", r.text
    return rid, seen


def _lines_for(read_log, rid, expected, timeout=5.0):
    """Poll until `expected` lines for `rid` are in the log (a logger may write
    late), then wait a little more and read again, so a duplicate shows up."""
    def current():
        return [(raw, m) for raw, m in _http_lines(read_log()) if m and m.group("rid") == rid]
    deadline = time.time() + timeout
    while True:
        lines = current()
        if len(lines) >= expected or time.time() > deadline:
            break
        time.sleep(0.1)
    time.sleep(0.3)
    return current()


def _assert_no_secrets(lines):
    for raw, _ in lines:
        for s in SECRETS:
            assert s not in raw, f"a duckdb http line leaks {s!r}: {raw}"


def test_switch_on_logs_each_request_with_its_request_id(bucket, workdir):
    proc, port, read_log = _serve(bucket, workdir, log_http_requests=True)
    try:
        rid, seen = _call(port, 3)
        assert seen, "scenario broken: the call made no request to the fake S3"
        lines = _lines_for(read_log, rid, len(seen))
        all_lines = _http_lines(read_log())
        assert lines, (
            f"no `duckdb http ... request_id={rid}` line in the log with log-http-requests: true; "
            f"the fake S3 served {len(seen)} request(s) during the call: {seen}\n"
            f"all duckdb http lines: {[raw for raw, _ in all_lines]}")
        assert len(lines) == len(seen), (
            f"{len(lines)} log line(s) for request_id={rid}, but the fake S3 served {len(seen)} "
            f"request(s) during the call:\nfake: {seen}\nlog: {[raw for raw, _ in lines]}")
        for raw, m in lines:
            assert m.group("method") in ("GET", "HEAD", "PUT", "POST", "DELETE"), raw
            assert "mybucket/" in m.group("url"), raw
            assert m.group("status") != "ERR" and 100 <= int(m.group("status")) <= 599, raw
            assert int(m.group("ms")) >= 0, raw
        assert any("mybucket/data/obj3.csv" in m.group("url") for _, m in lines), \
            [raw for raw, _ in lines]
        unparsed = [raw for raw, m in all_lines if m is None]
        assert not unparsed, f"duckdb http lines not in the agreed format: {unparsed}"
        _assert_no_secrets(all_lines)
    finally:
        stop(proc)


def test_startup_remote_config_read_is_logged_without_a_request_id(bucket, workdir):
    proc, port, read_log = _serve(bucket, workdir, log_http_requests=True)
    try:
        boot = [(raw, m) for raw, m in _http_lines(read_log()) if m and m.group("rid") == "-"]
        assert boot, ("no `duckdb http ... request_id=-` line for the startup read of the remote "
                      f"templates; the fake S3 served at startup: {FakeS3.requests_seen}")
        assert any("mybucket/sqls/obj.yaml" in m.group("url") for _, m in boot), (
            "the endpoint YAML fetch at startup is not logged: " + str([raw for raw, _ in boot]))
        _assert_no_secrets(boot)
    finally:
        stop(proc)


def test_env_switch_logs_the_same_lines(bucket, workdir):
    proc, port, read_log = _serve(bucket, workdir, env_switch=True)
    try:
        rid, seen = _call(port, 5)
        assert seen, "scenario broken: the call made no request to the fake S3"
        lines = _lines_for(read_log, rid, len(seen))
        assert len(lines) == len(seen), (
            f"FLAPI_LOG_HTTP_REQUESTS=1: {len(lines)} line(s) for request_id={rid}, the fake S3 "
            f"served {len(seen)}:\nfake: {seen}\nlog: {[raw for raw, _ in lines]}")
        assert any("mybucket/data/obj5.csv" in m.group("url") for _, m in lines), [raw for raw, _ in lines]
        assert any(m.group("rid") == "-" for _, m in _http_lines(read_log()) if m), \
            "FLAPI_LOG_HTTP_REQUESTS=1 must also log the startup reads with request_id=-"
    finally:
        stop(proc)


def test_json_log_format_carries_the_same_message(bucket, workdir):
    proc, port, read_log = _serve(bucket, workdir, log_http_requests=True, log_format="json")
    try:
        rid, seen = _call(port, 1)
        assert seen, "scenario broken: the call made no request to the fake S3"
        lines = _lines_for(read_log, rid, len(seen))
        assert len(lines) == len(seen), (
            f"log-format: json: {len(lines)} record(s) whose message is a `duckdb http` line for "
            f"request_id={rid}, the fake S3 served {len(seen)}: {seen}")
        for raw, _ in lines:
            rec = json.loads(raw)
            assert rec["message"].startswith("duckdb http "), rec
            assert rec.get("level") == "info", rec
            assert rec["request_id"] == rid, rec
    finally:
        stop(proc)


def test_switch_off_logs_nothing_and_does_not_change_io(bucket, workdir):
    # Reference run with the switch on: how many requests one call costs.
    proc, port, read_log = _serve(bucket, workdir, log_http_requests=True)
    try:
        _, seen_on = _call(port, 2)
    finally:
        stop(proc)
    FakeS3.requests_seen.clear()
    FakeS3.auth_seen.clear()

    proc, port, read_log = _serve(bucket, workdir)  # default: off
    try:
        _, seen_off = _call(port, 2)
        time.sleep(0.5)
        lines = _http_lines(read_log())
        assert not lines, f"the switch is off but duckdb http lines were logged: {[raw for raw, _ in lines]}"
        assert len(seen_off) == len(seen_on), (
            f"logging changed the I/O: {len(seen_on)} request(s) with the switch on, {len(seen_off)} off\n"
            f"on: {seen_on}\noff: {seen_off}")
    finally:
        stop(proc)


def test_concurrent_requests_are_not_cross_attributed(bucket, workdir):
    proc, port, read_log = _serve(bucket, workdir, log_http_requests=True)
    try:
        def one(n):
            r = requests.get(f"http://127.0.0.1:{port}/obj", params={"n": n}, timeout=60)
            assert r.status_code == 200, r.text
            assert _rows(r)[0]["tag"] == f"obj{n}", r.text
            return n, r.headers["X-Request-Id"]

        with concurrent.futures.ThreadPoolExecutor(max_workers=N_OBJECTS) as pool:
            rid_of = dict(pool.map(one, range(N_OBJECTS)))
        n_of = {rid: n for n, rid in rid_of.items()}
        assert len(n_of) == N_OBJECTS, f"request ids are not distinct: {rid_of}"

        deadline = time.time() + 5
        while True:
            parsed = [(raw, m) for raw, m in _http_lines(read_log()) if m]
            seen_rids = {m.group("rid") for _, m in parsed}
            if set(n_of) <= seen_rids or time.time() > deadline:
                break
            time.sleep(0.1)

        missing = {rid_of[n]: n for n in range(N_OBJECTS) if rid_of[n] not in seen_rids}
        assert not missing, (
            f"no duckdb http line for {len(missing)} of the {N_OBJECTS} concurrent requests "
            f"(request_id -> n): {missing}\nall lines: {[raw for raw, _ in parsed]}")

        for raw, m in parsed:
            rid, url = m.group("rid"), m.group("url")
            data_obj = re.search(r"mybucket/data/obj(\d)\.csv", url)
            if rid == "-":
                assert not data_obj, f"a data read made while serving a request was logged with request_id=-: {raw}"
                continue
            assert rid in n_of, f"a duckdb http line carries a request_id none of the 8 calls had: {raw}"
            if data_obj:
                assert int(data_obj.group(1)) == n_of[rid], (
                    f"cross-talk: request_id={rid} asked for obj{n_of[rid]} but is logged reading "
                    f"obj{data_obj.group(1)}: {raw}")
        for n, rid in rid_of.items():
            assert any(m.group("rid") == rid and f"mybucket/data/obj{n}.csv" in m.group("url")
                       for _, m in parsed), f"request_id={rid} (n={n}) has no line for its own object"
        _assert_no_secrets(parsed)
    finally:
        stop(proc)


def test_reads_on_duckdb_worker_threads_carry_the_request_id(bucket, workdir):
    # Nine objects in one scan with four DuckDB threads: most GETs are issued by
    # DuckDB's worker threads, where no flAPI request is ambient.
    proc, port, read_log = _serve(bucket, workdir, log_http_requests=True, threads=4)
    try:
        r, rid, seen = _get(port, "/many")
        assert r.status_code == 200, r.text
        assert _rows(r)[0]["rows"] == N_OBJECTS + 300_000, r.text
        assert len(seen) >= N_OBJECTS + 1, f"scenario broken: the scan made only {len(seen)} request(s): {seen}"
        lines = _lines_for(read_log, rid, len(seen))
        assert len(lines) == len(seen), (
            f"{len(lines)} line(s) carry request_id={rid}, but the fake S3 served {len(seen)} request(s) "
            f"during the call; the rest were logged without it or not at all:\nfake: {seen}\n"
            f"log: {[raw for raw, _ in _http_lines(read_log())]}")
        for i in range(N_OBJECTS):
            assert any(f"mybucket/data/obj{i}.csv" in m.group("url") for _, m in lines), (i, [r for r, _ in lines])
        assert any("mybucket/big/big.csv" in m.group("url") for _, m in lines), [raw for raw, _ in lines]
        _assert_no_secrets(_http_lines(read_log()))
    finally:
        stop(proc)


@pytest.mark.parametrize("duckdb_redacts", [True, False], ids=["duckdb-redacts", "redact_http_logs-false"])
def test_credentials_in_urls_never_reach_the_log(bucket, workdir, duckdb_redacts):
    # With `SET redact_http_logs = false` in a connection's init, DuckDB itself
    # stops redacting; flAPI must still keep the signature and the password out.
    init_extra = "" if duckdb_redacts else "      SET redact_http_logs = false;\n"
    proc, port, read_log = _serve(bucket, workdir, log_http_requests=True, init_extra=init_extra)
    try:
        for path in ("/presigned", "/userinfo"):
            r, rid, seen = _get(port, path)
            assert r.status_code == 200, f"{path}: {r.status_code} {r.text}"
            assert _rows(r)[0]["tag"] == "obj0", r.text
            assert seen, f"scenario broken: {path} made no request to the fake store"
            lines = _lines_for(read_log, rid, len(seen))
            assert len(lines) == len(seen), (
                f"{path}: {len(lines)} line(s) for request_id={rid}, the fake served {len(seen)}: {seen}\n"
                f"log: {[raw for raw, _ in _http_lines(read_log())]}")
            assert all("mybucket/data/obj0.csv" in m.group("url") for _, m in lines), [raw for raw, _ in lines]
        everything = read_log()
        http = [raw for raw, _ in _http_lines(everything)]
        for raw in http:
            for s in SECRETS:
                assert s not in raw, f"a duckdb http line leaks {s!r}: {raw}"
    finally:
        stop(proc)


def test_failed_requests_are_logged_with_their_outcome(bucket, workdir):
    proc, port, read_log = _serve(bucket, workdir, log_http_requests=True)
    try:
        r, rid, seen = _get(port, "/missing")
        assert r.status_code >= 400, r.text
        assert seen, "scenario broken: /missing made no request to the fake S3"
        lines = _lines_for(read_log, rid, len(seen))
        assert len(lines) == len(seen), (
            f"{len(lines)} line(s) for request_id={rid}, the fake served {len(seen)}: {seen}\n"
            f"log: {[raw for raw, _ in _http_lines(read_log())]}")
        assert any(m.group("status") == "404" and "mybucket/data/nope.csv" in m.group("url") for _, m in lines), (
            "the 404 for the missing object is not logged as `-> 404`: " + str([raw for raw, _ in lines]))

        r, rid, _ = _get(port, "/dead", timeout=120)
        assert r.status_code >= 400, r.text
        lines = _lines_for(read_log, rid, 1)
        assert lines, (f"no duckdb http line for request_id={rid} (connection refused); all lines: "
                       f"{[raw for raw, _ in _http_lines(read_log())]}")
        assert all(m.group("status") == "ERR" for _, m in lines), (
            "a request that got no response must log `-> ERR`: " + str([raw for raw, _ in lines]))
        assert all(f"127.0.0.1:{_DEAD_PORT}/x.csv" in m.group("url") for _, m in lines), [raw for raw, _ in lines]
        unparsed = [raw for raw, m in _http_lines(read_log()) if m is None]
        assert not unparsed, f"duckdb http lines not in the agreed format: {unparsed}"
    finally:
        stop(proc)


def test_remote_main_config_read_is_logged_without_a_request_id(bucket, workdir):
    # `-c s3://...`: the main configuration is fetched by the bootstrap DuckDB,
    # before the configuration (and so the config key) exists: only the
    # environment switch can cover it.
    root, endpoint = bucket
    put(root, "mybucket/flapi.yaml", _config(None, "text"))
    proc, port, read_log = _serve(bucket, workdir, env_switch=True, config_arg="s3://mybucket/flapi.yaml")
    try:
        boot = [(raw, m) for raw, m in _http_lines(read_log()) if m and m.group("rid") == "-"]
        assert any("mybucket/flapi.yaml" in m.group("url") for _, m in boot), (
            "the fetch of the remote main configuration is not logged with request_id=-; "
            f"fake S3 saw {FakeS3.requests_seen[:10]}, lines: {[raw for raw, _ in _http_lines(read_log())]}")
        _assert_no_secrets(_http_lines(read_log()))
    finally:
        stop(proc)


@pytest.mark.parametrize("config_key,env_value,expect_lines", [
    (True, "0", False),
    (True, "false", False),
    (False, "1", True),
    (False, "true", True),
], ids=["key-on-env-0", "key-on-env-false", "key-off-env-1", "key-off-env-true"])
def test_environment_switch_wins_over_the_config_key(bucket, workdir, config_key, env_value, expect_lines):
    proc, port, read_log = _serve(bucket, workdir, log_http_requests=config_key, env_switch=env_value)
    try:
        rid, seen = _call(port, 4)
        assert seen, "scenario broken: the call made no request to the fake S3"
        if expect_lines:
            lines = _lines_for(read_log, rid, len(seen))
            assert len(lines) == len(seen), (
                f"log-http-requests: {config_key} with FLAPI_LOG_HTTP_REQUESTS={env_value}: expected "
                f"{len(seen)} line(s) for request_id={rid}, got {[raw for raw, _ in lines]}")
        else:
            time.sleep(0.5)
            lines = _http_lines(read_log())
            assert not lines, (
                f"log-http-requests: {config_key} with FLAPI_LOG_HTTP_REQUESTS={env_value} must log nothing: "
                f"{[raw for raw, _ in lines]}")
    finally:
        stop(proc)
