"""End-to-end tests for per-user rate limiting (issue #23, W1.4).

Boots flapi with a REST endpoint configured with a very low per-user
rate limit. Two clients share the same IP but present different Bearer
tokens. With `rate-limit.key: user-or-ip`, each gets its own quota; with
the historic `ip` strategy, both would compete for one bucket.

Marked `standalone_server` so the conftest autouse fixture does not
spin up the shared api_configuration server. Skips cleanly when flapi
cannot boot (local DuckDB extension cache mismatch); CI runs against
fresh extensions.
"""

import os
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
    candidates: List[str] = []
    for build_type in ("release", "debug"):
        path = os.path.join(_repo_root(), "build", build_type, "flapi")
        if os.path.exists(path):
            candidates.append(path)
    if not candidates:
        pytest.skip("flapi binary not found in build/release or build/debug")
    candidates.sort(key=os.path.getmtime, reverse=True)
    return candidates[0]


def _free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def _write_config(dirpath: str, port: int, key_strategy: str) -> str:
    sqls = os.path.join(dirpath, "sqls")
    os.makedirs(sqls)

    with open(os.path.join(dirpath, "flapi.yaml"), "w") as f:
        f.write(
            f"project-name: per-user-rate-limit-test\n"
            f"project-description: Per-user rate limit E2E\n"
            f"http-port: {port}\n"
            f"template:\n"
            f"  path: ./sqls\n"
            f"connections:\n"
            f"  inmem:\n"
            f"    properties:\n"
            f"      database: ':memory:'\n"
        )

    # A trivial REST endpoint with a strict per-user rate limit (2 reqs / 60s).
    # The limit is tight so the test finishes fast.
    with open(os.path.join(sqls, "ping.yaml"), "w") as f:
        f.write(f"""
url-path: /ping
method: GET
template-source: ping.sql
connection: [inmem]
rate-limit:
  enabled: true
  max: 2
  interval: 60
  key: {key_strategy}
""")
    with open(os.path.join(sqls, "ping.sql"), "w") as f:
        f.write("SELECT 1 AS ok\n")

    return os.path.join(dirpath, "flapi.yaml")


def _spawn_server(binary: str, config_path: str, log_path: str) -> subprocess.Popen:
    return subprocess.Popen(
        [binary, "-c", config_path, "--no-telemetry"],
        cwd=os.path.dirname(config_path),
        stdout=open(log_path, "w"),
        stderr=subprocess.STDOUT,
    )


def _wait_for_server(proc: subprocess.Popen, base_url: str, log_path: str) -> bool:
    deadline = time.time() + 30
    while time.time() < deadline:
        if proc.poll() is not None:
            return False
        try:
            r = requests.get(f"{base_url}/ping", timeout=1)
            if r.status_code < 500:
                return True
        except requests.exceptions.RequestException:
            time.sleep(0.5)
    return False


def _maybe_skip_on_ddb_mismatch(log_path: str) -> None:
    with open(log_path) as f:
        log_text = f.read()
    if "core_functions_duckdb_cpp_init" in log_text and "unique_ptr that is NULL" in log_text:
        pytest.skip(
            "flapi could not boot: local DuckDB extension cache is "
            "incompatible with the in-tree DuckDB submodule. CI exercises this path."
        )
    raise RuntimeError(f"flapi failed to start. Log:\n{log_text}")


@pytest.fixture
def server_factory():
    """Yield a callable that starts a flapi server with a given key strategy."""
    binary = _flapi_binary()
    started: List[subprocess.Popen] = []
    tmpdirs: List[tempfile.TemporaryDirectory] = []

    def _start(key_strategy: str) -> Dict[str, str]:
        port = _free_port()
        tmp = tempfile.TemporaryDirectory(prefix="flapi_rl_")
        tmpdirs.append(tmp)
        config_path = _write_config(tmp.name, port, key_strategy)
        log_path = os.path.join(tmp.name, "server.log")
        proc = _spawn_server(binary, config_path, log_path)
        started.append(proc)
        base_url = f"http://127.0.0.1:{port}"
        if not _wait_for_server(proc, base_url, log_path):
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
            _maybe_skip_on_ddb_mismatch(log_path)
        return {"base_url": base_url}

    yield _start

    for proc in started:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
    for tmp in tmpdirs:
        tmp.cleanup()


def _hammer(base_url: str, token: str, attempts: int) -> List[int]:
    """Issue `attempts` GETs with the given Bearer token; return status codes."""
    codes: List[int] = []
    for _ in range(attempts):
        headers = {"Authorization": f"Bearer {token}"} if token else {}
        r = requests.get(f"{base_url}/ping", headers=headers, timeout=5)
        codes.append(r.status_code)
    return codes


@pytest.mark.standalone_server
class TestPerUserRateLimit:
    """End-to-end coverage for `rate-limit.key`."""

    def test_user_or_ip_isolates_two_bearer_tokens_from_same_ip(self, server_factory):
        # With `key: user-or-ip`, two clients sharing localhost but using
        # different bearer tokens MUST each have their own 2-request quota.
        # So 4 requests total succeed (2 per token), the 5th of either is 429.
        server = server_factory("user-or-ip")
        codes_a = _hammer(server["base_url"], "token-alice", attempts=3)
        codes_b = _hammer(server["base_url"], "token-bob", attempts=3)

        # First two for each token succeed (200 or 500 if DB env unhappy
        # but rate-limiter didn't trip); third should be 429.
        assert codes_a[0] != 429, f"alice's first call hit rate limit: {codes_a}"
        assert codes_a[1] != 429, f"alice's second call hit rate limit: {codes_a}"
        assert codes_a[2] == 429, f"alice's third call should be limited: {codes_a}"

        assert codes_b[0] != 429, f"bob's first call hit rate limit: {codes_b}"
        assert codes_b[1] != 429, f"bob's second call hit rate limit: {codes_b}"
        assert codes_b[2] == 429, f"bob's third call should be limited: {codes_b}"

    def test_ip_strategy_pools_two_bearer_tokens_into_one_bucket(self, server_factory):
        # With the historic `key: ip` strategy, the same 4 requests share
        # ONE bucket because the IP is identical. The third request
        # (regardless of which token sent it) must be 429.
        server = server_factory("ip")
        codes_a = _hammer(server["base_url"], "token-alice", attempts=2)
        codes_b = _hammer(server["base_url"], "token-bob", attempts=2)

        # alice's two calls fit. bob's first call is the third overall →
        # rate-limited. (bob's second is also limited.)
        assert codes_a[0] != 429, f"alice's first call: {codes_a}"
        assert codes_a[1] != 429, f"alice's second call: {codes_a}"
        assert codes_b[0] == 429, (
            f"bob's first call should be limited (3rd overall) under ip strategy: {codes_b}"
        )
