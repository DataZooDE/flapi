"""Startup cost and idle footprint (#241).

* Extensions linked into the binary (core_functions, parquet, json, icu) are neither installed
  nor loaded: INSTALL of one downloaded its 32-51 MB loadable copy that nothing ever opened
  (84 MB and ~1.6 s on a first start for json + parquet; 123 MB more in
  `flapi extensions install` and `pack --with-extensions` with core_functions).
* A cache that warm-up would not rebuild (`warmup: never` / `if-empty`) and that exists is
  served from the first request after /health/live, not 503 cache_warming.
* A keep-alive connection opened the moment the port accepts gets TCP_NODELAY: before, only
  connections accepted after the first 50 ms tick did, and an early one paid ~40 ms
  (Nagle + delayed ACK) on every response for its lifetime.
"""
import http.client
import os
import pathlib
import shutil
import signal
import statistics
import subprocess
import tempfile
import time

import pytest
import requests

from otel_helpers import flapi_binary, free_port

pytestmark = pytest.mark.standalone_server

TOKEN = "startup-resources-token"
AUTH = {"Authorization": f"Bearer {TOKEN}"}


def write_project(root, flapi_extra="", endpoints=None):
    os.makedirs(os.path.join(root, "sqls"), exist_ok=True)
    with open(os.path.join(root, "flapi.yaml"), "w") as f:
        f.write("project-name: startup\ntemplate:\n  path: ./sqls\nconnections:\n  c:\n    properties: {}\n"
                "rate_limit:\n  enabled: false\n" + flapi_extra)
    endpoints = endpoints or {}
    endpoints.setdefault("loaded", "SELECT extension_name AS e FROM duckdb_extensions() WHERE loaded ORDER BY 1")
    for name, sql in endpoints.items():
        with open(os.path.join(root, "sqls", f"{name}.yaml"), "w") as f:
            f.write(f"url-path: /{name}\nmethod: GET\nwith-pagination: false\ntemplate-source: {name}.sql\n"
                    "connection: [c]\n")
        with open(os.path.join(root, "sqls", f"{name}.sql"), "w") as f:
            f.write(sql + "\n")


class Server:
    def __init__(self, root, env=None):
        self.port = free_port()
        self.base = f"http://127.0.0.1:{self.port}"
        self.log_path = os.path.join(root, f"server-{self.port}.log")
        self.log = open(self.log_path, "w")
        self.proc = subprocess.Popen(
            [flapi_binary(), "-c", "flapi.yaml", "-p", str(self.port), "--log-level", "info",
             "--config-service", "--config-service-token", TOKEN],
            cwd=root, stdout=self.log, stderr=subprocess.STDOUT, preexec_fn=os.setsid,
            env={**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1", **(env or {})})
        self.live_at = None
        deadline = time.time() + 90
        while time.time() < deadline:
            if self.proc.poll() is not None:
                pytest.fail("flapi exited during startup:\n" + self.output()[-4000:])
            try:
                if requests.get(f"{self.base}/health/live", timeout=1).status_code == 200:
                    self.live_at = time.time()
                    return
            except requests.RequestException:
                time.sleep(0.01)
        pytest.fail("never became live:\n" + self.output()[-4000:])

    def output(self):
        self.log.flush()
        return pathlib.Path(self.log_path).read_text()

    def get(self, path):
        r = requests.get(self.base + path, timeout=60)
        assert r.status_code == 200, f"{path}: {r.status_code} {r.text}\n{self.output()[-3000:]}"
        return r.json()

    def loaded(self):
        return {row["e"] for row in self.get("/loaded")}

    def stop(self):
        if self.proc.poll() is None:
            os.killpg(self.proc.pid, signal.SIGTERM)
            try:
                self.proc.wait(timeout=20)
            except subprocess.TimeoutExpired:
                os.killpg(self.proc.pid, signal.SIGKILL)
        self.log.close()


@pytest.fixture
def root():
    d = tempfile.mkdtemp(prefix="flapi_startup_")
    yield d
    shutil.rmtree(d, ignore_errors=True)


def extension_files(directory):
    return sorted(p.name for p in pathlib.Path(directory).rglob("*.duckdb_extension"))


def test_built_in_extensions_are_not_downloaded(root):
    """json and parquet in default-extensions are linked in: nothing is installed, and both work."""
    ext_dir = os.path.join(root, "extensions")
    os.makedirs(ext_dir)
    write_project(root, "duckdb:\n  default-extensions: [json, parquet]\n", {
        "j": "SELECT json_extract('{\"a\": 41}', '$.a')::INT + 1 AS v",
    })
    seed = Server(root, {"FLAPI_EXTENSION_DIRECTORY": ext_dir})
    try:
        assert extension_files(ext_dir) == [], "a built-in extension was downloaded"
        assert seed.get("/j") == [{"v": 42}]
        assert {"json", "parquet", "core_functions"} <= seed.loaded()
    finally:
        seed.stop()


def test_extensions_install_skips_built_in_extensions(root):
    """`flapi extensions install` reports the built-in ones and writes nothing for them."""
    write_project(root, "duckdb:\n  default-extensions: [json, parquet]\n")
    ext_dir = os.path.join(root, "installed")
    out = subprocess.run([flapi_binary(), "extensions", "install", "--config", os.path.join(root, "flapi.yaml"),
                          "--dir", ext_dir], capture_output=True, text=True, timeout=120,
                         env={**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"})
    assert out.returncode == 0, out.stdout + out.stderr
    for name in ("core_functions", "json", "parquet"):
        assert f"{name} (built into flapi" in out.stdout, out.stdout
    assert extension_files(ext_dir) == []


DUCKLAKE = ("ducklake:\n  enabled: true\n  alias: cache\n  metadata-path: ./cache.ducklake\n"
            "  data-path: ./cache_data\n  warmup: {warmup}\n")


def write_cached(root, warmup):
    write_project(root, DUCKLAKE.format(warmup=warmup))
    with open(os.path.join(root, "sqls", "t.yaml"), "w") as f:
        f.write("url-path: /t\nmethod: GET\ntemplate-source: t.sql\nconnection: [c]\n"
                "cache:\n  enabled: true\n  table: t_cache\n  schema: main\n  template-file: t_cache.sql\n")
    with open(os.path.join(root, "sqls", "t.sql"), "w") as f:
        f.write("SELECT * FROM {{{cache.catalog}}}.{{{cache.schema}}}.{{{cache.table}}} ORDER BY id\n")
    with open(os.path.join(root, "sqls", "t_cache.sql"), "w") as f:
        f.write("CREATE OR REPLACE TABLE {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS "
                "SELECT range AS id FROM range(3)\n")


@pytest.mark.parametrize("warmup", ["never", "if-empty"])
def test_an_existing_cache_is_served_right_after_live(root, warmup):
    write_cached(root, "if-empty")
    first = Server(root)
    try:
        deadline = time.time() + 60
        while requests.get(first.base + "/t", timeout=10).status_code == 503 and time.time() < deadline:
            time.sleep(0.05)
        assert "ducklake" in first.loaded()
    finally:
        first.stop()
    write_cached(root, warmup)
    again = Server(root)
    try:
        # The very first request after /health/live: the table is there, so no cache_warming.
        r = requests.get(again.base + "/t", timeout=30)
        assert r.status_code == 200, f"{r.status_code} {r.text}"
        assert [row["id"] for row in r.json()["data"]] == [0, 1, 2]
    finally:
        again.stop()


def test_a_cache_that_never_warms_up_is_unbuilt_from_the_start(root):
    write_cached(root, "never")
    server = Server(root)
    try:
        r = requests.get(server.base + "/t", timeout=30)
        assert r.status_code == 503 and r.json()["error"] == "cache_not_built", r.text
    finally:
        server.stop()


@pytest.mark.skipif(not os.path.exists("/proc/self/fd"), reason="Linux: accepted sockets inherit TCP_NODELAY")
def test_a_connection_opened_at_bind_time_has_no_nagle_delay(root):
    write_project(root)
    port = free_port()
    proc = subprocess.Popen([flapi_binary(), "-c", "flapi.yaml", "-p", str(port)], cwd=root,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, preexec_fn=os.setsid,
                            env={**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"})
    try:
        deadline = time.time() + 60
        while True:
            assert proc.poll() is None and time.time() < deadline, "flapi did not start"
            try:
                conn = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
                conn.connect()   # as soon as the port accepts: before the first 50 ms tick
                break
            except OSError:
                time.sleep(0.001)
        times = []
        for _ in range(21):
            t = time.perf_counter()
            conn.request("GET", "/health/live")
            r = conn.getresponse()
            r.read()
            times.append((time.perf_counter() - t) * 1000)
        # With Nagle on, every response after the first waits for the delayed ACK: ~40 ms.
        assert statistics.median(times[1:]) < 20, times
    finally:
        os.killpg(proc.pid, signal.SIGTERM)
        proc.wait(timeout=20)
