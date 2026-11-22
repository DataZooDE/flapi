"""Keep-alive responses are not delayed by Nagle's algorithm (#241).

Crow never set TCP_NODELAY, so on a keep-alive connection every response after the first
waited ~40 ms for the client's delayed ACK. flAPI now has Crow (1.3+) set it on every accepted
socket.
"""
import shutil
import statistics
import sys
import tempfile
import time

import pytest
import requests

from test_duckdb_defaults import project
from test_duckdb_upgrade import Run
from otel_helpers import flapi_binary

pytestmark = pytest.mark.standalone_server


@pytest.fixture
def root():
    d = tempfile.mkdtemp(prefix="flapi_nodelay_")
    yield d
    shutil.rmtree(d, ignore_errors=True)


def test_keep_alive_requests_are_not_held_back_by_nagle(root):
    project(root)
    run = Run(flapi_binary(), root)
    try:
        with requests.Session() as session:
            session.get(f"{run.base}/s", timeout=10)  # warm up: first request, extensions, plan cache
            timings = []
            for _ in range(30):
                start = time.perf_counter()
                assert session.get(f"{run.base}/s", timeout=10).status_code == 200
                timings.append((time.perf_counter() - start) * 1000)
        # With Nagle + delayed ACK every one of these took ~40 ms.
        assert statistics.median(timings) < 20, timings
    finally:
        run.stop()
