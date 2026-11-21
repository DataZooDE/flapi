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
            # Under a remote template.path the config service no longer writes endpoints at all (501);
            # either way the local file must not become an endpoint.
            assert r.status_code in (400, 501), r.text
            assert requests.get(f"http://127.0.0.1:{port}/leak", timeout=30).status_code == 404
        finally:
            stop(proc)


class TestQueryStringBase:
    def test_a_token_on_template_path_is_sent_with_every_file(self, workdir):
        # A SAS-style token: every request must carry ?token=abc, so a join that put
        # the file name after the query ("...?token=abc/hello.yaml") would 403/404.
        files = {"/sqls/manifest.txt": "hello.yaml\n", "/sqls/hello.yaml": ENDPOINT, "/sqls/hello.sql": SQL}
        seen = []

        class Guarded(http.server.BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def do_GET(self):
                path, _, query = self.path.partition("?")
                seen.append(self.path)
                if "token=abc" not in query:
                    self.send_response(403)
                    self.end_headers()
                    return
                body = files.get(path)
                if body is None:
                    self.send_response(404)
                    self.end_headers()
                    return
                data = body.encode()
                self.send_response(200)
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            do_HEAD = do_GET

        httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Guarded)
        threading.Thread(target=httpd.serve_forever, daemon=True).start()
        try:
            put(workdir, "flapi.yaml",
                main_config(f"http://127.0.0.1:{httpd.server_address[1]}/sqls?token=abc"))
            r = run(os.path.join(workdir, "flapi.yaml"), workdir, "--validate-config")
            out = r.stdout + r.stderr
            assert r.returncode == 0 and "Parsed 1 endpoint" in out, out + "\n".join(seen)
            assert not any("?token=abc/" in p for p in seen), seen
            assert "token=abc" not in out, "the token must not be logged:\n" + out
        finally:
            httpd.shutdown()


class TestRound1Findings:
    def test_urls_with_secrets_are_never_logged_or_reported(self, workdir):
        # userinfo, a presigned query and a fragment: none may appear in logs or errors.
        put(workdir, "flapi.yaml", main_config("./sqls"))
        url = "http://USERCANARY:PASSCANARY@127.0.0.1:9/flapi.yaml?X-Amz-Signature=SIGCANARY#FRAGCANARY"
        r = subprocess.run([flapi_binary(), "-c", url, "--validate-config", "--log-level", "debug"],
                           cwd=workdir, capture_output=True, text=True, timeout=120, env=ENV)
        out = r.stdout + r.stderr
        assert r.returncode != 0, out
        for canary in ("USERCANARY", "PASSCANARY", "SIGCANARY", "FRAGCANARY"):
            assert canary not in out, f"{canary} leaked:\n{out}"

    def test_missing_template_error_does_not_leak_the_token(self, workdir):
        files = {"/sqls/manifest.txt": "hello.yaml\n", "/sqls/hello.yaml": ENDPOINT}

        class Guarded(http.server.BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def do_GET(self):
                path, _, query = self.path.partition("?")
                body = files.get(path) if "sig=SIGCANARY" in query else None
                self.send_response(200 if body else 404)
                data = (body or "").encode()
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            do_HEAD = do_GET

        httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Guarded)
        threading.Thread(target=httpd.serve_forever, daemon=True).start()
        try:
            put(workdir, "flapi.yaml", main_config(f"http://127.0.0.1:{httpd.server_address[1]}/sqls?sig=SIGCANARY"))
            r = run(os.path.join(workdir, "flapi.yaml"), workdir, "--validate-config")
            out = r.stdout + r.stderr
            assert r.returncode != 0 and "does not exist" in out, out
            assert "SIGCANARY" not in out, out
        finally:
            httpd.shutdown()

    def test_a_relative_template_path_keeps_the_config_urls_token(self, workdir):
        files = {"/flapi.yaml": main_config("./sqls"), "/sqls/manifest.txt": "hello.yaml\n",
                 "/sqls/hello.yaml": ENDPOINT, "/sqls/hello.sql": SQL}

        class Guarded(http.server.BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def do_GET(self):
                path, _, query = self.path.partition("?")
                body = files.get(path) if "sig=abc" in query else None
                self.send_response(200 if body else 403)
                data = (body or "").encode()
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            do_HEAD = do_GET

        httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Guarded)
        threading.Thread(target=httpd.serve_forever, daemon=True).start()
        try:
            r = run(f"http://127.0.0.1:{httpd.server_address[1]}/flapi.yaml?sig=abc", workdir, "--validate-config")
            out = r.stdout + r.stderr
            assert r.returncode == 0 and "Parsed 1 endpoint" in out, out
        finally:
            httpd.shutdown()

    @pytest.mark.parametrize("sneaky", [".%2e/x.yaml", "%2e./x.yaml", "..%2fb", "..\\x.yaml", "a/%2E%2e/b.yaml", "sub/.."])
    def test_dot_dot_spellings_are_refused_in_a_manifest(self, store, workdir, sneaky):
        root, base = store
        put(root, "sqls/manifest.txt", sneaky + "\n")
        put(workdir, "flapi.yaml", main_config(f"{base}/sqls"))
        r = run(os.path.join(workdir, "flapi.yaml"), workdir, "--validate-config")
        out = r.stdout + r.stderr
        assert r.returncode != 0 and "escapes template.path" in out, out


class TestStorageCache:
    def _serve_counting(self, files):
        hits = []

        class Counting(http.server.BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def do_GET(self):
                hits.append((self.command, self.path))
                body = files.get(self.path.partition("?")[0])
                self.send_response(200 if body is not None else 404)
                data = (body or "").encode()
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                if self.command != "HEAD":
                    self.wfile.write(data)

            do_HEAD = do_GET

        httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Counting)
        threading.Thread(target=httpd.serve_forever, daemon=True).start()
        return httpd, hits

    def _requests_for_template(self, workdir, cache_block):
        files = {"/sqls/manifest.txt": "hello.yaml\n", "/sqls/hello.yaml": ENDPOINT, "/sqls/hello.sql": SQL}
        httpd, hits = self._serve_counting(files)
        try:
            put(workdir, "flapi.yaml",
                main_config(f"http://127.0.0.1:{httpd.server_address[1]}/sqls") + cache_block)
            proc, port, output = serve(os.path.join(workdir, "flapi.yaml"), workdir)
            try:
                before = len([h for h in hits if h[1].endswith("hello.sql")])
                for _ in range(6):
                    assert requests.get(f"http://127.0.0.1:{port}/remote-hello", timeout=30).status_code == 200
                self.last_hits = [h for h in hits if h[1].endswith("hello.sql")][before:]
                return len(self.last_hits)
            finally:
                stop(proc)
        finally:
            httpd.shutdown()

    def test_storage_cache_saves_round_trips(self, workdir):
        uncached = self._requests_for_template(workdir, "storage:\n  cache:\n    enabled: false\n")
        cached = self._requests_for_template(workdir, "storage:\n  cache:\n    enabled: true\n    ttl: 300\n")
        assert uncached >= 6, uncached
        assert cached < uncached / 2, f"cached={cached} uncached={uncached}"


class TestConfigServiceWithRemoteTemplates:
    def _boot(self, store, workdir):
        root, base = store
        put(root, "sqls/hello.yaml", ENDPOINT)
        put(root, "sqls/hello.sql", SQL)
        put(root, "sqls/manifest.txt", "hello.yaml\n")
        put(workdir, "flapi.yaml", main_config(f"{base}/sqls"))
        port = free_port()
        log = open(os.path.join(workdir, "cs.log"), "w")
        proc = subprocess.Popen(
            [flapi_binary(), "-c", os.path.join(workdir, "flapi.yaml"), "-p", str(port), "--log-level", "info",
             "--config-service", "--config-service-token", "tok"],
            cwd=workdir, stdout=log, stderr=subprocess.STDOUT, preexec_fn=os.setsid, env=ENV)
        deadline = time.time() + 90
        while time.time() < deadline:
            try:
                if requests.get(f"http://127.0.0.1:{port}/health/live", timeout=1).status_code == 200:
                    break
            except requests.RequestException:
                time.sleep(0.3)
        return proc, f"http://127.0.0.1:{port}/api/v1/_config/endpoints", {"Authorization": "Bearer tok"}, base

    def test_get_template_reads_it_from_remote_storage(self, store, workdir):
        proc, api, auth, base = self._boot(store, workdir)
        try:
            r = requests.get(f"{api}/-remote~1hello/template", headers=auth, timeout=30)
            assert r.status_code == 200, r.text
            assert r.json()["template"] == SQL, r.text
        finally:
            stop(proc)

    def test_put_template_is_refused_and_writes_nothing_locally(self, store, workdir):
        proc, api, auth, base = self._boot(store, workdir)
        try:
            r = requests.put(f"{api}/-remote~1hello/template", json={"template": "SELECT 1"}, headers=auth, timeout=30)
            assert r.status_code == 501, r.text
            assert "remote" in r.text.lower(), r.text
            leftovers = [n for n in os.listdir(workdir) if n.startswith("http")]
            assert leftovers == [], f"a junk local file was written: {leftovers}"
        finally:
            stop(proc)

    def test_by_template_finds_a_remote_endpoint(self, store, workdir):
        proc, api, auth, base = self._boot(store, workdir)
        try:
            r = requests.post(f"{api}/by-template", json={"template_path": f"{base}/sqls/hello.sql"},
                              headers=auth, timeout=30)
            assert r.status_code == 200, r.text
            assert r.json().get("count") == 1, r.text
        finally:
            stop(proc)


def test_relative_ducklake_paths_in_a_remote_config_resolve_against_the_cwd(store, workdir):
    root, base = store
    put(root, "flapi.yaml", main_config("./sqls") + (
        "ducklake:\n  enabled: true\n  alias: lake\n  metadata-path: ./meta.ducklake\n"
        "  data-path: ./lake_data\n  warmup: never\n"))
    put(root, "sqls/hello.yaml", ENDPOINT)
    put(root, "sqls/hello.sql", SQL)
    put(root, "sqls/manifest.txt", "hello.yaml\n")
    proc, port, output = serve(f"{base}/flapi.yaml", workdir)
    try:
        assert requests.get(f"http://127.0.0.1:{port}/remote-hello", timeout=30).status_code == 200
    finally:
        stop(proc)
    assert os.path.exists(os.path.join(workdir, "meta.ducklake")), os.listdir(workdir)
    assert not [n for n in os.listdir(workdir) if n.startswith("http")], os.listdir(workdir)


class TestRemoteWritesAndRedactionInTheConfigService:
    def _guarded(self, files, token):
        class Guarded(http.server.BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def do_GET(self):
                path, _, query = self.path.partition("?")
                body = files.get(path) if f"token={token}" in query else None
                self.send_response(200 if body is not None else 403)
                data = (body or "").encode()
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                if self.command != "HEAD":
                    self.wfile.write(data)

            do_HEAD = do_GET

        httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Guarded)
        threading.Thread(target=httpd.serve_forever, daemon=True).start()
        return httpd

    def _boot(self, workdir, template_path):
        put(workdir, "flapi.yaml", main_config(template_path))
        port = free_port()
        log = open(os.path.join(workdir, "cs2.log"), "w")
        proc = subprocess.Popen(
            [flapi_binary(), "-c", os.path.join(workdir, "flapi.yaml"), "-p", str(port), "--log-level", "info",
             "--config-service", "--config-service-token", "tok"],
            cwd=workdir, stdout=log, stderr=subprocess.STDOUT, preexec_fn=os.setsid, env=ENV)
        deadline = time.time() + 90
        while time.time() < deadline:
            try:
                if requests.get(f"http://127.0.0.1:{port}/health/live", timeout=1).status_code == 200:
                    break
            except requests.RequestException:
                time.sleep(0.3)
        return proc, f"http://127.0.0.1:{port}/api/v1/_config", {"Authorization": "Bearer tok"}

    def test_writes_are_refused_with_501_and_leave_no_local_files(self, store, workdir):
        root, base = store
        put(root, "sqls/hello.yaml", ENDPOINT)
        put(root, "sqls/hello.sql", SQL)
        put(root, "sqls/manifest.txt", "hello.yaml\n")
        proc, api, auth = self._boot(workdir, f"{base}/sqls")
        try:
            new = {"url-path": "/new", "method": "GET", "template-source": "new.sql", "connection": ["inmem"]}
            for method, url, body in [
                ("post", f"{api}/endpoints", new),
                ("put", f"{api}/endpoints/-remote~1hello", {"method": "GET", "connection": ["inmem"]}),
                ("delete", f"{api}/endpoints/-remote~1hello", None),
                ("post", f"{api}/endpoints/-remote~1hello/reload", None),
            ]:
                r = getattr(requests, method)(url, json=body, headers=auth, timeout=30) if body is not None \
                    else getattr(requests, method)(url, headers=auth, timeout=30)
                assert r.status_code == 501, (method, url, r.status_code, r.text)
                assert "restart" in r.text.lower(), r.text
            assert requests.get(f"http://{api.split('/')[2]}/remote-hello", timeout=30).status_code == 200
            assert not [n for n in os.listdir(workdir) if n.startswith(("http", "s3"))], os.listdir(workdir)
        finally:
            stop(proc)

    def test_config_service_json_does_not_leak_the_url_token(self, workdir):
        files = {"/sqls/manifest.txt": "hello.yaml\n", "/sqls/hello.yaml": ENDPOINT, "/sqls/hello.sql": SQL}
        httpd = self._guarded(files, "SECRETTOK123")
        try:
            proc, api, auth = self._boot(
                workdir, f"http://127.0.0.1:{httpd.server_address[1]}/sqls?token=SECRETTOK123")
            try:
                for path in ["/project", "/endpoints", "/endpoints/-remote~1hello", "/filesystem"]:
                    r = requests.get(api + path, headers=auth, timeout=30)
                    assert "SECRETTOK123" not in r.text, f"{path} leaked the token: {r.text[:300]}"
            finally:
                stop(proc)
        finally:
            httpd.shutdown()

    def test_an_absolute_include_in_a_remote_yaml_is_refused(self, store, workdir):
        root, base = store
        put(root, "sqls/hello.yaml", ENDPOINT + "description: |\n  {{include from /etc/os-release}}\n")
        put(root, "sqls/hello.sql", SQL)
        put(root, "sqls/manifest.txt", "hello.yaml\n")
        put(workdir, "flapi.yaml", main_config(f"{base}/sqls"))
        r = run(os.path.join(workdir, "flapi.yaml"), workdir, "--validate-config")
        out = r.stdout + r.stderr
        assert r.returncode != 0, out
        assert "Could not resolve include path" in out, out
        assert "PRETTY_NAME" not in out, "the local file must not be read"


class TestOperations:
    def test_sigterm_after_remote_reads_exits_cleanly(self, store, workdir):
        # Regression: a SIGABRT ("corrupted double-linked list") on shutdown in about 1 of 5 runs
        # once a request had been served from remote storage.
        root, base = store
        put(root, "sqls/hello.yaml", ENDPOINT)
        put(root, "sqls/hello.sql", SQL)
        put(root, "sqls/manifest.txt", "hello.yaml\n")
        put(workdir, "flapi.yaml", main_config(f"{base}/sqls"))
        codes = []
        for _ in range(8):
            port = free_port()
            log = open(os.path.join(workdir, "term.log"), "w")
            proc = subprocess.Popen(
                [flapi_binary(), "-c", os.path.join(workdir, "flapi.yaml"), "-p", str(port), "--log-level", "info"],
                cwd=workdir, stdout=log, stderr=subprocess.STDOUT, preexec_fn=os.setsid, env=ENV)
            deadline = time.time() + 90
            while time.time() < deadline:
                try:
                    if requests.get(f"http://127.0.0.1:{port}/health/live", timeout=1).status_code == 200:
                        break
                except requests.RequestException:
                    time.sleep(0.2)
            requests.get(f"http://127.0.0.1:{port}/remote-hello", timeout=30)
            os.killpg(proc.pid, signal.SIGTERM)
            try:
                codes.append(proc.wait(timeout=60))
            except subprocess.TimeoutExpired:
                os.killpg(proc.pid, signal.SIGKILL)
                codes.append("hang")
        assert codes == [0] * 8, codes

    def test_a_template_is_served_stale_when_the_store_fails_after_the_ttl(self, workdir):
        state = {"down": False}
        files = {"/sqls/manifest.txt": "hello.yaml\n", "/sqls/hello.yaml": ENDPOINT, "/sqls/hello.sql": SQL}

        class Flaky(http.server.BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def do_GET(self):
                body = files.get(self.path.partition("?")[0])
                if state["down"] and self.path.endswith("hello.sql"):
                    self.send_response(500)
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                self.send_response(200 if body is not None else 404)
                data = (body or "").encode()
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                if self.command != "HEAD":
                    self.wfile.write(data)

            do_HEAD = do_GET

        httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Flaky)
        threading.Thread(target=httpd.serve_forever, daemon=True).start()
        try:
            put(workdir, "flapi.yaml", main_config(f"http://127.0.0.1:{httpd.server_address[1]}/sqls") +
                "storage:\n  cache:\n    enabled: true\n    ttl: 1\n")
            proc, port, output = serve(os.path.join(workdir, "flapi.yaml"), workdir)
            try:
                assert requests.get(f"http://127.0.0.1:{port}/remote-hello", timeout=30).status_code == 200
                state["down"] = True
                time.sleep(1.5)  # the cached copy is now past its TTL and the store answers 500
                r = requests.get(f"http://127.0.0.1:{port}/remote-hello", timeout=30)
                assert r.status_code == 200, r.text + output()[-1500:]
                assert "serving the cached copy past its TTL" in output(), output()[-1500:]
            finally:
                stop(proc)
        finally:
            httpd.shutdown()

    def test_a_store_error_for_a_template_shows_its_cause_not_not_found(self, workdir):
        state = {"down": False}
        files = {"/sqls/manifest.txt": "hello.yaml\n", "/sqls/hello.yaml": ENDPOINT, "/sqls/hello.sql": SQL}

        class Flaky(http.server.BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def do_GET(self):
                body = files.get(self.path.partition("?")[0])
                if state["down"] and self.path.endswith("hello.sql"):
                    self.send_response(403)
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                self.send_response(200 if body is not None else 404)
                data = (body or "").encode()
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                if self.command != "HEAD":
                    self.wfile.write(data)

            do_HEAD = do_GET

        httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Flaky)
        threading.Thread(target=httpd.serve_forever, daemon=True).start()
        try:
            put(workdir, "flapi.yaml", main_config(f"http://127.0.0.1:{httpd.server_address[1]}/sqls") +
                "storage:\n  cache:\n    enabled: false\n")
            proc, port, output = serve(os.path.join(workdir, "flapi.yaml"), workdir)
            try:
                state["down"] = True
                r = requests.get(f"http://127.0.0.1:{port}/remote-hello", timeout=30)
                assert r.status_code == 500, r.text
                log = output()
                assert "Could not read template" in log and "HTTP GET error" in log, log[-1500:]
                assert "Template file not found" not in log, log[-1500:]
            finally:
                stop(proc)
        finally:
            httpd.shutdown()
