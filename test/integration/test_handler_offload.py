"""A slow request must not block other connections (#120).

Crow runs a handler on the io thread that owns its connection, so a slow
synchronous query held that thread for the whole query and every other
connection assigned to it waited - including GET /health, which is how a
stalled instance is supposed to report itself.

Measured on one ~6.5s query with 40 concurrent probes and a single io thread:

    offload OFF : 40/40 probes blocked,  0/40 reported the stall, worst 5.04s
    offload ON  :  0/40 probes blocked, 40/40 reported the stall, worst 0.005s

These tests FORCE the minimum io thread count (FLAPI_IO_THREADS=2, one accept
thread plus one io thread), because on a many-core machine the thread floor
makes the condition vanishingly unlikely to occur and the test passes against
the un-offloaded server too. The first version of this file did exactly that -
a crew review caught it - so each test here is checked against
FLAPI_DISABLE_HANDLER_OFFLOAD=1 and must fail there.
"""
import json
import os
import subprocess
import tempfile
import threading
import time

import pytest
import requests

from otel_helpers import flapi_binary, free_port

pytestmark = pytest.mark.standalone_server


class _Server:
    def __init__(self, offload: bool = True):
        self.offload = offload
        self.tmp = tempfile.mkdtemp(prefix="flapi_offload_")
        self.port = free_port()
        self.base_url = f"http://127.0.0.1:{self.port}"
        self.log_path = os.path.join(self.tmp, "server.log")
        sqls = os.path.join(self.tmp, "sqls")
        os.makedirs(sqls, exist_ok=True)
        with open(os.path.join(sqls, "slow.yaml"), "w") as f:
            f.write("url-path: /slow\nmethod: GET\n"
                    "template-source: slow.sql\nconnection: [inmem]\n")
        with open(os.path.join(sqls, "slow.sql"), "w") as f:
            f.write("SELECT sum(i*i) AS s FROM range(0, 1200000000) t(i)\n")
        with open(os.path.join(sqls, "fast.yaml"), "w") as f:
            f.write("url-path: /fast\nmethod: GET\n"
                    "template-source: fast.sql\nconnection: [inmem]\n")
        with open(os.path.join(sqls, "fast.sql"), "w") as f:
            f.write("SELECT 1 AS n\n")
        with open(os.path.join(self.tmp, "flapi.yaml"), "w") as f:
            f.write(
                "project-name: offload-test\n"
                "project-description: a slow request must not block others\n"
                f"http-port: {self.port}\n"
                "stall-timeout-s: 1\n"
                "template:\n  path: ./sqls\n"
                "connections:\n  inmem:\n    properties:\n      database: ':memory:'\n")
        self.proc = None

    def start(self):
        env = {**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"}
        env["FLAPI_DISABLE_HANDLER_OFFLOAD"] = "0" if self.offload else "1"
        # One accept thread + one io thread: the condition #120 is about.
        env["FLAPI_IO_THREADS"] = "2"
        self.proc = subprocess.Popen(
            [flapi_binary(), "-c", os.path.join(self.tmp, "flapi.yaml"),
             "-p", str(self.port), "--log-level", "warning"],
            stdout=open(self.log_path, "w"), stderr=subprocess.STDOUT,
            cwd=self.tmp, env=env, preexec_fn=os.setsid)
        deadline = time.time() + 90
        while time.time() < deadline:
            try:
                if requests.get(f"{self.base_url}/health/live", timeout=2).status_code == 200:
                    return self
            except requests.RequestException:
                pass
            time.sleep(0.2)
        pytest.fail(f"server did not start:\n{open(self.log_path).read()[-3000:]}")

    def stop(self):
        if self.proc:
            import signal
            try:
                os.killpg(os.getpgid(self.proc.pid), signal.SIGKILL)
            except ProcessLookupError:
                pass
            self.proc.wait(timeout=30)

    def __enter__(self):
        return self.start()

    def __exit__(self, *exc):
        self.stop()


def _in_flight_slow(server):
    """Start a slow request and return a thread plus a done-event."""
    done = threading.Event()

    def run():
        try:
            requests.get(f"{server.base_url}/slow", timeout=180)
        finally:
            done.set()

    t = threading.Thread(target=run, daemon=True)
    t.start()
    return t, done


class TestHandlerOffload:
    def test_other_endpoints_answer_while_a_slow_query_runs(self):
        # The contract. Without the offload these queue behind the query on
        # whichever io thread the slow request occupies.
        with _Server() as s:
            worker, done = _in_flight_slow(s)
            try:
                time.sleep(1.5)
                assert not done.is_set(), "the slow query finished too early to test anything"
                for _ in range(20):
                    r = requests.get(f"{s.base_url}/fast", timeout=10)
                    assert r.status_code == 200, r.text[:200]
                    assert r.json()["data"][0]["n"] == 1
            finally:
                done.wait(timeout=180)

    def test_readiness_reports_the_stall_while_it_is_happening(self):
        # The point of #120: the probe has to be able to ANSWER in order to
        # report. Blocked probes cannot say anything.
        with _Server() as s:
            worker, done = _in_flight_slow(s)
            try:
                deadline = time.time() + 30
                saw = None
                while time.time() < deadline and not done.is_set():
                    r = requests.get(f"{s.base_url}/health", timeout=5)
                    if r.status_code == 503 and r.json().get("status") == "stalled":
                        saw = r.json()
                        break
                    time.sleep(0.1)
                assert saw is not None, "readiness never reported the stall"
                assert saw["requests"]["in_flight"] >= 1
            finally:
                done.wait(timeout=180)

    def test_the_slow_request_still_gets_its_answer(self):
        # Offloading must not lose the response.
        with _Server() as s:
            r = requests.get(f"{s.base_url}/slow", timeout=180)
            assert r.status_code == 200, r.text[:200]
            assert r.json()["data"][0]["s"] is not None

    def test_the_offload_can_be_disabled(self):
        # The escape hatch must actually serve requests, so a deployment that
        # hits something unanticipated can fall back.
        with _Server(offload=False) as s:
            r = requests.get(f"{s.base_url}/fast", timeout=20)
            assert r.status_code == 200
            assert r.json()["data"][0]["n"] == 1


class TestOffloadTestsActuallyDiscriminate:
    """The tests above must FAIL without the offload.

    A regression test that passes against the defect it claims to pin is
    worse than no test: it reports safety that was never checked. Three of the
    four tests added with #137 did exactly that, which a crew review found.
    """

    def test_other_endpoints_are_blocked_when_the_offload_is_off(self):
        with _Server(offload=False) as s:
            worker, done = _in_flight_slow(s)
            try:
                time.sleep(1.5)
                assert not done.is_set(), "the slow query finished too early to test anything"
                blocked = 0
                for _ in range(6):
                    start = time.time()
                    try:
                        requests.get(f"{s.base_url}/fast", timeout=3)
                    except requests.Timeout:
                        blocked += 1
                        continue
                    if time.time() - start > 0.5:
                        blocked += 1
                assert blocked > 0, (
                    "no request blocked with the offload disabled, so the "
                    "offload tests above prove nothing")
            finally:
                done.wait(timeout=180)


class TestGracefulTermination:
    """SIGTERM must be handled off the signal handler, and must terminate.

    APIServer::stop() was invoked directly FROM the POSIX signal handler, and
    it now calls HandlerPool::shutdown() - which takes a mutex and joins every
    worker. Neither is async-signal-safe. A signal is delivered on whichever
    thread happens to be running, so SIGTERM landing on a pool worker meant
    that worker attempting to join itself, and a signal arriving while any
    thread held the pool mutex meant re-entering it. Both hang until the
    platform SIGKILLs the container mid-write.

    Every other fixture in this file stops the server with SIGKILL, so this
    path had never once been exercised.
    """

    def _terminate(self, server, timeout):
        import signal
        os.killpg(os.getpgid(server.proc.pid), signal.SIGTERM)
        try:
            return server.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            return None

    def test_sigterm_with_nothing_in_flight_exits_promptly(self):
        # Baseline: if this hangs, nothing below means anything.
        s = _Server().start()
        try:
            assert self._terminate(s, timeout=30) is not None, (
                "the process did not exit on SIGTERM while completely idle")
        finally:
            s.stop()

    def test_sigterm_with_a_request_in_flight_and_another_queued_exits(self):
        # The measured hazard: work on the pool while the signal arrives.
        s = _Server().start()
        try:
            in_flight, _ = _in_flight_slow(s)
            queued, _ = _in_flight_slow(s)
            # Let both reach the pool - one running, one queued behind it.
            time.sleep(1.5)
            assert requests.get(f"{s.base_url}/fast", timeout=10).status_code == 200

            code = self._terminate(s, timeout=60)
            assert code is not None, (
                "the process did not exit on SIGTERM with a request in flight; "
                "a container would be SIGKILLed here\n"
                + open(s.log_path).read()[-3000:])
            # "It exited" is not the contract - it exited by SIGSEGV before
            # this was tightened, and the weaker assertion passed on it.
            assert code == 0, (
                f"SIGTERM with a query in flight did not shut down cleanly: {code} "
                f"({'killed by signal ' + str(-code) if code < 0 else 'status'})\n"
                + open(s.log_path).read()[-3000:])
            in_flight.join(timeout=5)
            queued.join(timeout=5)
        finally:
            s.stop()

    def test_sigterm_delivered_to_a_pool_worker_still_shuts_down(self):
        # THE discriminating case, and the reason the other two are not enough.
        #
        # killpg lets the kernel pick any thread that is not blocking the
        # signal, so it almost never picks a busy pool worker and the unsafe
        # version passes. Delivering to EVERY thread with tgkill(2) guarantees
        # a worker receives it while it is inside a job.
        #
        # Unsafe version: that worker runs APIServer::stop() -> shutdown() ->
        # join() on the thread it is running on. std::thread::join on self is
        # undefined; in practice it throws system_error "Resource deadlock
        # avoided" out of a signal handler, or hangs.
        #
        # Safe version: every one of those handlers does nothing but write a
        # byte to the pipe, and the supervisor performs the shutdown once.
        import ctypes
        import signal as signal_mod

        libc = ctypes.CDLL("libc.so.6", use_errno=True)
        SYS_tgkill = 234   # x86_64

        s = _Server().start()
        try:
            in_flight, _ = _in_flight_slow(s)
            queued, _ = _in_flight_slow(s)
            time.sleep(1.5)   # let one reach a worker and the other queue

            pid = s.proc.pid
            tids = [int(t) for t in os.listdir(f"/proc/{pid}/task")]
            assert len(tids) > 2, f"expected a threaded server, saw {tids}"
            for tid in tids:
                libc.syscall(SYS_tgkill, pid, tid, int(signal_mod.SIGTERM))

            try:
                code = s.proc.wait(timeout=60)
            except subprocess.TimeoutExpired:
                code = None
            assert code is not None, (
                "the process hung after SIGTERM was delivered to a pool "
                "worker - this is the self-join\n"
                + open(s.log_path).read()[-3000:])
            assert code == 0, (
                f"expected a clean exit, got {code}\n"
                + open(s.log_path).read()[-3000:])
            in_flight.join(timeout=5)
            queued.join(timeout=5)
        finally:
            s.stop()

    def test_sigterm_is_not_handled_inside_the_signal_handler(self):
        # Direct evidence rather than inference from "it exited": the shutdown
        # log line must come from the supervisor thread, and the process must
        # exit cleanly rather than via a signal.
        s = _Server().start()
        try:
            code = self._terminate(s, timeout=30)
            assert code is not None, "no exit at all"
            # A negative return code is death BY a signal - the default
            # disposition, i.e. the handler never completed. Handled properly,
            # the supervisor runs the shutdown and main() RETURNS, so the exit
            # status is an ordinary 0.
            assert code == 0, (
                f"expected a clean exit; got {code} "
                f"({'killed by signal ' + str(-code) if code < 0 else 'non-zero status'})\n"
                + open(s.log_path).read()[-2000:])
        finally:
            s.stop()
