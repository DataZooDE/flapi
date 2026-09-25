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
             "-p", str(self.port), "--log-level", getattr(self, "log_level", "warning")],
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
        import platform
        import signal as signal_mod
        import sys as _sys

        # The syscall number is per-architecture and /proc/<pid>/task is
        # Linux-only. Without this guard, an ARM64 runner - and flapi ships
        # ARM64 binaries - issues syscall 234 (which is not tgkill there),
        # delivers no signal, waits 60s, and reports "the process hung after
        # SIGTERM was delivered to a pool worker": a false positive of exactly
        # the bug under test.
        SYS_TGKILL = {"x86_64": 234, "aarch64": 131}
        machine = platform.machine()
        if _sys.platform != "linux" or machine not in SYS_TGKILL:
            pytest.skip(f"per-thread signal delivery not available on "
                        f"{_sys.platform}/{machine}")
        libc = ctypes.CDLL("libc.so.6", use_errno=True)
        SYS_tgkill = SYS_TGKILL[machine]

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

    def test_a_signalled_exit_is_a_clean_exit(self):
        # Kept for what it asserts - exit status 0, i.e. main() RETURNED - but
        # no longer claims to prove the shutdown ran on the supervisor thread.
        # The earlier version promised "direct evidence... the shutdown log
        # line must come from the supervisor thread" and then asserted only
        # `code == 0`, which is identical in substance to the idle baseline
        # above. The per-thread delivery test is the one that discriminates.
        s = _Server().start()
        try:
            code = self._terminate(s, timeout=30)
            assert code is not None, "no exit at all"
            # A negative status is death BY a signal - the default
            # disposition, i.e. the handler never completed.
            assert code == 0, (
                f"expected a clean exit; got {code}\n"
                + open(s.log_path).read()[-2000:])
        finally:
            s.stop()

class TestHeartbeatRequestsAreNotOffloaded:
    """The heartbeat must not reach the offload with a request that has no connection.

    APIServer::requestForEndpoint() used to build a bare crow::request and call
    app.handle_full() directly, so `req.io_service` and `req.middleware_context`
    were both null. The offload dereferenced both - `*req.io_service` in the
    Completer and get_context<>() beside it. Offload is ON by default, so every
    deployment with a scheduled cache was hitting it.

    That fix shipped with NO test: nothing in test/ referenced
    requestForEndpoint, and the scheduler suite never waits for a
    heartbeat-driven refresh. This is the minimal experiment that discriminates
    inline-from-offloaded for a synthesised request: let the heartbeat fire,
    and require the process to still be answering afterwards.

    #141 removed the cause rather than the symptom - APIServer::warmEndpoint()
    calls RequestHandler directly and nothing synthetic enters the router any
    more - so this class no longer guards a guard. It is kept because what it
    asserts is still exactly right from the outside: a heartbeat-backed cache
    refreshes, and the process survives it. test_heartbeat_warming.py covers
    what the new path does; this covers that the offload is still not involved.
    """

    def _server_with_heartbeat(self, log_level="warning"):
        s = _Server()
        s.log_level = log_level
        sqls = os.path.join(s.tmp, "sqls")
        # A cached endpoint whose heartbeat fires every second.
        with open(os.path.join(sqls, "cached.yaml"), "w") as f:
            f.write("url-path: /cached\nmethod: GET\n"
                    "template-source: cached.sql\nconnection: [inmem]\n"
                    "cache:\n"
                    "  enabled: true\n"
                    "  table: hb_cache\n"
                    "  schema: main\n"
                    "  schedule: 1s\n"
                    "heartbeat:\n  enabled: true\n")
        with open(os.path.join(sqls, "cached.sql"), "w") as f:
            f.write("SELECT 1 AS n\n")
        with open(os.path.join(s.tmp, "flapi.yaml"), "w") as f:
            f.write(
                "project-name: heartbeat-offload\n"
                "project-description: a synthesised request must not be offloaded\n"
                f"http-port: {s.port}\n"
                "stall-timeout-s: 5\n"
                "template:\n  path: ./sqls\n"
                "duckdb:\n  access_mode: READ_WRITE\n"
                "ducklake:\n  enabled: true\n  alias: cache\n"
                f"  metadata-path: {os.path.join(s.tmp, 'meta.ducklake')}\n"
                f"  data-path: {os.path.join(s.tmp, 'data')}\n"
                "connections:\n  inmem:\n    properties:\n      database: ':memory:'\n"
                # worker-interval is an INTEGER number of seconds, not a duration
                "heartbeat:\n  enabled: true\n  worker-interval: 1\n")
        os.makedirs(os.path.join(s.tmp, "data"), exist_ok=True)
        return s

    def test_the_heartbeat_actually_fires(self):
        # "Still answering 20s later" is only evidence if the heartbeat ran.
        # Without this the test stays green while exercising nothing at all,
        # should `heartbeat.enabled` or `worker-interval` ever stop parsing
        # the way this config assumes - which is exactly how it was written
        # the first time.
        with self._server_with_heartbeat(log_level="debug") as s:
            deadline = time.time() + 25
            log = ""
            while time.time() < deadline:
                log = open(s.log_path).read()
                if "heartbeat" in log.lower() and "/cached" in log:
                    break
                time.sleep(1)
            assert "heartbeat" in log.lower(), (
                "no heartbeat activity in the log; the test below would be "
                "asserting nothing\n" + log[-3000:])

    def test_the_server_survives_a_heartbeat_driven_refresh(self):
        # Before #141 the heartbeat's synthesised request took the offload path,
        # dereferenced a null io_service, and took the process with it - so
        # "still answering 15s later" is the discriminator.
        with self._server_with_heartbeat() as s:
            deadline = time.time() + 20
            last = None
            while time.time() < deadline:
                try:
                    last = requests.get(f"{s.base_url}/health/live", timeout=3)
                except requests.RequestException as exc:
                    last = exc
                time.sleep(1)
            assert isinstance(last, requests.Response) and last.status_code == 200, (
                "the server stopped answering while the heartbeat ran; a "
                "synthesised request was probably offloaded\n"
                + open(s.log_path).read()[-3000:])
            assert s.proc.poll() is None, (
                "the process exited during heartbeat refreshes\n"
                + open(s.log_path).read()[-3000:])

    def test_ordinary_requests_still_work_after_a_heartbeat_refresh(self):
        with self._server_with_heartbeat() as s:
            time.sleep(6)   # several heartbeat cycles
            r = requests.get(f"{s.base_url}/cached", timeout=15)
            assert r.status_code == 200, r.text


class TestSigtermDuringStartup:
    """The window between process start and bind.

    This sequence has changed in three consecutive review rounds and had no
    test at any point: every other termination case here signals a server that
    is already answering /health/live.

    The failure it guards against is specific and bad. APIServer::stop() can
    run before Crow has published its server, and Crow's app.stop() is a no-op
    until it does - so a SIGTERM in that window drained the handler pool,
    stopped nothing, and left a process that served 503 for the rest of its
    life and could only be killed with SIGKILL.
    """

    def _start(self, log_level="warning", pause_before_bind_ms=None):
        # A DELIBERATELY slow startup, so the window this guards is wide
        # enough to aim at. The dangerous gap is between APIServer being
        # constructed and Crow publishing its server, and cache warmup sits
        # inside it - on a trivial config that gap is about a millisecond.
        s = _Server()
        s.log_level = log_level
        sqls = os.path.join(s.tmp, "sqls")
        os.makedirs(os.path.join(s.tmp, "data"), exist_ok=True)
        for i in range(12):
            with open(os.path.join(sqls, f"c{i}.yaml"), "w") as f:
                f.write(f"url-path: /c{i}\nmethod: GET\n"
                        f"template-source: c{i}.sql\nconnection: [inmem]\n"
                        "cache:\n  enabled: true\n"
                        f"  table: c{i}_cache\n  schema: main\n  schedule: 1h\n")
            with open(os.path.join(sqls, f"c{i}.sql"), "w") as f:
                f.write("SELECT sum(i) AS s FROM range(0, 400000) t(i)\n")
        with open(os.path.join(s.tmp, "flapi.yaml"), "w") as f:
            f.write(
                "project-name: slow-start\n"
                "project-description: a signal must not be lost during startup\n"
                f"http-port: {s.port}\n"
                "template:\n  path: ./sqls\n"
                "duckdb:\n  access_mode: READ_WRITE\n"
                "ducklake:\n  enabled: true\n  alias: cache\n"
                f"  metadata-path: {os.path.join(s.tmp, 'meta.ducklake')}\n"
                f"  data-path: {os.path.join(s.tmp, 'data')}\n"
                "connections:\n  inmem:\n    properties:\n      database: ':memory:'\n")
        env = {**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1",
               "FLAPI_DISABLE_HANDLER_OFFLOAD": "0", "FLAPI_IO_THREADS": "2"}
        if pause_before_bind_ms is not None:
            env["FLAPI_TEST_PAUSE_BEFORE_BIND"] = str(pause_before_bind_ms)
        s.proc = subprocess.Popen(
            [flapi_binary(), "-c", os.path.join(s.tmp, "flapi.yaml"),
             "-p", str(s.port), "--log-level", log_level],
            stdout=open(s.log_path, "w"), stderr=subprocess.STDOUT,
            cwd=s.tmp, env=env, preexec_fn=os.setsid)
        return s

    @pytest.mark.parametrize("delay", [0.0, 0.01, 0.05, 0.1, 0.2, 0.35])
    def test_sigterm_before_the_port_is_bound_still_exits(self, delay):
        # Swept across the window rather than aimed at one point in it,
        # because where startup actually is at a given millisecond varies by
        # machine and by run.
        import signal as signal_mod
        s = self._start()
        try:
            time.sleep(delay)
            os.killpg(os.getpgid(s.proc.pid), signal_mod.SIGTERM)
            try:
                code = s.proc.wait(timeout=45)
            except subprocess.TimeoutExpired:
                code = None

            assert code is not None, (
                f"SIGTERM {delay}s after exec did not terminate the process; "
                "this is the 503-forever state\n"
                + open(s.log_path).read()[-3000:])
            # Two acceptable outcomes, and only two:
            #   0    the handler was installed and shutdown ran;
            #   -15  the signal arrived before the handler was installed, so
            #        the DEFAULT disposition terminated the process.
            # The failure being guarded against is neither: a process that
            # survives, having drained its pool, and serves 503 forever.
            assert code in (0, -signal_mod.SIGTERM), (
                f"SIGTERM {delay}s after exec exited {code}\n"
                + open(s.log_path).read()[-3000:])

            # And it must not be left serving.
            with pytest.raises(requests.RequestException):
                requests.get(f"{s.base_url}/health/live", timeout=2)
        finally:
            s.stop()

    def test_two_signals_in_the_startup_window_are_not_swallowed(self):
        # The supervisor used to handle exactly one signal and return, so a
        # first SIGTERM that arrived too early left nothing to read a second.
        import signal as signal_mod
        s = self._start()
        try:
            time.sleep(0.02)
            os.killpg(os.getpgid(s.proc.pid), signal_mod.SIGTERM)
            time.sleep(0.05)
            os.killpg(os.getpgid(s.proc.pid), signal_mod.SIGTERM)
            try:
                code = s.proc.wait(timeout=45)
            except subprocess.TimeoutExpired:
                code = None
            assert code is not None, (
                "two SIGTERMs during startup and the process is still alive\n"
                + open(s.log_path).read()[-3000:])
            assert code in (0, -signal_mod.SIGTERM), code
        finally:
            s.stop()

    def _wait_for_log(self, s, marker, timeout=60):
        """Block until `marker` appears in the server log, or time out."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                with open(s.log_path) as f:
                    if marker in f.read():
                        return True
            except FileNotFoundError:
                pass
            if s.proc.poll() is not None:
                return False
            time.sleep(0.05)
        return False

    def test_sigterm_inside_the_pre_bind_window_drains_and_exits_zero(self):
        """The deterministic counterpart to the delay sweep above.

        The sweep aims six guesses at a window about a millisecond wide, so it
        does not reliably go red against any of the three defects it covers -
        every one of them was found by reading the code instead. FLAPI_TEST_
        PAUSE_BEFORE_BIND holds the process *in* that window, which lets this
        assert the thing the sweep cannot: exit status exactly 0.

        The sweep must accept `-SIGTERM` too, because a signal that lands
        before main() installs the handler is legitimately killed by the
        default disposition. Here the handler is provably installed - we waited
        for a log line that main() emits after installing it - so the default
        disposition is NOT an acceptable outcome, and a regression that loses
        the signal shows up as a hang rather than as a different-but-allowed
        exit code.
        """
        import signal as signal_mod
        s = self._start(pause_before_bind_ms=5000)
        try:
            assert self._wait_for_log(s, "startup paused before bind"), (
                "the process never reached the pre-bind pause\n"
                + open(s.log_path).read()[-3000:])

            # Provably inside the window now: flapi's SIGTERM handler is
            # installed, and crow has no server, so app.stop() is a no-op.
            os.killpg(os.getpgid(s.proc.pid), signal_mod.SIGTERM)
            try:
                code = s.proc.wait(timeout=60)
            except subprocess.TimeoutExpired:
                code = None

            log = open(s.log_path).read()
            assert code is not None, (
                "SIGTERM inside the pre-bind window did not terminate the "
                "process; this is the 503-forever state that needed SIGKILL\n"
                + log[-4000:])
            assert code == 0, (
                f"expected a clean 0, got {code}; the handler was installed "
                "before the signal, so nothing else is acceptable here\n"
                + log[-4000:])
            # It must not be left listening.
            with pytest.raises(requests.RequestException):
                requests.get(f"{s.base_url}/health/live", timeout=2)
        finally:
            s.stop()

    def test_the_pause_seam_is_off_by_default_and_capped_when_on(self):
        """The seam must be inert unless asked for, and bounded when asked.

        A test-only knob in production code earns its keep only if it cannot be
        left on silently (hence WARNING, asserted here) and cannot hold a
        container closed if mistyped (hence the cap).
        """
        # INFO: "Server starting on" is an info-level line, and _start defaults
        # to warning.
        s = self._start(log_level="info")
        try:
            assert self._wait_for_log(s, "Server starting on", timeout=90), (
                "server did not start without the seam\n"
                + open(s.log_path).read()[-2000:])
            log = open(s.log_path).read()
            assert "FLAPI_TEST_PAUSE_BEFORE_BIND" not in log, (
                "the seam announced itself when it was never set\n" + log[-2000:])
            assert "startup paused before bind" not in log
        finally:
            s.stop()

        # Absurd value: capped, and it says so rather than obeying.
        s2 = self._start(pause_before_bind_ms=999999)
        try:
            assert self._wait_for_log(s2, "startup paused before bind", timeout=60)
            log = open(s2.log_path).read()
            assert "holding for 10000ms" in log, (
                "999999ms was not capped to the 10s ceiling\n" + log[-2000:])
            assert "must not be set in production" in log
        finally:
            s2.stop()
