"""DuckDB defaults flAPI chooses on top of DuckDB's own (#241).

DuckDB 2.0 starts 4 async I/O threads per core. Under flAPI's many short concurrent queries on local
data that pool cost ~25% throughput and ~3x memory; on uncached remote reads it is 2-3x faster than 1.5.
So flAPI uses no pool when nothing is remote and 2x the worker threads when a connection names object
storage or http(s). A `duckdb: async_threads:` in flapi.yaml always wins.
"""
import os
import shutil
import tempfile

import pytest

from test_duckdb_upgrade import Run

pytestmark = pytest.mark.standalone_server

from otel_helpers import flapi_binary


def project(root, duckdb_block="", properties="{}"):
    os.makedirs(os.path.join(root, "sqls"), exist_ok=True)
    with open(os.path.join(root, "flapi.yaml"), "w") as f:
        f.write("project-name: d\ntemplate:\n  path: ./sqls\nconnections:\n  c:\n    properties: " + properties + "\n"
                "rate_limit:\n  enabled: false\n" + duckdb_block)
    with open(os.path.join(root, "sqls", "s.yaml"), "w") as f:
        f.write("url-path: /s\nmethod: GET\ntemplate-source: s.sql\nconnection: [c]\n")
    with open(os.path.join(root, "sqls", "s.sql"), "w") as f:
        f.write("SELECT current_setting('async_threads')::INT AS async_threads")


@pytest.fixture
def root():
    d = tempfile.mkdtemp(prefix="flapi_defaults_")
    yield d
    shutil.rmtree(d, ignore_errors=True)


REMOTE = "{path: 's3://bucket/events/*.parquet'}"


@pytest.mark.parametrize("block,properties,expected", [
    ("", "{}", 0),
    ("duckdb:\n  threads: 4\n", REMOTE, 8),
    ("duckdb:\n  async_threads: 6\n", "{}", 6),
    ("duckdb:\n  async_threads: 3\n", REMOTE, 3),
], ids=["local-no-pool", "remote-2x-threads", "user-wins-local", "user-wins-remote"])
def test_async_threads(root, block, properties, expected):
    project(root, block, properties)
    run = Run(flapi_binary(), root)
    try:
        assert run.get("/s")[0]["async_threads"] == expected
    finally:
        run.stop()


@pytest.mark.skipif(not hasattr(os, "sched_setaffinity") or (os.cpu_count() or 1) < 4,
                    reason="needs Linux CPU affinity and at least 4 CPUs")
def test_threads_follow_the_cpus_the_process_may_use(root):
    """DuckDB's pool counted every host CPU, even when taskset / a cpuset / a container
    limit allows two. flAPI now sizes it (and its HTTP handler pool) to the usable CPUs."""
    import subprocess
    import time
    import requests
    from otel_helpers import free_port
    project(root)
    with open(os.path.join(root, "sqls", "s.sql"), "w") as f:
        f.write("SELECT current_setting('threads')::INT AS threads")
    port = free_port()
    proc = subprocess.Popen([flapi_binary(), "-c", "flapi.yaml", "-p", str(port)], cwd=root,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                            env={**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"},
                            preexec_fn=lambda: os.sched_setaffinity(0, {0, 1}))
    try:
        r = None
        deadline = time.time() + 60
        while time.time() < deadline:
            try:
                r = requests.get(f"http://127.0.0.1:{port}/s", timeout=2)
                break
            except requests.RequestException:
                time.sleep(0.2)
        assert r is not None, "server never answered"
        assert r.json()["data"][0]["threads"] == 2, r.text
        assert len(os.listdir(f"/proc/{proc.pid}/task")) < 40, "thread pools sized for the host, not 2 CPUs"
    finally:
        proc.terminate()
        proc.wait(timeout=20)
