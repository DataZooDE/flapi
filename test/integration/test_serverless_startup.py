"""Serverless startup: nothing heavy on the boot path.

A scale-to-zero platform starts a fresh container for every instance, restart and
CI run. These tests drive the real binary and check that it can start:

- without running a single cache populate query (--no-cache-warmup), while still
  saying so on /health and refusing the unbuilt endpoint with its own error;
- from a cache another process built (flapi cache refresh, then if-empty);
- without downloading any extension (pre-installed + FLAPI_EXTENSIONS_OFFLINE).

The fixture is test/integration/fixtures/offline (also used by the runtime-image CI
job with --network none).
"""
import json
import os
import shutil
import subprocess
import tempfile
import time

import pytest
import requests

from otel_helpers import flapi_binary, free_port

pytestmark = pytest.mark.standalone_server

FIXTURE = os.path.join(os.path.dirname(__file__), "fixtures", "offline")
# A proxy nobody listens on: any download attempt fails instead of succeeding.
NO_NETWORK = {"HTTP_PROXY": "http://127.0.0.1:9", "HTTPS_PROXY": "http://127.0.0.1:9",
              "http_proxy": "http://127.0.0.1:9", "https_proxy": "http://127.0.0.1:9",
              "NO_PROXY": "", "no_proxy": ""}


class Project:
    """A copy of the fixture with its own DuckLake catalog and port."""

    def __init__(self, extra_duckdb=""):
        self.dir = tempfile.mkdtemp(prefix="flapi_serverless_")
        shutil.copytree(os.path.join(FIXTURE, "sqls"), os.path.join(self.dir, "sqls"))
        shutil.copytree(os.path.join(FIXTURE, "data"), os.path.join(self.dir, "data"))
        self.port = free_port()
        self.base_url = f"http://127.0.0.1:{self.port}"
        with open(os.path.join(self.dir, "flapi.yaml"), "w") as f:
            f.write(
                "project-name: serverless-startup\n"
                "project-description: startup without data or network\n"
                f"http-port: {self.port}\n"
                "template:\n  path: ./sqls\n"
                f"duckdb:\n  db_path: ':memory:'\n{extra_duckdb}"
                "ducklake:\n  enabled: true\n  alias: cache\n"
                f"  metadata-path: {self.dir}/cache.ducklake\n"
                f"  data-path: {self.dir}/cache_data\n"
                "connections:\n  local:\n    properties:\n      path: ./data/cities.csv\n")
        self.config = os.path.join(self.dir, "flapi.yaml")
        self.proc = None

    def env(self, **extra):
        return {**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1", "FLAPI_NO_TELEMETRY": "1", **extra}

    def run(self, *args, env=None, timeout=120):
        return subprocess.run([flapi_binary(), *args], cwd=self.dir, env=env or self.env(),
                              capture_output=True, text=True, timeout=timeout)

    def start(self, *args, env=None):
        log = open(os.path.join(self.dir, "server.log"), "w")
        self.proc = subprocess.Popen([flapi_binary(), "-c", self.config, "-p", str(self.port), *args],
                                     cwd=self.dir, env=env or self.env(), stdout=log,
                                     stderr=subprocess.STDOUT)
        deadline = time.time() + 60
        while time.time() < deadline:
            if self.proc.poll() is not None:
                pytest.fail(f"server exited {self.proc.returncode}:\n{self.log()}")
            try:
                if requests.get(f"{self.base_url}/health/live", timeout=1).status_code == 200:
                    return self
            except requests.RequestException:
                pass
            time.sleep(0.2)
        pytest.fail("server did not start:\n" + self.log())

    def log(self):
        with open(os.path.join(self.dir, "server.log")) as f:
            return f.read()

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=20)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        self.proc = None

    def cleanup(self):
        self.stop()
        shutil.rmtree(self.dir, ignore_errors=True)


@pytest.fixture
def project():
    p = Project()
    yield p
    p.cleanup()


def wait_health(p, predicate, timeout=30):
    deadline = time.time() + timeout
    while time.time() < deadline:
        r = requests.get(f"{p.base_url}/health", timeout=5)
        if predicate(r):
            return r
        time.sleep(0.2)
    pytest.fail(f"/health never matched; last: {r.status_code} {r.text}")


class TestWarmupSkipped:
    def test_no_cache_warmup_starts_degraded_and_refuses_the_unbuilt_endpoint(self, project):
        project.start("--no-cache-warmup")
        r = wait_health(project, lambda r: r.status_code == 200)
        body = r.json()
        assert body["status"] == "degraded", body
        assert [c["table"] for c in body["unbuilt"]] == ["cities_cache"]

        cached = requests.get(f"{project.base_url}/cities-cached", timeout=10)
        assert cached.status_code == 503, cached.text
        assert cached.json()["error"] == "cache_not_built"

        plain = requests.get(f"{project.base_url}/cities", timeout=10)
        assert plain.status_code == 200, plain.text

    def test_env_var_is_the_fallback_and_an_invalid_value_stops_startup(self, project):
        bad = project.run("-c", project.config, "-p", str(project.port),
                          env=project.env(FLAPI_CACHE_WARMUP="sometimes"), timeout=60)
        assert bad.returncode == 1
        assert "sometimes" in (bad.stdout + bad.stderr)

        project.start(env=project.env(FLAPI_CACHE_WARMUP="never"))
        body = wait_health(project, lambda r: r.status_code == 200).json()
        assert body["status"] == "degraded"


class TestWriterThenReader:
    def test_refresh_job_then_if_empty_server_serves_without_rebuilding(self, project):
        refresh = project.run("cache", "refresh", "--config", project.config, "--all")
        assert refresh.returncode == 0, refresh.stdout + refresh.stderr
        summary = json.loads(refresh.stdout.strip().splitlines()[-1])
        assert summary["failed"] == 0
        assert [(r["table"], r["status"]) for r in summary["refreshed"]] == [("cities_cache", "ok")]

        project.start("--cache-warmup", "if-empty")
        body = wait_health(project, lambda r: r.status_code == 200).json()
        assert body["status"] == "ready", body
        rows = requests.get(f"{project.base_url}/cities-cached", timeout=10).json()
        data = rows["data"] if isinstance(rows, dict) else rows
        assert sorted(r["city"] for r in data) == ["Lyon", "Stuttgart"]

    def test_refresh_job_usage_errors_exit_2(self, project):
        assert project.run("cache", "refresh", "--config", project.config).returncode == 2
        assert project.run("cache", "refresh", "--config", project.config,
                           "--endpoint", "/nope").returncode == 2


class TestOfflineExtensions:
    def test_preinstalled_extensions_start_with_no_network(self, project):
        ext_dir = os.path.join(project.dir, "ext")
        install = project.run("extensions", "install", "--config", project.config, "--dir", ext_dir,
                              timeout=600)
        assert install.returncode == 0, install.stdout + install.stderr
        assert any(f.endswith(".duckdb_extension") for _, _, fs in os.walk(ext_dir) for f in fs)

        env = project.env(FLAPI_EXTENSION_DIRECTORY=ext_dir, FLAPI_EXTENSIONS_OFFLINE="1", **NO_NETWORK)
        project.start(env=env)
        wait_health(project, lambda r: r.status_code == 200)
        assert requests.get(f"{project.base_url}/cities", timeout=10).status_code == 200

    def test_offline_with_an_empty_directory_fails_fast_and_says_why(self, project):
        empty = os.path.join(project.dir, "empty_ext")
        os.makedirs(empty)
        env = project.env(FLAPI_EXTENSION_DIRECTORY=empty, FLAPI_EXTENSIONS_OFFLINE="1", **NO_NETWORK)
        started = time.time()
        res = project.run("-c", project.config, "-p", str(project.port), env=env, timeout=60)
        out = res.stdout + res.stderr
        assert res.returncode != 0, out
        assert "flapi extensions install" in out
        assert time.time() - started < 30
