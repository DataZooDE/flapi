"""A startup failure must exit non-zero, and must not abort (#145).

Two sites, both one screen below the #126 configuration catch:

1. `initializeDatabase` rethrows as std::runtime_error and nothing above it
   caught, so the throw escaped main, reached terminateHandler and hit
   std::abort(). Measured against `examples/flapi.yaml` with a second
   instance that could not take the DuckLake file lock: exit 134 (SIGABRT)
   plus a core dump of the ~77 MB binary. That is the most likely failure in
   a restart loop - the previous process has not released the cache file yet -
   so a crash-looping container filled its disk with core dumps of a
   recoverable, operator-fixable condition.

2. A bind failure (EADDRINUSE) was caught inside the server thread, logged,
   and then main fell through to `return 0`. A supervisor cannot distinguish
   "served and shut down cleanly" from "never started" when both exit 0.

Everything here uses temp dirs and ephemeral ports: no fixed port, and
nothing under examples/ is touched.
"""
import os
import signal
import subprocess
import tempfile
import time

import pytest
import requests

from otel_helpers import flapi_binary, free_port

pytestmark = pytest.mark.standalone_server

# 128 + SIGABRT(6) as a shell reports it. Python's subprocess reports a
# signal death as the NEGATIVE signal number instead, so both spellings are
# checked - and a negative return code is also the portable way to say "this
# process was killed by a signal", i.e. the state in which a core dump is
# written at all. The kernel's core_pattern decides whether a file lands in
# cwd or goes to a collector, so the exit status is the assertable part.
ABORT_EXIT_CODES = (134, -int(signal.SIGABRT))

# Generous enough for a cold binary on a loaded CI box, far below the
# subprocess timeouts: a lock conflict is detected before any query runs, so
# a startup failure that takes this long is a hang, not slowness.
PROMPT_EXIT_SECONDS = 30


def _write_config(tmp, port, ducklake_dir):
    """A minimal config with DuckLake on, so the process takes a file lock.

    `ducklake_dir` is separate from `tmp` so two instances can share a lock
    while using different config directories - or not share one, which is
    what the bind test needs.
    """
    sqls = os.path.join(tmp, "sqls")
    os.makedirs(sqls, exist_ok=True)
    with open(os.path.join(sqls, "one.yaml"), "w") as f:
        f.write("url-path: /one\nmethod: GET\n"
                "template-source: one.sql\nconnection: [inmem]\n")
    with open(os.path.join(sqls, "one.sql"), "w") as f:
        f.write("SELECT 1 AS n\n")
    os.makedirs(os.path.join(ducklake_dir, "data"), exist_ok=True)
    cfg = os.path.join(tmp, "flapi.yaml")
    with open(cfg, "w") as f:
        f.write(
            "project-name: startup-failures\n"
            "project-description: a startup failure must exit non-zero\n"
            f"http-port: {port}\n"
            "template:\n  path: ./sqls\n"
            "duckdb:\n  access_mode: READ_WRITE\n"
            "ducklake:\n  enabled: true\n  alias: cache\n"
            f"  metadata-path: {os.path.join(ducklake_dir, 'meta.ducklake')}\n"
            f"  data-path: {os.path.join(ducklake_dir, 'data')}\n"
            "connections:\n  inmem:\n    properties:\n      database: ':memory:'\n")
    return cfg


def _env():
    return {**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"}


class _Holder:
    """A first instance that stays up, holding the DuckLake lock and the port."""

    def __init__(self, ducklake_dir=None):
        self.tmp = tempfile.mkdtemp(prefix="flapi_startup_hold_")
        self.ducklake_dir = ducklake_dir or self.tmp
        self.port = free_port()
        self.cfg = _write_config(self.tmp, self.port, self.ducklake_dir)
        self.log_path = os.path.join(self.tmp, "server.log")
        self.proc = None

    def start(self):
        self.proc = subprocess.Popen(
            [flapi_binary(), "-c", self.cfg, "-p", str(self.port),
             "--log-level", "warning"],
            stdout=open(self.log_path, "w"), stderr=subprocess.STDOUT,
            cwd=self.tmp, env=_env(), preexec_fn=os.setsid)
        deadline = time.time() + 90
        while time.time() < deadline:
            try:
                r = requests.get(
                    f"http://127.0.0.1:{self.port}/health/live", timeout=2)
                if r.status_code == 200:
                    return self
            except requests.RequestException:
                pass
            if self.proc.poll() is not None:
                pytest.fail("the first instance died before it was up:\n"
                            + open(self.log_path).read()[-3000:])
            time.sleep(0.2)
        pytest.fail("the first instance did not start:\n"
                    + open(self.log_path).read()[-3000:])

    def stop(self):
        if self.proc and self.proc.poll() is None:
            try:
                os.killpg(os.getpgid(self.proc.pid), signal.SIGKILL)
            except (ProcessLookupError, OSError):
                pass
            self.proc.wait(timeout=30)

    def __enter__(self):
        return self.start()

    def __exit__(self, *exc):
        self.stop()


def _run_second(cfg, cwd, port):
    started = time.time()
    r = subprocess.run(
        [flapi_binary(), "-c", cfg, "-p", str(port), "--log-level", "warning"],
        cwd=cwd, capture_output=True, text=True, timeout=120, env=_env())
    return r, time.time() - started


class TestLockedCacheFile:
    """Requirement 1: the DuckLake/DuckDB lock conflict."""

    def test_a_locked_cache_file_exits_non_zero_without_aborting(self):
        with _Holder() as first:
            # Same config, so the same metadata-path - but a different port,
            # so the ONLY thing that can fail is taking the file lock.
            second_port = free_port()
            r, elapsed = _run_second(first.cfg, first.tmp, second_port)

        combined = r.stdout + r.stderr
        assert r.returncode not in ABORT_EXIT_CODES, (
            f"aborted (core dump) instead of exiting: {r.returncode}\n"
            + combined[-2000:])
        # A signal death is a negative return code in Python. Requiring a
        # positive status is therefore the assertion that no signal - and so
        # no core dump - was involved.
        assert r.returncode > 0, (
            f"expected a positive exit status, got {r.returncode} "
            f"(negative means killed by signal {-r.returncode})\n"
            + combined[-2000:])
        assert elapsed < PROMPT_EXIT_SECONDS, (
            f"took {elapsed:.1f}s to give up on a locked file")

    def test_the_error_names_the_lock(self):
        # An operator must be able to act on it without a debugger: which
        # file, and which process is holding it.
        with _Holder() as first:
            second_port = free_port()
            r, _ = _run_second(first.cfg, first.tmp, second_port)
        # flAPI's log handler writes to stderr; stdout is checked too so the
        # test does not fail merely because that changes.
        combined = r.stdout + r.stderr
        assert "Database initialization error" in combined, combined[-2000:]
        assert "lock" in combined.lower(), combined[-2000:]
        assert ".ducklake" in combined, combined[-2000:]


class TestPortAlreadyBound:
    """Requirement 2: a bind failure used to exit 0."""

    def test_an_already_bound_port_exits_non_zero(self):
        with _Holder() as first:
            # A SEPARATE DuckLake directory, so the database initialises
            # cleanly and the bind is the only thing that can fail. Without
            # this the process would die at the lock instead and the test
            # would be a duplicate of the one above.
            tmp = tempfile.mkdtemp(prefix="flapi_startup_bind_")
            ducklake = tempfile.mkdtemp(prefix="flapi_startup_bind_dl_")
            cfg = _write_config(tmp, first.port, ducklake)
            r, elapsed = _run_second(cfg, tmp, first.port)

        combined = r.stdout + r.stderr
        assert r.returncode != 0, (
            "a server that never started exited 0; a supervisor cannot tell "
            "that from a clean shutdown\n" + combined[-2000:])
        assert r.returncode not in ABORT_EXIT_CODES, (
            f"aborted instead of exiting: {r.returncode}\n" + combined[-2000:])
        assert elapsed < PROMPT_EXIT_SECONDS, (
            f"took {elapsed:.1f}s to give up on a bound port")

    def test_the_error_names_the_bind(self):
        with _Holder() as first:
            tmp = tempfile.mkdtemp(prefix="flapi_startup_bind_")
            ducklake = tempfile.mkdtemp(prefix="flapi_startup_bind_dl_")
            cfg = _write_config(tmp, first.port, ducklake)
            r, _ = _run_second(cfg, tmp, first.port)
        combined = r.stdout + r.stderr
        assert "the server could not start" in combined, combined[-2000:]
        assert "Address already in use" in combined, combined[-2000:]
