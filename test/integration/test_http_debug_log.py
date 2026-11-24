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

LINE_RE = re.compile(
    r"duckdb http (?P<method>[A-Z]+) (?P<url>\S+) -> (?P<status>\d{3}) (?P<ms>\d+)ms request_id=(?P<rid>[^\s\"]+)")

SECRETS = ("AKIATESTKEY", "Signature=", "Authorization")


def _config(log_http_requests, log_format):
    key = "" if log_http_requests is None else f"log-http-requests: {'true' if log_http_requests else 'false'}\n"
    return (
        "project-name: http-debug-log\n"
        f"log-format: {log_format}\n"
        f"{key}"
        "template:\n  path: s3://mybucket/sqls\n"
        "connections:\n  inmem:\n    init: |\n      INSTALL httpfs;\n      LOAD httpfs;\n"
        "    properties:\n      database: ':memory:'\n"
        "rate_limit:\n  enabled: false\n")


def _serve(bucket_fixture, workdir, *, log_http_requests=None, env_switch=False, log_format="text"):
    """Fill the bucket, boot a server against it; returns (proc, port, read_log)."""
    root, endpoint = bucket_fixture
    put(root, "mybucket/sqls/obj.yaml", OBJ_ENDPOINT)
    put(root, "mybucket/sqls/obj.sql", OBJ_SQL)
    for i in range(N_OBJECTS):
        put(root, f"mybucket/data/obj{i}.csv", f"n,tag\n{i},obj{i}\n")
    put(workdir, "flapi.yaml", _config(log_http_requests, log_format))

    env = {**ENV, **s3_env(endpoint)}
    env.pop("FLAPI_LOG_HTTP_REQUESTS", None)
    if env_switch:
        env["FLAPI_LOG_HTTP_REQUESTS"] = "1"

    port = free_port()
    log_path = os.path.join(workdir, f"server-{port}.log")
    log = open(log_path, "w")
    proc = subprocess.Popen(
        [flapi_binary(), "-c", os.path.join(workdir, "flapi.yaml"), "-p", str(port), "--log-level", "info"],
        cwd=workdir, stdout=log, stderr=subprocess.STDOUT, preexec_fn=os.setsid, env=env)

    def read_log():
        log.flush()
        with open(log_path, errors="replace") as f:
            return f.read()

    deadline = time.time() + 90
    while time.time() < deadline:
        if proc.poll() is not None:
            hint = ""
            if log_http_requests is not None and "log-http-requests" in read_log():
                hint = "\nThe server refused the new top-level key `log-http-requests` (unknown key fails startup)."
            pytest.fail(f"server exited during startup{hint}:\n" + read_log()[-4000:])
        try:
            if requests.get(f"http://127.0.0.1:{port}/health/live", timeout=1).status_code == 200:
                return proc, port, read_log
        except requests.RequestException:
            time.sleep(0.3)
    stop(proc)
    pytest.fail("server never became live:\n" + read_log()[-4000:])


def _http_lines(text):
    """Every `duckdb http` line, parsed. JSON records are unwrapped to their message."""
    out = []
    for raw in text.splitlines():
        if "duckdb http" not in raw:
            continue
        msg = raw
        if raw.lstrip().startswith("{"):
            try:
                msg = json.loads(raw).get("message", "")
            except json.JSONDecodeError:
                continue  # still being written
        m = LINE_RE.search(msg)
        out.append((raw, m))
    return out


def _rows(r):
    body = r.json()
    return body["data"] if isinstance(body, dict) else body


def _call(port, n):
    """GET /obj?n=; returns (request_id, fake requests seen while it ran)."""
    before = len(FakeS3.requests_seen)
    r = requests.get(f"http://127.0.0.1:{port}/obj", params={"n": n}, timeout=30)
    after = len(FakeS3.requests_seen)
    assert r.status_code == 200, r.text
    assert _rows(r)[0]["tag"] == f"obj{n}", r.text
    rid = r.headers.get("X-Request-Id")
    assert rid, "the response carries no X-Request-Id"
    return rid, FakeS3.requests_seen[before:after]


def _lines_for(read_log, rid, expected, timeout=5.0):
    """Poll until `expected` lines for `rid` are in the log (a logger may write late)."""
    deadline = time.time() + timeout
    while True:
        lines = [(raw, m) for raw, m in _http_lines(read_log()) if m and m.group("rid") == rid]
        if len(lines) >= expected or time.time() > deadline:
            return lines
        time.sleep(0.1)


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
            assert 100 <= int(m.group("status")) <= 599, raw
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
