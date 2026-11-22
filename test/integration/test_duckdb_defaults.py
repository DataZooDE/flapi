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
