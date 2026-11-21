"""End-to-end readiness coverage for cache warmup (issue #114)."""

import os
import signal
import socket
import subprocess
import tempfile
import time
from typing import Dict, Iterator, List

import pytest
import requests


def _repo_root() -> str:
    return os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def _flapi_binary() -> str:
    build_type = os.getenv("FLAPI_BUILD_TYPE")
    candidates: List[str] = []
    if build_type:
        candidates.append(os.path.join(_repo_root(), "build", build_type, "flapi"))
    candidates.extend(
        [
            os.path.join(_repo_root(), "build", "debug", "flapi"),
            os.path.join(_repo_root(), "build", "release", "flapi"),
        ]
    )
    for path in candidates:
        if os.path.exists(path):
            return path
    pytest.skip("flapi binary not found in build/debug or build/release")


def _free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def _write_config(dirpath: str, port: int, scheduler_enabled: bool = False, invalid_cache: bool = False,
                  populate_rows: int = 120000000) -> str:
    sqls = os.path.join(dirpath, "sqls")
    os.makedirs(sqls)

    metadata_path = os.path.join(dirpath, "cache.ducklake")
    data_path = os.path.join(dirpath, "cache_data")
    os.makedirs(data_path)

    with open(os.path.join(dirpath, "flapi.yaml"), "w") as f:
        f.write(
            f"project-name: warmup-readiness-test\n"
            f"project-description: Warmup readiness E2E\n"
            f"http-port: {port}\n"
            f"template:\n"
            f"  path: ./sqls\n"
            f"connections:\n"
            f"  inmem:\n"
            f"    properties:\n"
            f"      database: ':memory:'\n"
            f"duckdb:\n"
            f"  access_mode: READ_WRITE\n"
            f"  threads: 1\n"
            f"  max_memory: 512MB\n"
            f"ducklake:\n"
            f"  enabled: true\n"
            f"  alias: cache\n"
            f"  metadata-path: {metadata_path}\n"
            f"  data-path: {data_path}\n"
            f"  scheduler:\n"
            f"    enabled: {'true' if scheduler_enabled else 'false'}\n"
            f"    scan-interval: 1s\n"
        )

    with open(os.path.join(sqls, "cached.yaml"), "w") as f:
        f.write(
            """
url-path: /cached
method: GET
template-source: cached.sql
connection: [inmem]
cache:
  enabled: true
  table: slow_cache
  schema: cache
  schedule: 2s
  template-file: warmup.sql
"""
        )
    with open(os.path.join(sqls, "cached.sql"), "w") as f:
        f.write("SELECT * FROM {{cache.catalog}}.{{cache.schema}}.{{cache.table}}\n")
    with open(os.path.join(sqls, "warmup.sql"), "w") as f:
        if invalid_cache:
            f.write("SELECT * FROM definitely_missing_source_table\n")
        else:
            f.write(
                """
CREATE OR REPLACE TABLE {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS
SELECT
  1 AS id,
  SUM(i % 13) AS checksum
FROM range({populate_rows}) t(i);
""".replace("{populate_rows}", str(populate_rows))
            )

    with open(os.path.join(sqls, "uncached.yaml"), "w") as f:
        f.write(
            """
url-path: /uncached
method: GET
template-source: uncached.sql
connection: [inmem]
"""
        )
    with open(os.path.join(sqls, "uncached.sql"), "w") as f:
        f.write("SELECT 7 AS ok\n")

    return os.path.join(dirpath, "flapi.yaml")


def _spawn_server(config_path: str, log_path: str) -> subprocess.Popen:
    log_file = open(log_path, "w")
    proc = subprocess.Popen(
        [_flapi_binary(), "-c", config_path, "--no-telemetry", "--log-level", "debug"],
        cwd=os.path.dirname(config_path),
        stdout=log_file,
        stderr=subprocess.STDOUT,
        preexec_fn=os.setsid,
    )
    proc.log_file = log_file
    return proc


def _read_log(log_path: str) -> str:
    with open(log_path) as f:
        return f.read()


def _stop_server(proc: subprocess.Popen) -> None:
    try:
        os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
        proc.wait(timeout=10)
    except ProcessLookupError:
        pass
    except subprocess.TimeoutExpired:
        os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
        proc.wait(timeout=5)
    finally:
        proc.log_file.flush()
        proc.log_file.close()


def _wait_for_live(base_url: str, proc: subprocess.Popen, log_path: str, timeout_s: float = 10.0) -> None:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(f"flapi exited early. Log:\n{_read_log(log_path)}")
        try:
            response = requests.get(f"{base_url}/health/live", timeout=0.5)
            if response.status_code == 200:
                return
        except requests.exceptions.RequestException:
            time.sleep(0.1)
    raise RuntimeError(f"flapi did not become live. Log:\n{_read_log(log_path)}")


def _rows(body):
    return body.get("data", body.get("rows", [])) if isinstance(body, dict) else body


@pytest.fixture
def warmup_server() -> Iterator[Dict[str, str]]:
    port = _free_port()
    with tempfile.TemporaryDirectory(prefix="flapi_warmup_") as tmpdir:
        config_path = _write_config(tmpdir, port)
        log_path = os.path.join(tmpdir, "server.log")
        proc = _spawn_server(config_path, log_path)
        base_url = f"http://127.0.0.1:{port}"
        try:
            yield {"base_url": base_url, "process": proc, "log_path": log_path}
        finally:
            _stop_server(proc)


@pytest.fixture
def scheduler_warmup_server() -> Iterator[Dict[str, str]]:
    port = _free_port()
    with tempfile.TemporaryDirectory(prefix="flapi_warmup_sched_") as tmpdir:
        config_path = _write_config(tmpdir, port, scheduler_enabled=True)
        log_path = os.path.join(tmpdir, "server.log")
        proc = _spawn_server(config_path, log_path)
        base_url = f"http://127.0.0.1:{port}"
        try:
            yield {"base_url": base_url, "process": proc, "log_path": log_path}
        finally:
            _stop_server(proc)


@pytest.fixture
def failed_warmup_server() -> Iterator[Dict[str, str]]:
    port = _free_port()
    with tempfile.TemporaryDirectory(prefix="flapi_warmup_failed_") as tmpdir:
        config_path = _write_config(tmpdir, port, invalid_cache=True)
        log_path = os.path.join(tmpdir, "server.log")
        proc = _spawn_server(config_path, log_path)
        base_url = f"http://127.0.0.1:{port}"
        try:
            yield {"base_url": base_url, "process": proc, "log_path": log_path}
        finally:
            _stop_server(proc)


@pytest.mark.standalone_server
class TestWarmupReadiness:
    def test_liveness_responds_during_cache_warmup(self, warmup_server):
        _wait_for_live(warmup_server["base_url"], warmup_server["process"], warmup_server["log_path"])
        response = requests.get(f"{warmup_server['base_url']}/health/live", timeout=1)

        assert response.status_code == 200
        assert response.json()["status"] == "live"

    def test_readiness_and_cached_endpoint_are_unavailable_until_warmup_finishes(self, warmup_server):
        _wait_for_live(warmup_server["base_url"], warmup_server["process"], warmup_server["log_path"])

        readiness = requests.get(f"{warmup_server['base_url']}/health", timeout=1)
        assert readiness.status_code == 503
        assert readiness.json()["status"] == "starting"

        cached = requests.get(f"{warmup_server['base_url']}/cached", timeout=1)
        assert cached.status_code == 503
        assert cached.headers["Retry-After"] == "5"
        assert cached.json()["error"] == "cache_warming"

    def test_cached_endpoint_serves_rows_after_warmup_completes(self, warmup_server):
        _wait_for_live(warmup_server["base_url"], warmup_server["process"], warmup_server["log_path"])

        deadline = time.time() + 30
        ready = None
        while time.time() < deadline:
            ready = requests.get(f"{warmup_server['base_url']}/health", timeout=2)
            if ready.status_code == 200:
                break
            time.sleep(0.5)

        assert ready is not None
        assert ready.status_code == 200, f"readiness never became ready: {ready.text}\n{_read_log(warmup_server['log_path'])}"
        assert ready.json()["status"] == "ready"

        cached = requests.get(f"{warmup_server['base_url']}/cached", timeout=5)
        assert cached.status_code == 200, cached.text
        rows = _rows(cached.json())
        assert len(rows) == 1
        assert rows[0]["id"] == 1

    def test_failed_cache_keeps_process_live_and_endpoint_unavailable(self, failed_warmup_server):
        _wait_for_live(
            failed_warmup_server["base_url"],
            failed_warmup_server["process"],
            failed_warmup_server["log_path"],
        )

        deadline = time.time() + 10
        degraded = None
        while time.time() < deadline:
            degraded = requests.get(f"{failed_warmup_server['base_url']}/health", timeout=1)
            if degraded.status_code == 503 and degraded.json().get("status") == "degraded":
                break
            time.sleep(0.25)

        assert failed_warmup_server["process"].poll() is None
        assert degraded is not None
        assert degraded.status_code == 503, degraded.text
        body = degraded.json()
        assert body["status"] == "degraded"
        assert body["failed"][0]["table"] == "slow_cache"

        live = requests.get(f"{failed_warmup_server['base_url']}/health/live", timeout=1)
        assert live.status_code == 200

        cached = requests.get(f"{failed_warmup_server['base_url']}/cached", timeout=1)
        assert cached.status_code == 503
        # Not "cache_warming": a failed build does not finish by waiting.
        assert cached.json()["error"] == "cache_build_failed"
        assert cached.headers["Retry-After"] == "5"
        # Logged once, not once by the refresh and again by warm-up.
        assert _read_log(failed_warmup_server["log_path"]).count("Cache build failed") == 1

    def test_scheduler_does_not_refresh_during_warmup(self, scheduler_warmup_server):
        _wait_for_live(
            scheduler_warmup_server["base_url"],
            scheduler_warmup_server["process"],
            scheduler_warmup_server["log_path"],
        )

        deadline = time.time() + 30
        while time.time() < deadline:
            ready = requests.get(f"{scheduler_warmup_server['base_url']}/health", timeout=2)
            if ready.status_code == 200:
                break
            time.sleep(0.5)

        assert ready.status_code == 200, f"readiness never became ready: {ready.text}\n{_read_log(scheduler_warmup_server['log_path'])}"
        # The scheduler used to treat every cache as "never refreshed" and fire on
        # its first scan, racing the warm-up build (only the in-flight guard
        # stopped a double build). It now waits for warm-up and counts the
        # schedule from it: the first scheduled refresh comes one schedule (2 s)
        # AFTER warm-up finished.
        time.sleep(4)
        log_text = _read_log(scheduler_warmup_server["log_path"])
        assert "Skipping duplicate in-flight cache refresh" not in log_text
        finished = log_text.find("Finished warming up endpoint caches")
        first_scheduled = log_text.find("Running scheduled cache refresh")
        assert finished >= 0, log_text[-3000:]
        assert first_scheduled > finished, log_text[-3000:]


def _poll_from_bind(base_url: str, proc: subprocess.Popen, seconds: float):
    """Tight-poll /health and /cached from the first moment the port answers.

    Returns the ordered list of (health_status, cached_status, cached_error)
    triples, one per iteration that reached the server.
    """
    samples = []
    deadline = time.time() + seconds
    session = requests.Session()
    while time.time() < deadline and proc.poll() is None:
        try:
            health = session.get(f"{base_url}/health", timeout=2)
            cached = session.get(f"{base_url}/cached", timeout=2)
        except requests.exceptions.RequestException:
            continue
        try:
            error = cached.json().get("error")
        except ValueError:
            error = None
        samples.append((health.status_code, cached.status_code, error))
        if health.status_code == 200 and cached.status_code == 200:
            break
    return samples


@pytest.mark.standalone_server
class TestStartupRaceIssue198:
    """/health must never be ready while a cache is still being built, and the
    registry must never be re-initialised under a running server (#198)."""

    @pytest.mark.parametrize("attempt", range(6))
    def test_readiness_is_monotonic_from_first_bind(self, attempt):
        port = _free_port()
        with tempfile.TemporaryDirectory(prefix="flapi_race_") as tmpdir:
            config_path = _write_config(tmpdir, port, scheduler_enabled=True, populate_rows=20000000)
            log_path = os.path.join(tmpdir, "server.log")
            proc = _spawn_server(config_path, log_path)
            try:
                samples = _poll_from_bind(f"http://127.0.0.1:{port}", proc, 60)
                assert samples, _read_log(log_path)
                assert samples[-1][:2] == (200, 200), (samples[-5:], _read_log(log_path)[-3000:])
                seen_ready = False
                for health, cached, error in samples:
                    # Ready may only be reported together with a serving cache.
                    if health == 200:
                        assert cached == 200, (health, cached, error)
                    # No flicker back to warming once the cache serves.
                    if cached == 200:
                        seen_ready = True
                    elif seen_ready:
                        raise AssertionError(f"cache went back to {cached}/{error}: {samples}")
                    # The cache is only ever warming (or served): never 500, never
                    # an empty/unknown registry answer.
                    if cached != 200:
                        assert (cached, error) == (503, "cache_warming"), (cached, error)
                    if health != 200:
                        assert health == 503, health
            finally:
                _stop_server(proc)

    def test_warmup_initialises_readiness_exactly_once(self):
        port = _free_port()
        with tempfile.TemporaryDirectory(prefix="flapi_race_once_") as tmpdir:
            config_path = _write_config(tmpdir, port, populate_rows=1000000)
            log_path = os.path.join(tmpdir, "server.log")
            proc = _spawn_server(config_path, log_path)
            try:
                _wait_for_live(f"http://127.0.0.1:{port}", proc, log_path)
                deadline = time.time() + 30
                while time.time() < deadline:
                    if requests.get(f"http://127.0.0.1:{port}/health", timeout=2).status_code == 200:
                        break
                    time.sleep(0.1)
                log_text = _read_log(log_path)
                assert log_text.count("Cache readiness initialised") == 1, log_text[-3000:]
            finally:
                _stop_server(proc)
