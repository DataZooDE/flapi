"""Soak: minutes of mixed load on the realistic project, then a clean SIGTERM (#241).

Skipped unless FLAPI_SOAK=1 (FLAPI_SOAK_SECONDS, default 600). One server, the current binary, with
telemetry ON - DATAZOO_DISABLE_TELEMETRY unset - but kept on the box: PostHog's host cannot be set
from the environment, so realistic_shim.so (LD_PRELOAD) resolves every non-loopback name to 127.0.0.1
and redirects every outbound connect to a local sink that accepts and never answers - an unreachable,
hanging ingestion host. No DNS query and no byte leaves the machine.

Concurrently, for the whole run:
  * 20 keep-alive REST clients (JSON / CSV / Arrow, every page shape of the matrix, auth outcomes),
  * 4 clients that open a new connection per request (fd / socket churn),
  * 6 MCP clients (tools/call),
  * a DuckLake cache refresher (every 2 s) and garbage collection (every 30 s),
  * 2 config-service readers (endpoint list, endpoint, cache config, audit, metrics, schema),
  * random unmatched URLs (the telemetry aggregation's `<unmatched>` route).

Asserted: zero 5xx and zero transport errors; every response equal to the serial run's (checksums);
after warm-up RSS plateaus (slope bound) and the quiescent fd / thread counts come back to where they
were; SIGTERM exits 0 quickly while the telemetry host hangs.
"""
import concurrent.futures as cf
import os
import random
import shutil
import socket
import statistics
import tempfile
import threading
import time
import uuid

import pytest
import requests

import realistic_project as rp

pytestmark = [pytest.mark.soak, pytest.mark.standalone_server, pytest.mark.slow,
              pytest.mark.skipif(os.environ.get("FLAPI_SOAK") != "1", reason="soak: set FLAPI_SOAK=1")]

SECONDS = int(os.environ.get("FLAPI_SOAK_SECONDS", "600"))
WARMUP = max(30, SECONDS // 4)
SAMPLE_EVERY = 5


class Sink:
    """A TCP listener that accepts and never answers (the unreachable telemetry host)."""

    def __init__(self):
        self.sock = socket.socket()
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(64)
        self.port = self.sock.getsockname()[1]
        self.accepted, self.bytes, self.conns = 0, 0, []
        self.stop = threading.Event()
        threading.Thread(target=self._run, daemon=True).start()

    def _run(self):
        self.sock.settimeout(0.5)
        while not self.stop.is_set():
            try:
                c, _ = self.sock.accept()
                self.accepted += 1
                c.setblocking(False)
                self.conns.append(c)
            except OSError:
                pass
            for x in list(self.conns):
                try:
                    self.bytes += len(x.recv(65536))
                except BlockingIOError:
                    pass
                except OSError:
                    self.conns.remove(x)

    def close(self):
        self.stop.set()
        for c in self.conns:
            c.close()
        self.sock.close()


def proc_stats(pid):
    with open(f"/proc/{pid}/status") as f:
        st = dict(line.split(":", 1) for line in f)
    return {"rss_mb": int(st["VmRSS"].split()[0]) / 1024, "threads": int(st["Threads"]),
            "fds": len(os.listdir(f"/proc/{pid}/fd"))}


def slope_mb_per_min(samples):
    """Least-squares slope of RSS over time."""
    xs = [s["t"] / 60 for s in samples]
    ys = [s["rss_mb"] for s in samples]
    n = len(xs)
    mx, my = sum(xs) / n, sum(ys) / n
    den = sum((x - mx) ** 2 for x in xs)
    return sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / den if den else 0.0


@pytest.fixture(scope="module")
def env():
    root = tempfile.mkdtemp(prefix="flapi_soak_")
    data = os.path.join(root, "data")
    rp.generate_data(data)
    shim = rp.build_shim(root)
    if not shim:
        pytest.skip("no C compiler for the network shim")
    sink = Sink()
    shim_log = os.path.join(root, "outbound.log")
    server = rp.Server(rp.current_binary(), data, shim=shim, sink_port=sink.port, shim_log=shim_log,
                       telemetry=True, log_level="warning")
    yield server, sink, shim_log
    server.stop()
    server.cleanup()
    sink.close()
    shutil.rmtree(root, ignore_errors=True)


def test_soak_mixed_load(env):
    server, sink, shim_log = env
    pid = server.proc.pid
    totals = rp.read_totals(server.base)
    matrix = rp.build_matrix(totals)
    s = requests.Session()
    serial = {r.name: rp.digest(r.send(s, server.base)) for r in matrix}
    again = {r.name: rp.digest(r.send(s, server.base)) for r in matrix}
    stable = [r for r in matrix if serial[r.name] == again[r.name]]
    rest = [r for r in stable if r.kind != "mcp"]
    mcp = [r for r in stable if r.kind == "mcp"]

    stop = threading.Event()
    lock = threading.Lock()
    counts = {"requests": 0, "5xx": 0, "mismatch": 0, "transport": 0, "refresh": 0, "gc": 0, "config": 0}
    problems = []

    def record(kind, detail):
        with lock:
            counts[kind] += 1
            if len(problems) < 40:
                problems.append(f"[{kind}] {detail}")

    def check(req, r):
        with lock:
            counts["requests"] += 1
        if r.status_code >= 500:
            record("5xx", f"{req.name}: {r.status_code} {r.content[:200]!r}")
        elif rp.digest(r) != serial[req.name]:
            record("mismatch", f"{req.name}: {r.status_code} {r.content[:200]!r}")

    def client(pool, keepalive, seed):
        rnd = random.Random(seed)
        sess = requests.Session()
        while not stop.is_set():
            req = rnd.choice(pool)
            try:
                if keepalive:
                    r = req.send(sess, server.base)
                else:
                    with requests.Session() as one:
                        one.headers["Connection"] = "close"
                        r = req.send(one, server.base)
                check(req, r)
            except Exception as e:                     # noqa: BLE001 - any transport failure counts
                record("transport", f"{req.name}: {e!r:.200}")

    def unmatched(seed):
        sess = requests.Session()
        while not stop.is_set():
            try:
                r = sess.get(f"{server.base}/nope/{uuid.uuid4().hex}", timeout=30)
                if r.status_code != 404:
                    record("5xx" if r.status_code >= 500 else "mismatch", f"unmatched: {r.status_code}")
                with lock:
                    counts["requests"] += 1
            except Exception as e:                     # noqa: BLE001
                record("transport", f"unmatched: {e!r:.200}")
            time.sleep(0.01)

    def cache_ops():
        sess = requests.Session()
        last_gc = time.time()
        while not stop.is_set():
            try:
                r = sess.post(f"{server.base}/api/v1/_config/endpoints/-sales-daily/cache/refresh",
                              headers=rp.CS_AUTH, timeout=120)
                if r.status_code != 200:
                    record("5xx" if r.status_code >= 500 else "mismatch", f"refresh: {r.status_code} {r.text[:200]}")
                with lock:
                    counts["refresh"] += 1
                if time.time() - last_gc > 30:
                    r = sess.post(f"{server.base}/api/v1/_config/endpoints/-sales-daily/cache/gc",
                                  headers=rp.CS_AUTH, timeout=120)
                    if r.status_code != 200:
                        record("5xx" if r.status_code >= 500 else "mismatch", f"gc: {r.status_code} {r.text[:200]}")
                    with lock:
                        counts["gc"] += 1
                    last_gc = time.time()
            except Exception as e:                     # noqa: BLE001
                record("transport", f"cache op: {e!r:.200}")
            stop.wait(2)

    config_reads = ["/api/v1/_config/endpoints", "/api/v1/_config/endpoints/-customers",
                    "/api/v1/_config/endpoints/-sales-daily/cache", "/api/v1/_config/endpoints/-sales-daily/cache/audit",
                    "/api/v1/_config/metrics", "/api/v1/_config/project", "/api/v1/_config/schema",
                    "/api/v1/_config/endpoints/-customers-:id/template"]

    def config_reader(seed):
        rnd = random.Random(seed)
        sess = requests.Session()
        while not stop.is_set():
            path = rnd.choice(config_reads)
            try:
                r = sess.get(f"{server.base}{path}", headers=rp.CS_AUTH, timeout=60)
                if r.status_code != 200:
                    record("5xx" if r.status_code >= 500 else "mismatch", f"GET {path}: {r.status_code} {r.text[:200]}")
                with lock:
                    counts["config"] += 1
            except Exception as e:                     # noqa: BLE001
                record("transport", f"GET {path}: {e!r:.200}")
            stop.wait(0.05)

    samples = []
    started = time.time()

    def quiescent():
        time.sleep(3)
        return proc_stats(pid)

    jobs = ([(client, rest, True, i) for i in range(20)] + [(client, rest, False, 100 + i) for i in range(4)]
            + [(client, mcp, True, 200 + i) for i in range(6)])
    with cf.ThreadPoolExecutor(len(jobs) + 6) as ex:
        # warm-up: the same load for WARMUP seconds, then a quiescent baseline
        futs = [ex.submit(*j) for j in jobs] + [ex.submit(cache_ops), ex.submit(unmatched, 1)] + \
               [ex.submit(config_reader, i) for i in range(2)]
        while time.time() - started < WARMUP:
            time.sleep(SAMPLE_EVERY)
            samples.append({"t": time.time() - started, **proc_stats(pid)})
        stop.set()
        for f in futs:
            f.result()
        baseline = quiescent()
        stop.clear()
        resumed = time.time()
        futs = [ex.submit(*j) for j in jobs] + [ex.submit(cache_ops), ex.submit(unmatched, 1)] + \
               [ex.submit(config_reader, i) for i in range(2)]
        while time.time() - started < SECONDS:
            time.sleep(SAMPLE_EVERY)
            samples.append({"t": time.time() - started, **proc_stats(pid)})
            assert server.proc.poll() is None, f"server died: {server.output()[-3000:]}"
        stop.set()
        for f in futs:
            f.result()
    final = quiescent()
    took = time.time() - started

    after = [x for x in samples if x["t"] >= WARMUP]
    # Single samples swing by ~120 MB (DuckDB buffers, concurrent large responses): judge the trend
    # on the second half of the run, and the drift as the change of the median between its thirds.
    second_half = [x for x in samples if x["t"] >= took / 2]
    slope = slope_mb_per_min(second_half)
    thirds = [statistics.median([x["rss_mb"] for x in samples if took * i / 3 <= x["t"] < took * (i + 1) / 3])
              for i in range(3)]
    rss = [x["rss_mb"] for x in after]
    report = (f"\nsoak {took:.0f}s: {counts}  ({counts['requests'] / took:.0f} req/s)\n"
              f"RSS after warm-up: min {min(rss):.0f} MB, max {max(rss):.0f} MB, last {rss[-1]:.0f} MB, "
              f"second-half slope {slope:+.2f} MB/min, median by third {[round(t) for t in thirds]}; peak RSS overall {max(x['rss_mb'] for x in samples):.0f} MB\n"
              f"quiescent baseline {baseline} -> final {final}; threads under load "
              f"{min(x['threads'] for x in samples)}..{max(x['threads'] for x in samples)}, fds under load "
              f"{min(x['fds'] for x in samples)}..{max(x['fds'] for x in samples)}\n"
              f"telemetry sink: {sink.accepted} connections, {sink.bytes} bytes received (never answered)")
    print(report)
    print("RSS MB per minute: " + " ".join(f"{x['rss_mb']:.0f}" for x in samples[::12]))
    if os.environ.get("FLAPI_SOAK_SAMPLES"):
        import json
        with open(os.environ["FLAPI_SOAK_SAMPLES"], "w") as f:
            json.dump({"samples": samples, "baseline": baseline, "final": final, "counts": counts}, f)

    t0 = time.time()
    code = server.stop(timeout=60)
    shutdown = time.time() - t0
    log = server.output()
    print(f"SIGTERM -> exit {code} in {shutdown:.1f}s")
    outbound = open(shim_log).read().split() if os.path.exists(shim_log) else []
    print(f"outbound connects redirected to the sink: {len(outbound)} {sorted(set(outbound))[:5]}")

    assert not problems, "\n".join(problems) + report
    assert counts["refresh"] > 0 and counts["config"] > 0 and counts["requests"] > 1000, report
    # Memory plateaus. Bounds from 7/10/12-minute runs on 8 cores (~165 req/s): second-half slope
    # -0.4..+8.3 MB/min, median drift between the last thirds 8..36 MB, quiescent RSS +12..+32 MB.
    # A leak of 1 KB per request would add ~10 MB/min and ~100 MB quiescent over the run.
    assert slope < 12.0, report
    assert thirds[2] - thirds[1] < 64, report
    assert final["rss_mb"] - baseline["rss_mb"] < 48, report
    # quiescent fds and threads come back (a leaked socket or thread per N requests would not)
    assert final["fds"] - baseline["fds"] <= 4, report
    assert final["threads"] - baseline["threads"] <= 2, report
    assert code == 0 and shutdown < 20, f"SIGTERM: exit {code} after {shutdown:.1f}s\n{log[-3000:]}"
    assert "Segmentation" not in log and "AddressSanitizer" not in log, log[-3000:]
