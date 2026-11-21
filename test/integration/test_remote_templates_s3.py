"""s3:// template.path listed with a glob (#219), against an S3-compatible fake.

A tiny path-style fake (ListObjectsV2 + GET + HEAD, no signature check) over a
temp dir stands in for the bucket, so the glob listing that boots a server from
`template.path: s3://bucket/sqls` - with no manifest - is exercised end to end.
"""
import functools
import http.server
import os
import shutil
import subprocess
import tempfile
import threading
import urllib.parse
from xml.sax.saxutils import escape

import pytest

import requests

from otel_helpers import flapi_binary, free_port
from test_remote_templates import ENDPOINT, ENV, SQL, main_config, put, stop, workdir  # noqa: F401

pytestmark = pytest.mark.standalone_server


class FakeS3(http.server.BaseHTTPRequestHandler):
    root = None
    requests_seen = []
    auth_seen = []

    def log_message(self, *args):
        pass

    def _send(self, code, body=b"", ctype="application/xml"):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("ETag", '"x"')
        self.send_header("Last-Modified", "Sat, 21 Nov 2026 00:00:00 GMT")
        self.send_header("Accept-Ranges", "bytes")
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def _handle(self):
        FakeS3.requests_seen.append(f"{self.command} {self.path}")
        FakeS3.auth_seen.append(self.headers.get("Authorization", ""))
        u = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(u.query)
        parts = u.path.lstrip("/").split("/", 1)
        bucket, key = parts[0], (parts[1] if len(parts) > 1 else "")
        base = os.path.join(self.root, bucket)
        if "list-type" in q or (not key and self.command == "GET"):
            prefix = q.get("prefix", [""])[0]
            keys = []
            for dirpath, _, names in os.walk(base):
                for n in names:
                    rel = os.path.relpath(os.path.join(dirpath, n), base).replace(os.sep, "/")
                    if rel.startswith(prefix):
                        keys.append(rel)
            items = "".join(
                f"<Contents><Key>{escape(k)}</Key><Size>{os.path.getsize(os.path.join(base, k))}</Size>"
                f"<LastModified>2026-11-21T00:00:00.000Z</LastModified><ETag>\"x\"</ETag></Contents>"
                for k in sorted(keys))
            xml = ('<?xml version="1.0" encoding="UTF-8"?><ListBucketResult>'
                   f"<Name>{bucket}</Name><Prefix>{escape(prefix)}</Prefix><KeyCount>{len(keys)}</KeyCount>"
                   f"<MaxKeys>1000</MaxKeys><IsTruncated>false</IsTruncated>{items}</ListBucketResult>")
            return self._send(200, xml.encode())
        path = os.path.join(base, urllib.parse.unquote(key))
        if not os.path.isfile(path):
            return self._send(404, b"<Error><Code>NoSuchKey</Code></Error>")
        data = open(path, "rb").read()
        rng = self.headers.get("Range")
        if rng and rng.startswith("bytes="):
            lo, _, hi = rng[6:].partition("-")
            lo = int(lo)
            hi = int(hi) if hi else len(data) - 1
            chunk = data[lo:hi + 1]
            self.send_response(206)
            self.send_header("Content-Range", f"bytes {lo}-{lo + len(chunk) - 1}/{len(data)}")
            self.send_header("Content-Length", str(len(chunk)))
            self.send_header("ETag", '"x"')
            self.send_header("Last-Modified", "Sat, 21 Nov 2026 00:00:00 GMT")
            self.end_headers()
            if self.command != "HEAD":
                self.wfile.write(chunk)
            return
        self._send(200, data, "application/octet-stream")

    do_GET = do_HEAD = _handle


@pytest.fixture
def bucket():
    d = tempfile.mkdtemp(prefix="flapi_fake_s3_")
    FakeS3.root = d
    FakeS3.requests_seen = []
    FakeS3.auth_seen = []
    httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), FakeS3)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    yield d, f"http://127.0.0.1:{httpd.server_address[1]}"
    httpd.shutdown()
    shutil.rmtree(d, ignore_errors=True)


def s3_env(endpoint):
    return {"AWS_ENDPOINT_URL": endpoint, "AWS_ACCESS_KEY_ID": "AKIATESTKEY", "AWS_SECRET_ACCESS_KEY": "y",
            "AWS_REGION": "eu-central-1"}


def test_glob_lists_endpoints_without_a_manifest(bucket, workdir):
    root, endpoint = bucket
    put(root, "mybucket/sqls/hello.yaml", ENDPOINT)
    put(root, "mybucket/sqls/hello.sql", SQL)
    put(root, "mybucket/sqls/nested/deeper.yaml", ENDPOINT.replace("/remote-hello", "/deeper"))
    put(root, "mybucket/sqls/nested/hello.sql", SQL)
    put(workdir, "flapi.yaml", main_config("s3://mybucket/sqls"))
    r = subprocess.run(
        [flapi_binary(), "-c", os.path.join(workdir, "flapi.yaml"), "--validate-config", "--log-level", "info"],
        cwd=workdir, capture_output=True, text=True, timeout=120, env={**ENV, **s3_env(endpoint)})
    out = r.stdout + r.stderr
    assert r.returncode == 0, out + "\n".join(FakeS3.requests_seen)
    assert "Parsed 2 endpoint" in out, out
    # The environment's credentials and region must actually reach DuckDB: the
    # requests are SigV4-signed with them (the fake does not verify, it records).
    signed = [a for a in FakeS3.auth_seen if a]
    assert signed, "no request was signed - the S3 credentials were never applied"
    assert all("Credential=AKIATESTKEY/" in a and "/eu-central-1/s3/" in a for a in signed), signed[:2]


def test_server_serves_an_endpoint_listed_from_a_bucket(bucket, workdir):
    import signal
    root, endpoint = bucket
    put(root, "mybucket/sqls/hello.yaml", ENDPOINT)
    put(root, "mybucket/sqls/hello.sql", SQL)
    put(workdir, "flapi.yaml", main_config("s3://mybucket/sqls"))
    port = free_port()
    log = open(os.path.join(workdir, "server.log"), "w")
    proc = subprocess.Popen(
        [flapi_binary(), "-c", os.path.join(workdir, "flapi.yaml"), "-p", str(port), "--log-level", "info"],
        cwd=workdir, stdout=log, stderr=subprocess.STDOUT, preexec_fn=os.setsid,
        env={**ENV, **s3_env(endpoint)})
    try:
        import time
        deadline = time.time() + 90
        while time.time() < deadline:
            assert proc.poll() is None, open(os.path.join(workdir, "server.log")).read()
            try:
                if requests.get(f"http://127.0.0.1:{port}/health/live", timeout=1).status_code == 200:
                    break
            except requests.RequestException:
                time.sleep(0.3)
        r = requests.get(f"http://127.0.0.1:{port}/remote-hello", timeout=30)
        assert r.status_code == 200, r.text
        assert r.json()["data"][0] == {"answer": 42, "origin": "from-remote"}
    finally:
        stop(proc)


def test_an_empty_prefix_falls_back_to_the_manifest(bucket, workdir):
    root, endpoint = bucket
    put(root, "mybucket/other/placeholder.txt", "x")  # bucket exists, prefix is empty
    put(workdir, "flapi.yaml", main_config("s3://mybucket/sqls"))
    r = subprocess.run(
        [flapi_binary(), "-c", os.path.join(workdir, "flapi.yaml"), "--validate-config", "--log-level", "info"],
        cwd=workdir, capture_output=True, text=True, timeout=120, env={**ENV, **s3_env(endpoint)})
    out = r.stdout + r.stderr
    assert r.returncode != 0, out
    assert "manifest.txt" in out, out
