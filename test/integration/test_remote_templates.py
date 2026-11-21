"""Remote `template.path` and a remote `--config` (#219, #198).

`template.path: s3://...` was joined onto the config directory and run through
`lexically_normal`, which turned it into `<cwd>/s3:/bucket/...`: the server looked
for a directory with that name and stopped. `--config http://...` could not even
construct its loader. Both needed a DuckDB (httpfs) before DatabaseManager exists.

A local http.server stands in for the object store. Plain http(s) has no listing,
so the template directory carries a `manifest.txt` (one endpoint YAML per line,
relative to template.path); s3/gs/az are listed with a glob and use the manifest
only when the glob finds nothing.
"""
import functools
import http.server
import os
import shutil
import signal
import subprocess
import tempfile
import threading
import time

import pytest
import requests

from otel_helpers import flapi_binary, free_port

pytestmark = pytest.mark.standalone_server

ENV = {**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"}

ENDPOINT = """\
url-path: /remote-hello
method: GET
template-source: hello.sql
connection: [inmem]
"""
SQL = "SELECT 42 AS answer, 'from-remote' AS origin"


class _Quiet(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *args):
        pass


@pytest.fixture
def store():
    """Serve a directory over http; yields (dir, base_url)."""
    d = tempfile.mkdtemp(prefix="flapi_remote_")
    handler = functools.partial(_Quiet, directory=d)
    httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    thread = threading.Thread(target=httpd.serve_forever, daemon=True)
    thread.start()
    yield d, f"http://127.0.0.1:{httpd.server_address[1]}"
    httpd.shutdown()
    shutil.rmtree(d, ignore_errors=True)


def put(root, rel, body):
    path = os.path.join(root, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(body)


def main_config(template_path):
    return f"""\
project-name: remote-templates
template:
  path: {template_path}
connections:
  inmem:
    properties:
      database: ':memory:'
rate_limit:
  enabled: false
"""


def run(config, cwd, *extra, timeout=120):
    return subprocess.run(
        [flapi_binary(), "-c", config, "--log-level", "info", *extra],
        cwd=cwd, capture_output=True, text=True, timeout=timeout, env=ENV)


def serve(config, cwd):
    port = free_port()
    log_path = os.path.join(cwd, f"server-{port}.log")
    log = open(log_path, "w")
    proc = subprocess.Popen(
        [flapi_binary(), "-c", config, "-p", str(port), "--log-level", "info"],
        cwd=cwd, stdout=log, stderr=subprocess.STDOUT, preexec_fn=os.setsid, env=ENV)

    def output():
        log.flush()
        return open(log_path).read()

    deadline = time.time() + 90
    while time.time() < deadline:
        if proc.poll() is not None:
            pytest.fail("server exited:\n" + output())
        try:
            if requests.get(f"http://127.0.0.1:{port}/health/live", timeout=1).status_code == 200:
                return proc, port, output
        except requests.RequestException:
            time.sleep(0.3)
    os.killpg(proc.pid, signal.SIGKILL)
    pytest.fail("server never became live:\n" + output())


def stop(proc):
    if proc.poll() is None:
        os.killpg(proc.pid, signal.SIGTERM)
        try:
            proc.wait(timeout=20)
        except subprocess.TimeoutExpired:
            os.killpg(proc.pid, signal.SIGKILL)


@pytest.fixture
def workdir():
    d = tempfile.mkdtemp(prefix="flapi_remote_work_")
    yield d
    shutil.rmtree(d, ignore_errors=True)


class TestRemoteTemplatePath:
    def test_validate_config_reads_templates_from_http(self, store, workdir):
        root, base = store
        put(root, "sqls/hello.yaml", ENDPOINT)
        put(root, "sqls/hello.sql", SQL)
        put(root, "sqls/manifest.txt", "# endpoints\nhello.yaml\n")
        cfg = os.path.join(workdir, "flapi.yaml")
        put(workdir, "flapi.yaml", main_config(f"{base}/sqls"))

        r = run(cfg, workdir, "--validate-config")
        out = r.stdout + r.stderr
        assert r.returncode == 0, out
        assert "Parsed 1 endpoint" in out, out

    def test_server_serves_an_endpoint_whose_templates_are_remote(self, store, workdir):
        root, base = store
        put(root, "sqls/hello.yaml", ENDPOINT)
        put(root, "sqls/hello.sql", SQL)
        put(root, "sqls/manifest.txt", "hello.yaml\n")
        put(workdir, "flapi.yaml", main_config(f"{base}/sqls"))

        proc, port, output = serve(os.path.join(workdir, "flapi.yaml"), workdir)
        try:
            r = requests.get(f"http://127.0.0.1:{port}/remote-hello", timeout=30)
            assert r.status_code == 200, r.text + output()[-2000:]
            row = r.json()["data"][0]
            assert row == {"answer": 42, "origin": "from-remote"}, row
        finally:
            stop(proc)

    def test_a_missing_manifest_names_what_to_add(self, store, workdir):
        root, base = store
        put(root, "sqls/hello.yaml", ENDPOINT)
        put(root, "sqls/hello.sql", SQL)  # no manifest.txt
        put(workdir, "flapi.yaml", main_config(f"{base}/sqls"))

        r = run(os.path.join(workdir, "flapi.yaml"), workdir, "--validate-config")
        out = r.stdout + r.stderr
        assert r.returncode != 0, out
        assert "manifest.txt" in out, f"should say a plain http template dir needs a manifest:\n{out}"
        assert "s3:/" not in out.replace("s3://", ""), out

    def test_s3_template_path_is_not_mangled_into_a_local_path(self, workdir):
        # The reported failure: `<cwd>/s3:/my-bucket/...`. Nothing is listening at
        # the endpoint, so the load must fail - but naming the s3:// URL.
        put(workdir, "flapi.yaml", main_config("s3://my-bucket/api-templates/sqls"))
        r = subprocess.run(
            [flapi_binary(), "-c", os.path.join(workdir, "flapi.yaml"), "--validate-config",
             "--log-level", "info"],
            cwd=workdir, capture_output=True, text=True, timeout=120,
            env={**ENV, "AWS_ENDPOINT_URL": "http://127.0.0.1:9", "AWS_ACCESS_KEY_ID": "x",
                 "AWS_SECRET_ACCESS_KEY": "y", "AWS_REGION": "us-east-1"})
        out = r.stdout + r.stderr
        assert r.returncode != 0, out
        assert "s3://my-bucket/api-templates/sqls" in out, out
        assert "s3:/my-bucket" not in out.replace("s3://my-bucket", ""), out


class TestRemoteConfig:
    def test_boots_from_an_http_config_url(self, store, workdir):
        root, base = store
        put(root, "flapi.yaml", main_config("./sqls"))
        put(root, "sqls/hello.yaml", ENDPOINT)
        put(root, "sqls/hello.sql", SQL)
        put(root, "sqls/manifest.txt", "hello.yaml\n")

        proc, port, output = serve(f"{base}/flapi.yaml", workdir)
        try:
            r = requests.get(f"http://127.0.0.1:{port}/remote-hello", timeout=30)
            assert r.status_code == 200, r.text + output()[-2000:]
            assert r.json()["data"][0]["origin"] == "from-remote"
        finally:
            stop(proc)


class TestBootstrapExtensions:
    def test_offline_bootstrap_does_not_download_httpfs(self, store, workdir):
        # The bootstrap DuckDB reads a remote --config before the config exists, so
        # it follows FLAPI_EXTENSION_DIRECTORY / FLAPI_EXTENSIONS_OFFLINE from the
        # environment. Offline with an empty directory it must not INSTALL: it
        # reports the failed LOAD (naming the directory) instead of downloading.
        root, base = store
        put(root, "flapi.yaml", main_config("./sqls"))
        ext_dir = os.path.join(workdir, "ext")
        os.makedirs(ext_dir)
        env = {**ENV, "FLAPI_EXTENSION_DIRECTORY": ext_dir, "FLAPI_EXTENSIONS_OFFLINE": "1"}
        r = subprocess.run(
            [flapi_binary(), "-c", f"{base}/flapi.yaml", "--validate-config", "--log-level", "info"],
            cwd=workdir, capture_output=True, text=True, timeout=120, env=env)
        out = r.stdout + r.stderr
        assert "Bootstrap DuckDB: `LOAD httpfs` failed" in out, out
        assert ext_dir in out, out
        assert "FLAPI_EXTENSION_DIRECTORY" in out, out
        files = [f for _, _, names in os.walk(ext_dir) for f in names]
        assert files == [], f"offline mode must not download anything: {files}"


class TestReviewFindings:
    def test_manifest_cannot_point_outside_template_path(self, store, workdir):
        root, base = store
        put(root, "sqls/hello.sql", SQL)
        put(root, "sqls/manifest.txt", f"{base}/other/evil.yaml\n")
        put(workdir, "flapi.yaml", main_config(f"{base}/sqls"))
        r = run(os.path.join(workdir, "flapi.yaml"), workdir, "--validate-config")
        out = r.stdout + r.stderr
        assert r.returncode != 0, out
        assert "outside template.path" in out, out

    def test_manifest_name_containing_two_dots_is_allowed(self, store, workdir):
        root, base = store
        put(root, "sqls/a..b.yaml", ENDPOINT)
        put(root, "sqls/hello.sql", SQL)
        put(root, "sqls/manifest.txt", "a..b.yaml\n")
        put(workdir, "flapi.yaml", main_config(f"{base}/sqls"))
        r = run(os.path.join(workdir, "flapi.yaml"), workdir, "--validate-config")
        assert r.returncode == 0, r.stdout + r.stderr

    def test_a_chunked_response_is_not_read_as_empty(self, workdir):
        # No Content-Length: the size reads as 0, the content must still arrive.
        class Chunked(http.server.BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def do_GET(self):
                body = {"/flapi.yaml": main_config("./sqls"), "/sqls/manifest.txt": "hello.yaml\n",
                        "/sqls/hello.yaml": ENDPOINT, "/sqls/hello.sql": SQL}.get(self.path)
                if body is None:
                    self.send_response(404)
                    self.end_headers()
                    return
                self.send_response(200)
                self.send_header("Transfer-Encoding", "chunked")
                self.end_headers()
                data = body.encode()
                self.wfile.write(f"{len(data):x}\r\n".encode() + data + b"\r\n0\r\n\r\n")

            do_HEAD = do_GET

        httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Chunked)
        threading.Thread(target=httpd.serve_forever, daemon=True).start()
        try:
            r = run(f"http://127.0.0.1:{httpd.server_address[1]}/flapi.yaml", workdir, "--validate-config")
            out = r.stdout + r.stderr
            assert r.returncode == 0 and "Parsed 1 endpoint" in out, out
        finally:
            httpd.shutdown()

    def test_listing_error_is_readable_and_secret_free(self, workdir):
        put(workdir, "flapi.yaml", main_config("s3://my-bucket/sqls"))
        r = subprocess.run(
            [flapi_binary(), "-c", os.path.join(workdir, "flapi.yaml"), "--validate-config", "--log-level", "info"],
            cwd=workdir, capture_output=True, text=True, timeout=120,
            env={**ENV, "AWS_ENDPOINT_URL": "http://127.0.0.1:9", "AWS_ACCESS_KEY_ID": "AKIATOPSECRET",
                 "AWS_SECRET_ACCESS_KEY": "supersecretvalue", "AWS_REGION": "us-east-1"})
        out = r.stdout + r.stderr
        assert "AKIATOPSECRET" not in out and "supersecretvalue" not in out, out
        assert '"exception_type"' not in out, "raw DuckDB JSON should be reduced to its message:\n" + out

    def test_reload_file_with_a_remote_url_is_refused_not_unloaded(self, store, workdir):
        root, base = store
        put(root, "sqls/hello.yaml", ENDPOINT)
        put(root, "sqls/hello.sql", SQL)
        put(root, "sqls/manifest.txt", "hello.yaml\n")
        put(workdir, "flapi.yaml", main_config(f"{base}/sqls"))
        port = free_port()
        log = open(os.path.join(workdir, "s.log"), "w")
        proc = subprocess.Popen(
            [flapi_binary(), "-c", os.path.join(workdir, "flapi.yaml"), "-p", str(port), "--log-level", "info",
             "--config-service", "--config-service-token", "tok"],
            cwd=workdir, stdout=log, stderr=subprocess.STDOUT, preexec_fn=os.setsid, env=ENV)
        try:
            deadline = time.time() + 90
            while time.time() < deadline:
                try:
                    if requests.get(f"http://127.0.0.1:{port}/health/live", timeout=1).status_code == 200:
                        break
                except requests.RequestException:
                    time.sleep(0.3)
            r = requests.post(f"http://127.0.0.1:{port}/api/v1/_config/endpoints/reload-file",
                              json={"file": f"{base}/sqls/hello.yaml"},
                              headers={"Authorization": "Bearer tok"}, timeout=30)
            assert r.status_code == 501, r.text
            assert "remote" in r.text, r.text
            # and the endpoint is still served
            assert requests.get(f"http://127.0.0.1:{port}/remote-hello", timeout=30).status_code == 200
        finally:
            stop(proc)

    def test_a_local_absolute_template_source_is_refused_under_a_remote_template_path(self, store, workdir):
        # With template.path remote, `/etc/hostname` used to pass the containment check and
        # was then read from the LOCAL disk by anyone allowed to create endpoints.
        root, base = store
        put(root, "sqls/hello.yaml", ENDPOINT)
        put(root, "sqls/hello.sql", SQL)
        put(root, "sqls/manifest.txt", "hello.yaml\n")
        put(workdir, "flapi.yaml", main_config(f"{base}/sqls"))
        port = free_port()
        log = open(os.path.join(workdir, "s2.log"), "w")
        proc = subprocess.Popen(
            [flapi_binary(), "-c", os.path.join(workdir, "flapi.yaml"), "-p", str(port), "--log-level", "info",
             "--config-service", "--config-service-token", "tok"],
            cwd=workdir, stdout=log, stderr=subprocess.STDOUT, preexec_fn=os.setsid, env=ENV)
        try:
            deadline = time.time() + 90
            while time.time() < deadline:
                try:
                    if requests.get(f"http://127.0.0.1:{port}/health/live", timeout=1).status_code == 200:
                        break
                except requests.RequestException:
                    time.sleep(0.3)
            r = requests.post(f"http://127.0.0.1:{port}/api/v1/_config/endpoints",
                              json={"url-path": "/leak", "method": "GET", "template-source": "/etc/hostname",
                                    "connection": ["inmem"]},
                              headers={"Authorization": "Bearer tok"}, timeout=30)
            assert r.status_code >= 400, r.text
            assert "outside the templates directory" in r.text, r.text
            assert requests.get(f"http://127.0.0.1:{port}/leak", timeout=30).status_code == 404
        finally:
            stop(proc)
