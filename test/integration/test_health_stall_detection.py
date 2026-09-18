"""Readiness fails when a request stalls (#116).

A backend can stop answering without failing. A SQLite attachment that loses its
write transaction wedges permanently, while DuckDB, the HTTP server and every
other connection carry on — so a `SELECT 1` readiness probe passes, the
orchestrator sees a healthy instance, and traffic keeps arriving at an endpoint
that will never answer again.

Probing the wedged resource does not work: the probe blocks too, and leaks a
thread per health check. So flAPI measures the *symptom* — how long the oldest
in-flight request has been running — which costs nothing and catches any stall,
not only this one.

These tests drive the real binary over real HTTP. The stall is produced by a
genuinely slow query rather than by faulting anything, so it is deterministic.
"""
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
    """A flAPI server with a configurable stall timeout and one slow endpoint."""

    def __init__(self, stall_timeout_s: int):
        self.tmp = tempfile.mkdtemp(prefix="flapi_stall_")
        self.port = free_port()
        self.base_url = f"http://127.0.0.1:{self.port}"
        self.log_path = os.path.join(self.tmp, "server.log")
        sqls = os.path.join(self.tmp, "sqls")
        os.makedirs(sqls, exist_ok=True)

        # sum(i*i), not count(*): DuckDB knows range(N) has N rows and folds
        # count(*) to a constant, so the "slow" query returned in 0.35s and no
        # stall ever happened.
        #
        # The size is set by the SLOWEST machine's budget and the FASTEST
        # machine's window. 400M took 2.6s here but 1.53s on CI, leaving only
        # ~0.5s in which readiness could be caught reporting a stall - narrow
        # enough that the test failed there and passed here, twice. 1.2B takes
        # ~7s here and ~4.5s on CI, so the window is seconds wide either way.
        # Still bounded, so the test cannot hang if something goes wrong.
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
                "project-name: stall-test\n"
                "project-description: readiness stall detection\n"
                f"http-port: {self.port}\n"
                f"stall-timeout-s: {stall_timeout_s}\n"
                "template:\n  path: ./sqls\n"
                "connections:\n  inmem:\n    properties:\n      database: ':memory:'\n")
        self.proc = None

    def start(self):
        self.proc = subprocess.Popen(
            [flapi_binary(), "-c", os.path.join(self.tmp, "flapi.yaml"),
             "-p", str(self.port), "--log-level", "info"],
            stdout=open(self.log_path, "w"), stderr=subprocess.STDOUT, cwd=self.tmp,
            env={**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"},
            preexec_fn=os.setsid)
        deadline = time.time() + 60
        while time.time() < deadline:
            try:
                if requests.get(f"{self.base_url}/health/live", timeout=1).status_code == 200:
                    return self
            except requests.RequestException:
                pass
            time.sleep(0.2)
        pytest.fail("server did not start")

    def stop(self):
        if self.proc:
            import signal
            try:
                os.killpg(os.getpgid(self.proc.pid), signal.SIGKILL)
            except ProcessLookupError:
                pass
            self.proc.wait(timeout=20)

    def __enter__(self):
        return self.start()

    def __exit__(self, *exc):
        self.stop()


class TestStallDetection:
    def test_readiness_stays_green_when_nothing_is_stalled(self):
        with _Server(stall_timeout_s=1) as s:
            r = requests.get(f"{s.base_url}/health", timeout=10)
            assert r.status_code == 200, r.text
            body = r.json()
            assert body["requests"]["in_flight"] == 0
            assert body["requests"]["oldest_ms"] == 0

    def test_readiness_fails_while_a_request_is_stalled(self):
        # The contract. A request outliving the budget takes the instance out of
        # rotation, so an orchestrator replaces it instead of routing more
        # traffic at something that cannot answer.
        #
        # Polled from a thread at 50ms rather than 200ms between blocking calls:
        # the window is only as wide as the slow query outlives the budget, and
        # a sampler that is slower than the window turns a real contract into a
        # coin flip.
        #
        # Readiness is polled by SEVERAL samplers, not one, because a single
        # one is not enough to observe the contract: flAPI runs a handler on
        # the Crow io thread that owns its connection, so a slow synchronous
        # query blocks every other connection landing on that same thread.
        # Measured here - of 40 health requests issued during a stall, 39
        # returned in under a millisecond and one waited 5.1s for the query to
        # finish. With one sampler, drawing that thread ends the observation;
        # the odds are ~1/threads, which is why this passed on a 32-core
        # machine and failed on CI. Filed as #120 - readiness being blockable
        # by the very stall it reports is a real limitation, not a test
        # artifact, and the workaround below lives in the test, not the
        # product.
        #
        # Every sample is timestamped and liveness is sampled alongside, so a
        # failure distinguishes a slot released early, a blocked probe, and a
        # budget never applied, instead of only saying "no stall was reported".
        with _Server(stall_timeout_s=1) as s:
            done = threading.Event()
            elapsed = {}
            readiness = []
            liveness = []
            saw_stalled = None
            t0 = time.time()

            def slow():
                started = time.time()
                try:
                    r = requests.get(f"{s.base_url}/slow", timeout=120)
                    elapsed["code"] = r.status_code
                    elapsed["body"] = r.text[:200]
                except Exception as e:                      # noqa: BLE001
                    elapsed["code"] = f"error: {e}"
                finally:
                    elapsed["s"] = time.time() - started
                    done.set()

            def sample(url, into, want_stall):
                nonlocal saw_stalled
                deadline = time.time() + 90
                while time.time() < deadline and not done.is_set():
                    sent = time.time() - t0
                    try:
                        # Short, and retried on a NEW connection. A probe that
                        # draws the blocked io thread must be abandoned rather
                        # than waited out - waiting out is what ends the
                        # observation. This is also what a real orchestrator
                        # does with a readiness probe that does not answer.
                        r = requests.get(url, timeout=1.0)
                        body = r.json()
                    except Exception as e:                  # noqa: BLE001
                        into.append((round(sent, 2), "error", str(e)[:80]))
                        time.sleep(0.05)
                        continue
                    into.append((round(sent, 2), round(time.time() - t0, 2),
                                 r.status_code, body.get("status"),
                                 body.get("requests")))
                    if want_stall and r.status_code == 503 \
                            and body.get("status") == "stalled":
                        saw_stalled = body
                        return
                    time.sleep(0.05)

            threads = [
                threading.Thread(target=sample,
                                 args=(f"{s.base_url}/health", readiness, True),
                                 daemon=True)
                for _ in range(4)
            ] + [
                threading.Thread(target=sample,
                                 args=(f"{s.base_url}/health/live", liveness, False),
                                 daemon=True),
                threading.Thread(target=slow, daemon=True),
            ]
            for t in threads:
                t.start()
            done.wait(timeout=180)
            for t in threads[:-1]:
                t.join(timeout=20)

            took = elapsed.get("s")
            # Separate the ways this can fail. If the "slow" query was not slow,
            # the fixture is wrong and there was no stall to detect - saying so
            # is very different from saying readiness missed one. This demands
            # at least a second of window past the 1s budget, rather than the
            # query merely outliving it by a hair.
            assert took is not None and took > 2.0, (
                f"the fixture's slow query returned in {took}s (status "
                f"{elapsed.get('code')}, body {elapsed.get('body')!r}), so it "
                f"never outlived the 1s budget - there was no stall to detect. "
                f"Make it do more work, or fix whatever made it fail fast, "
                f"rather than relaxing the assertion below."
            )
            assert saw_stalled is not None, (
                f"readiness never reported a stall while a request ran for "
                f"{took:.1f}s against a 1s budget (slow request returned "
                f"{elapsed.get('code')}).\n"
                f"readiness ({len(readiness)} samples, each "
                f"(sent_at, returned_at, code, status, requests)):\n"
                f"  {readiness}\n"
                f"liveness ({len(liveness)} samples):\n  {liveness}\n"
                f"server log tail:\n{open(s.log_path).read()[-3000:]}"
            )
            assert saw_stalled["requests"]["in_flight"] >= 1
            assert saw_stalled["requests"]["oldest_ms"] >= 1000
            assert saw_stalled["stalled_after_s"] == 1

            # Liveness must NOT fail: the process is fine, and killing it
            # rather than draining it would turn a stall into an outage.
            live = requests.get(f"{s.base_url}/health/live", timeout=10)
            assert live.status_code == 200

    def test_readiness_recovers_once_the_request_finishes(self):
        # A latched health check is as useless as one that never fires.
        with _Server(stall_timeout_s=1) as s:
            done = threading.Event()

            def slow():
                try:
                    requests.get(f"{s.base_url}/slow", timeout=120)
                finally:
                    done.set()

            worker = threading.Thread(target=slow, daemon=True)
            worker.start()
            done.wait(timeout=120)
            assert done.is_set(), "the slow query never completed"

            deadline = time.time() + 20
            while time.time() < deadline:
                r = requests.get(f"{s.base_url}/health", timeout=10)
                if r.status_code == 200:
                    assert r.json()["requests"]["in_flight"] == 0
                    return
                time.sleep(0.2)
            pytest.fail("readiness stayed stalled after the request finished")

    def test_the_check_can_be_disabled(self):
        # 0 disables it. An operator running deliberately long synchronous
        # queries must be able to opt out rather than be told their service is
        # unhealthy for working as designed.
        with _Server(stall_timeout_s=0) as s:
            done = threading.Event()

            def slow():
                try:
                    requests.get(f"{s.base_url}/slow", timeout=120)
                finally:
                    done.set()

            worker = threading.Thread(target=slow, daemon=True)
            worker.start()
            try:
                time.sleep(3)   # well past any plausible budget
                r = requests.get(f"{s.base_url}/health", timeout=10)
                assert r.status_code == 200, (
                    f"stall detection fired with stall-timeout-s: 0 -> {r.text}"
                )
            finally:
                done.wait(timeout=120)
