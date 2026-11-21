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


SETTINGS_SQL = "SELECT secret_string AS value FROM duckdb_secrets() WHERE name = 'flapi_s3'"


def _settings_server(workdir, extra_duckdb, env):
    put(workdir, "flapi.yaml",
        "project-name: s3prec\ntemplate:\n  path: ./sqls\nconnections:\n  inmem:\n    init: |\n"
        "      INSTALL httpfs;\n      LOAD httpfs;\n    properties:\n      database: ':memory:'\n"
        "rate_limit:\n  enabled: false\n" + extra_duckdb)
    put(workdir, "sqls/r.yaml", "url-path: /region\nmethod: GET\ntemplate-source: r.sql\nconnection: [inmem]\n")
    put(workdir, "sqls/r.sql", SETTINGS_SQL)
    port = free_port()
    log = open(os.path.join(workdir, "prec.log"), "w")
    proc = subprocess.Popen(
        [flapi_binary(), "-c", os.path.join(workdir, "flapi.yaml"), "-p", str(port), "--log-level", "info"],
        cwd=workdir, stdout=log, stderr=subprocess.STDOUT, preexec_fn=os.setsid, env={**ENV, **env})
    import time
    deadline = time.time() + 90
    while time.time() < deadline:
        assert proc.poll() is None, open(os.path.join(workdir, "prec.log")).read()
        try:
            if requests.get(f"http://127.0.0.1:{port}/health/live", timeout=1).status_code == 200:
                break
        except requests.RequestException:
            time.sleep(0.3)
    return proc, port


def test_user_configured_s3_setting_wins_over_the_environment(workdir):
    proc, port = _settings_server(
        workdir, "duckdb:\n  s3_region: eu-west-1\n",
        {"AWS_ACCESS_KEY_ID": "AKIATESTKEY", "AWS_SECRET_ACCESS_KEY": "y", "AWS_REGION": "us-east-1"})
    try:
        r = requests.get(f"http://127.0.0.1:{port}/region", timeout=30)
        assert r.status_code == 200, r.text
        assert "region=eu-west-1" in r.json()["data"][0]["value"], r.text
    finally:
        stop(proc)


def test_environment_applies_when_the_user_set_nothing(workdir):
    proc, port = _settings_server(
        workdir, "", {"AWS_ACCESS_KEY_ID": "AKIATESTKEY", "AWS_SECRET_ACCESS_KEY": "y", "AWS_REGION": "us-east-1"})
    try:
        r = requests.get(f"http://127.0.0.1:{port}/region", timeout=30)
        assert r.status_code == 200, r.text
        assert "region=us-east-1" in r.json()["data"][0]["value"], r.text
    finally:
        stop(proc)


def test_gs_template_path_lists_with_hmac_credentials(bucket, workdir):
    """gs:// goes through the S3-interoperability API with an HMAC key pair (#234)."""
    root, endpoint = bucket
    put(root, "gbucket/sqls/hello.yaml", ENDPOINT)
    put(root, "gbucket/sqls/hello.sql", SQL)
    put(workdir, "flapi.yaml", main_config("gs://gbucket/sqls"))
    r = subprocess.run(
        [flapi_binary(), "-c", os.path.join(workdir, "flapi.yaml"), "--validate-config", "--log-level", "info"],
        cwd=workdir, capture_output=True, text=True, timeout=120,
        env={**ENV, "GCS_ACCESS_KEY_ID": "GOOGHMACKEY", "GCS_SECRET_ACCESS_KEY": "hmacsecret",
             "GCS_ENDPOINT_URL": endpoint})
    out = r.stdout + r.stderr
    assert r.returncode == 0 and "Parsed 1 endpoint" in out, out + "\n".join(FakeS3.requests_seen)
    signed = [a for a in FakeS3.auth_seen if a]
    assert signed and all("GOOGHMACKEY" in a for a in signed), signed[:2]
    assert "hmacsecret" not in out, "the HMAC secret must not be logged"


class FakeAzure(http.server.BaseHTTPRequestHandler):
    """Azure Blob (Azurite-style path addressing: /<account>/<container>/<blob>)."""
    root = None
    seen = []

    def log_message(self, *args):
        pass

    def _reply(self, code, body=b"", extra=None):
        self.send_response(code)
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def _handle(self):
        FakeAzure.seen.append(f"{self.command} {self.path}")
        u = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(u.query)
        parts = u.path.lstrip("/").split("/", 2)  # account, container, blob
        container = parts[1] if len(parts) > 1 else ""
        blob = parts[2] if len(parts) > 2 else ""
        base = os.path.join(self.root, container)
        if q.get("comp") == ["list"]:
            prefix = q.get("prefix", [""])[0]
            names = []
            for dirpath, _, files in os.walk(base):
                for n in files:
                    rel = os.path.relpath(os.path.join(dirpath, n), base).replace(os.sep, "/")
                    if rel.startswith(prefix):
                        names.append(rel)
            blobs = "".join(
                f"<Blob><Name>{escape(n)}</Name><Properties><Content-Length>{os.path.getsize(os.path.join(base, n))}"
                f"</Content-Length><Last-Modified>Sat, 21 Nov 2026 00:00:00 GMT</Last-Modified>"
                f"<Etag>0x1</Etag><BlobType>BlockBlob</BlobType></Properties></Blob>" for n in sorted(names))
            xml = ('<?xml version="1.0" encoding="utf-8"?><EnumerationResults ServiceEndpoint="x" '
                   f'ContainerName="{container}"><Prefix>{escape(prefix)}</Prefix><Blobs>{blobs}</Blobs>'
                   "<NextMarker /></EnumerationResults>")
            return self._reply(200, xml.encode(), {"Content-Type": "application/xml"})
        path = os.path.join(base, blob)
        if not os.path.isfile(path):
            return self._reply(404, b"<Error><Code>BlobNotFound</Code></Error>", {"x-ms-error-code": "BlobNotFound"})
        data = open(path, "rb").read()
        meta = {"ETag": '"0x1"', "Last-Modified": "Sat, 21 Nov 2026 00:00:00 GMT", "x-ms-blob-type": "BlockBlob",
                "Accept-Ranges": "bytes", "Content-Type": "application/octet-stream",
                "x-ms-creation-time": "Sat, 21 Nov 2026 00:00:00 GMT", "x-ms-lease-status": "unlocked",
                "x-ms-lease-state": "available", "x-ms-server-encrypted": "true", "x-ms-access-tier": "Hot",
                "x-ms-request-id": "0", "x-ms-version": "2021-12-02", "x-ms-meta-name": "x"}
        rng = self.headers.get("Range") or self.headers.get("x-ms-range")
        if rng and rng.startswith("bytes=") and self.command == "GET":
            lo, _, hi = rng[6:].partition("-")
            lo = int(lo)
            hi = int(hi) if hi else len(data) - 1
            chunk = data[lo:hi + 1]
            return self._reply(206, chunk, {**meta, "Content-Range": f"bytes {lo}-{lo + len(chunk) - 1}/{len(data)}"})
        self._reply(200, data, meta)

    do_GET = do_HEAD = _handle


def test_az_template_path_lists_through_the_azure_extension(workdir):
    d = tempfile.mkdtemp(prefix="flapi_fake_az_")
    FakeAzure.root = d
    FakeAzure.seen = []
    httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), FakeAzure)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    try:
        put(d, "cont/sqls/hello.yaml", ENDPOINT)
        put(d, "cont/sqls/hello.sql", SQL)
        put(workdir, "flapi.yaml", main_config("az://cont/sqls"))
        conn = ("DefaultEndpointsProtocol=http;AccountName=devstoreaccount1;AccountKey=c2VjcmV0a2V5;"
                f"BlobEndpoint=http://127.0.0.1:{httpd.server_address[1]}/devstoreaccount1")
        r = subprocess.run(
            [flapi_binary(), "-c", os.path.join(workdir, "flapi.yaml"), "--validate-config", "--log-level", "info"],
            cwd=workdir, capture_output=True, text=True, timeout=180,
            env={**ENV, "AZURE_STORAGE_CONNECTION_STRING": conn})
        out = r.stdout + r.stderr
        assert r.returncode == 0 and "Parsed 1 endpoint" in out, out + "\n".join(FakeAzure.seen)
        assert "c2VjcmV0a2V5" not in out, "the account key must not be logged"
    finally:
        httpd.shutdown()
        shutil.rmtree(d, ignore_errors=True)


def test_an_uppercase_endpoint_scheme_still_selects_ssl(bucket, workdir):
    root, endpoint = bucket
    put(root, "mybucket/sqls/hello.yaml", ENDPOINT)
    put(root, "mybucket/sqls/hello.sql", SQL)
    put(workdir, "flapi.yaml", main_config("s3://mybucket/sqls"))
    r = subprocess.run(
        [flapi_binary(), "-c", os.path.join(workdir, "flapi.yaml"), "--validate-config", "--log-level", "info"],
        cwd=workdir, capture_output=True, text=True, timeout=120,
        env={**ENV, **s3_env(endpoint.replace("http://", "HTTP://"))})
    out = r.stdout + r.stderr
    assert r.returncode == 0 and "Parsed 1 endpoint" in out, out


def test_a_key_with_a_hash_or_question_mark_is_not_mangled(bucket, workdir):
    root, endpoint = bucket
    put(root, "mybucket/a#1/hello.yaml", ENDPOINT)
    put(root, "mybucket/a#1/hello.sql", SQL)
    put(root, "mybucket/a#1/manifest.txt", "hello.yaml\n")
    put(workdir, "flapi.yaml", main_config("s3://mybucket/a#1"))
    r = subprocess.run(
        [flapi_binary(), "-c", os.path.join(workdir, "flapi.yaml"), "--validate-config", "--log-level", "info"],
        cwd=workdir, capture_output=True, text=True, timeout=120, env={**ENV, **s3_env(endpoint)})
    out = r.stdout + r.stderr
    assert r.returncode == 0 and "Parsed 1 endpoint" in out, out


def test_azure_env_without_az_paths_downloads_nothing(bucket, workdir):
    # AZURE_STORAGE_* in the environment must not trigger an extension download when
    # nothing reads az://: an offline, empty extension dir stays empty.
    root, endpoint = bucket
    put(root, "mybucket/sqls/hello.yaml", ENDPOINT)
    put(root, "mybucket/sqls/hello.sql", SQL)
    put(workdir, "flapi.yaml", main_config("s3://mybucket/sqls"))
    ext = os.path.join(workdir, "ext")
    os.makedirs(ext)
    r = subprocess.run(
        [flapi_binary(), "-c", os.path.join(workdir, "flapi.yaml"), "--validate-config", "--log-level", "info"],
        cwd=workdir, capture_output=True, text=True, timeout=120,
        env={**ENV, **s3_env(endpoint), "AZURE_STORAGE_CONNECTION_STRING": "AccountName=a;AccountKey=Yg==",
             "FLAPI_EXTENSION_DIRECTORY": ext})
    out = r.stdout + r.stderr
    assert r.returncode == 0, out
    files = [f for _, _, names in os.walk(ext) for f in names]
    assert not any("azure" in f for f in files), files


def test_a_template_cannot_read_the_s3_secret_key(workdir):
    """The environment's S3 credentials must not be readable from SQL (duckdb_settings / duckdb_secrets)."""
    put(workdir, "flapi.yaml",
        "project-name: s3leak\ntemplate:\n  path: ./sqls\nconnections:\n  inmem:\n    init: |\n"
        "      INSTALL httpfs;\n      LOAD httpfs;\n    properties:\n      database: ':memory:'\n"
        "rate_limit:\n  enabled: false\n")
    put(workdir, "sqls/leak.yaml", "url-path: /leak\nmethod: GET\ntemplate-source: leak.sql\nconnection: [inmem]\n")
    put(workdir, "sqls/leak.sql",
        "SELECT name, value FROM duckdb_settings() WHERE name LIKE 's3_%' "
        "UNION ALL SELECT name, secret_string FROM duckdb_secrets()")
    port = free_port()
    log = open(os.path.join(workdir, "leak.log"), "w")
    proc = subprocess.Popen(
        [flapi_binary(), "-c", os.path.join(workdir, "flapi.yaml"), "-p", str(port), "--log-level", "info"],
        cwd=workdir, stdout=log, stderr=subprocess.STDOUT, preexec_fn=os.setsid,
        env={**ENV, "AWS_ACCESS_KEY_ID": "AKIALEAKCANARY", "AWS_SECRET_ACCESS_KEY": "SECRETLEAKCANARY",
             "AWS_SESSION_TOKEN": "TOKENLEAKCANARY", "AWS_REGION": "us-east-1"})
    try:
        import time
        deadline = time.time() + 90
        while time.time() < deadline:
            assert proc.poll() is None, open(os.path.join(workdir, "leak.log")).read()
            try:
                if requests.get(f"http://127.0.0.1:{port}/health/live", timeout=1).status_code == 200:
                    break
            except requests.RequestException:
                time.sleep(0.3)
        r = requests.get(f"http://127.0.0.1:{port}/leak?limit=200", timeout=30)
        assert r.status_code == 200, r.text
        assert "SECRETLEAKCANARY" not in r.text and "TOKENLEAKCANARY" not in r.text, r.text
        assert "redacted" in r.text, "the secret should be listed, redacted:\n" + r.text
    finally:
        stop(proc)
