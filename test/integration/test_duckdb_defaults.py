"""DuckDB defaults flAPI chooses on top of DuckDB's own (#241).

DuckDB 2.0 starts 4 async I/O threads per core; under flAPI's many short concurrent queries that
cost ~27% throughput and ~3x memory on parquet scans, so flAPI defaults `async_threads` to 1.
A `duckdb: async_threads:` in flapi.yaml always wins.
"""
import os
import shutil
import tempfile

import pytest

from test_duckdb_upgrade import Run

pytestmark = pytest.mark.standalone_server

from otel_helpers import flapi_binary


def project(root, duckdb_block=""):
    os.makedirs(os.path.join(root, "sqls"), exist_ok=True)
    with open(os.path.join(root, "flapi.yaml"), "w") as f:
        f.write("project-name: d\ntemplate:\n  path: ./sqls\nconnections:\n  c:\n    properties: {}\n"
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


@pytest.mark.parametrize("block,expected", [("", 1), ("duckdb:\n  async_threads: 6\n", 6)],
                         ids=["flapi-default", "user-wins"])
def test_async_threads(root, block, expected):
    project(root, block)
    run = Run(flapi_binary(), root)
    try:
        assert run.get("/s")[0]["async_threads"] == expected
    finally:
        run.stop()
