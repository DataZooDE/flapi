"""How many HTTP requests one paginated GET of a remote file makes (#279).

A full page runs two queries over the template, the page and a COUNT(*) for
`total_count` (database_manager.cpp RunPaginated), so each query opens the
remote file. What that costs depends on DuckDB 2.0.1's external file cache
(`enable_external_file_cache`, on by default, `validate_external_file_cache:
VALIDATE_ALL`):

* the server sends an ETag (S3, GCS, Azure, nginx): the body is fetched once
  and reused by the count query and by later requests; every query revalidates
  with HEAD requests, so a full page costs twice the HEADs and no second GET;
* no ETag (Python's http.server, some static hosts): the cache cannot
  validate, so every query fetches the body again: a full page fetches it twice.

`total-count: false` skips the count query and halves both. The settings that
would drop the HEADs (`enable_http_metadata_cache: true`,
`validate_external_file_cache: NO_VALIDATION`) serve a changed file stale, so
flAPI does not set them; the last test pins that the defaults see a change.

The file server below counts what it serves; `log-http-requests` must report
the same requests, each under the request id of the GET that caused it.
"""
import collections
import email.utils
import http.server
import os
import re
import shutil
import subprocess
import tempfile
import threading
import time

import pytest
import requests

from otel_helpers import flapi_binary, free_port

pytestmark = pytest.mark.standalone_server

ROWS = 300_000   # ~6 MB: several range requests per scan


class _Files(http.server.BaseHTTPRequestHandler):
    """Static files with Range support. /etag/... sends ETag and Last-Modified,
    /plain/... neither. Every request is recorded as (method, path)."""
    protocol_version = "HTTP/1.1"
    root = ""
    seen = []
    lock = threading.Lock()

    def log_message(self, *args):
        pass

    def _serve(self, with_body):
        path = self.path.split("?", 1)[0]
        with self.lock:
            type(self).seen.append((self.command, path))
        kind, _, name = path.lstrip("/").partition("/")
        full = os.path.join(self.root, name)
        if kind not in ("etag", "plain") or not os.path.isfile(full):
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        with open(full, "rb") as f:
            data = f.read()
        rng = self.headers.get("Range")
        if rng:
            m = re.match(r"bytes=(\d+)-(\d*)", rng)
            start = int(m.group(1))
            end = min(int(m.group(2)) if m.group(2) else len(data) - 1, len(data) - 1)
            chunk = data[start:end + 1]
            self.send_response(206)
            self.send_header("Content-Range", f"bytes {start}-{end}/{len(data)}")
        else:
            chunk = data
            self.send_response(200)
        self.send_header("Accept-Ranges", "bytes")
        if kind == "etag":
            mtime = os.path.getmtime(full)
            self.send_header("ETag", f'"{len(data)}-{int(mtime * 1000)}"')
            self.send_header("Last-Modified", email.utils.formatdate(mtime, usegmt=True))
        self.send_header("Content-Length", str(len(chunk)))
        self.end_headers()
        if with_body:
            self.wfile.write(chunk)

    def do_GET(self):
        self._serve(True)

    def do_HEAD(self):
        self._serve(False)


def _write_csv(root, name, tag):
    with open(os.path.join(root, name), "w") as f:
        f.write("n,tag\n" + "".join(f"{i},{tag}{i:012d}\n" for i in range(ROWS)))


@pytest.fixture(scope="module")
def setup():
    tmp = tempfile.mkdtemp(prefix="flapi_pagefetch_")
    www = os.path.join(tmp, "www")
    os.makedirs(www)
    os.makedirs(os.path.join(tmp, "sqls"))
    _write_csv(www, "data.csv", "a")
    _write_csv(www, "changing.csv", "old")

    _Files.root = www
    files = http.server.ThreadingHTTPServer(("127.0.0.1", 0), _Files)
    threading.Thread(target=files.serve_forever, daemon=True).start()
    fport = files.server_address[1]

    with open(os.path.join(tmp, "flapi.yaml"), "w") as f:
        f.write("project-name: pagefetch\nlog-http-requests: true\n"
                "template:\n  path: ./sqls\n"
                "connections:\n  inmem:\n    init: |\n      INSTALL httpfs;\n      LOAD httpfs;\n"
                "    properties:\n      database: ':memory:'\n"
                "rate_limit:\n  enabled: false\n")
    endpoints = {
        "etag": ("etag/data.csv", True), "etag_nocount": ("etag/data.csv", False),
        "plain": ("plain/data.csv", True), "plain_nocount": ("plain/data.csv", False),
        "changing": ("etag/changing.csv", True),
    }
    for name, (rel, count) in endpoints.items():
        with open(os.path.join(tmp, "sqls", f"{name}.yaml"), "w") as f:
            f.write(f"url-path: /{name}\nmethod: GET\ntemplate-source: {name}.sql\nconnection: [inmem]\n"
                    + ("" if count else "total-count: false\n"))
        with open(os.path.join(tmp, "sqls", f"{name}.sql"), "w") as f:
            f.write(f"SELECT n, tag FROM read_csv('http://127.0.0.1:{fport}/{rel}') ORDER BY n\n")

    port = free_port()
    log_path = os.path.join(tmp, "server.log")
    proc = subprocess.Popen([flapi_binary(), "-c", os.path.join(tmp, "flapi.yaml"), "-p", str(port),
                             "--log-level", "info"],
                            cwd=tmp, stdout=open(log_path, "w"), stderr=subprocess.STDOUT,
                            env={**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1",
                                 "FLAPI_LOG_HTTP_REQUESTS": "1"})
    base = f"http://127.0.0.1:{port}"
    deadline = time.time() + 60
    while time.time() < deadline:
        if proc.poll() is not None:
            pytest.fail("server exited:\n" + open(log_path).read()[-3000:])
        try:
            if requests.get(f"{base}/health/live", timeout=1).status_code == 200:
                break
        except requests.RequestException:
            time.sleep(0.2)
    yield {"base": base, "www": www, "log": log_path}
    proc.terminate()
    proc.wait(timeout=30)
    files.shutdown()
    shutil.rmtree(tmp, ignore_errors=True)


def _get(setup, path):
    """GET a full page (limit=2 of ROWS rows, so the count query runs).
    Returns (body, request_id, Counter of what the file server served)."""
    before = len(_Files.seen)
    r = requests.get(f"{setup['base']}{path}?limit=2", timeout=120)
    assert r.status_code == 200, r.text
    time.sleep(0.2)
    served = collections.Counter(m for m, _ in _Files.seen[before:])
    return r.json(), r.headers["X-Request-Id"], served


def _logged(setup, rid):
    deadline = time.time() + 5
    while True:
        lines = [l for l in open(setup["log"]).read().splitlines()
                 if "duckdb http" in l and f"request_id={rid}" in l]
        if lines or time.time() > deadline:
            return collections.Counter(re.search(r"duckdb http ([A-Z]+) ", l).group(1) for l in lines)
        time.sleep(0.1)


def test_with_an_etag_the_body_is_fetched_once_and_the_count_costs_heads(setup):
    body, rid, first = _get(setup, "/etag")
    assert body["total_count"] == ROWS and len(body["data"]) == 2
    assert first["GET"] > 0, first
    _, _, nocount = _get(setup, "/etag_nocount")
    _, rid2, again = _get(setup, "/etag")
    # Cached and revalidated: no body on the second request, only HEADs,
    # twice those of a request without the count query.
    assert again.get("GET", 0) == 0, again
    assert nocount.get("GET", 0) == 0, nocount
    assert again["HEAD"] == 2 * nocount["HEAD"], (again, nocount)
    # log-http-requests reports exactly what the server served.
    assert _logged(setup, rid2) == again, (_logged(setup, rid2), again)
    assert _logged(setup, rid) == first, (_logged(setup, rid), first)


def test_without_an_etag_a_full_page_fetches_the_body_twice(setup):
    _, _, paged = _get(setup, "/plain")
    _, _, nocount = _get(setup, "/plain_nocount")
    _, _, again = _get(setup, "/plain")
    assert nocount["GET"] > 0, nocount
    assert paged["GET"] == 2 * nocount["GET"], (paged, nocount)
    assert again["GET"] == paged["GET"], "nothing is reused without an ETag"
    assert paged["HEAD"] == 2 * nocount["HEAD"], (paged, nocount)


def test_total_count_false_has_no_total(setup):
    body, _, _ = _get(setup, "/etag_nocount")
    assert "total_count" not in body and body["next"], body


def test_the_default_cache_sees_a_changed_file(setup):
    body, _, _ = _get(setup, "/changing")
    assert body["data"][0]["tag"].startswith("old")
    time.sleep(1.1)   # a new mtime, so a new ETag
    with open(os.path.join(setup["www"], "changing.csv"), "w") as f:
        f.write("n,tag\n0,new0\n1,new1\n2,new2\n")
    body, _, served = _get(setup, "/changing")
    assert body["data"][0]["tag"] == "new0", body
    assert body["total_count"] == 3, body
    assert served["GET"] > 0, served
