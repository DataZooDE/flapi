"""Test helpers clean up their scratch dirs (#263).

TracedServer writes a config, a log and an OTLP trace file under a mkdtemp dir
per server; stop() used to leave all of it in /tmp.
"""
import os

import pytest

from otel_helpers import TracedServer, traced_server

pytestmark = pytest.mark.standalone_server


def test_traced_server_stop_removes_its_temp_dir_even_if_never_started():
    s = TracedServer()
    assert os.path.isdir(s.tmp)
    s.stop()
    assert not os.path.exists(s.tmp), f"TracedServer left {s.tmp} behind"


def test_traced_server_generator_removes_its_temp_dir():
    for s in traced_server():
        tmp = s.tmp
        assert os.path.isdir(tmp)
    assert not os.path.exists(tmp), f"traced_server left {tmp} behind"
