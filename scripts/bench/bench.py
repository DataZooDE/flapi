#!/usr/bin/env python3
"""Black-box benchmark of a flapi binary: latency, throughput, startup and memory.

    scripts/bench/bench.py --binary build/release/flapi --out /tmp/new.json [--quick]
    scripts/bench/compare.py old.json new.json        # exits 1 past a threshold

Pure standard library, real HTTP against a real server. Run one benchmark at a time
on an idle machine; the harness takes the median of --runs runs per scenario.
"""
import argparse
import http.client
import json
import os
import shutil
import signal
import socket
import statistics
import subprocess
import tempfile
import threading
import time
from pathlib import Path

ENDPOINTS = 50
ROWS = 2_000_000


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def write_project(root: Path, endpoints: int, with_init: bool):
    (root / "sqls").mkdir(parents=True, exist_ok=True)
    (root / "data").mkdir(exist_ok=True)
    init = ""
    if with_init:
        init = (
            "    init: |\n"
            f"      COPY (SELECT i AS id, i % 1000 AS grp, (i * 7919) % 100003 AS val, "
            f"'name_' || (i % 5000) AS name FROM range({ROWS}) t(i)) "
            "TO 'data/big.parquet' (FORMAT parquet);\n"
        )
    (root / "flapi.yaml").write_text(
        "project-name: bench\n"
        "template:\n  path: ./sqls\n"
        "connections:\n  big:\n" + init +
        "    properties:\n      path: ./data/big.parquet\n"
        "duckdb:\n  db_path: ./bench.db\n"
        "log-level: warning\n"
    )
    if with_init:
        return
    for i in range(endpoints):
        (root / "sqls" / f"ep{i}.yaml").write_text(
            f"url-path: /ep{i}\nmethod: GET\ntemplate-source: ep{i}.sql\nconnection: [big]\n"
            "request:\n  - field-name: n\n    field-in: query\n    required: false\n"
            "    validators:\n      - type: int\n        min: 0\n        max: 1000000\n"
        )
        (root / "sqls" / f"ep{i}.sql").write_text("SELECT {{{ params.n }}}::INT + %d AS v" % i if False else
                                                   "SELECT 1 AS v")
    scenarios = {
        "scan": "SELECT grp, count(*) AS c, avg(val) AS a FROM read_parquet('{{{ conn.path }}}') "
                "WHERE grp < 50 GROUP BY grp ORDER BY grp",
        "paged": "SELECT id, name, val FROM read_parquet('{{{ conn.path }}}') WHERE grp = 7",
        "point": "SELECT id, name, val FROM read_parquet('{{{ conn.path }}}') WHERE id = 123456",
    }
    for name, sql in scenarios.items():
        (root / "sqls" / f"{name}.yaml").write_text(
            f"url-path: /{name}\nmethod: GET\ntemplate-source: {name}.sql\nconnection: [big]\n"
        )
        (root / "sqls" / f"{name}.sql").write_text(sql)


def rss_kb(pid):
    try:
        for line in Path(f"/proc/{pid}/status").read_text().splitlines():
            if line.startswith("VmRSS:"):
                return int(line.split()[1])
    except OSError:
        pass
    return 0


def proc_info(pid):
    info = {"rss_kb": rss_kb(pid)}
    try:
        info["fds"] = len(os.listdir(f"/proc/{pid}/fd"))
        info["threads"] = len(os.listdir(f"/proc/{pid}/task"))
        for line in Path(f"/proc/{pid}/status").read_text().splitlines():
            if line.startswith("VmHWM:"):
                info["hwm_kb"] = int(line.split()[1])
    except OSError:
        pass
    return info


def start(binary, root, port):
    env = dict(os.environ, FLAPI_EXTENSION_DIRECTORY=os.environ.get("FLAPI_EXTENSION_DIRECTORY", str(Path.home() / ".duckdb/extensions")))
    t0 = time.time()
    proc = subprocess.Popen([binary, "-c", str(root / "flapi.yaml"), "--port", str(port)],
                            cwd=root, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env)
    deadline = t0 + 300
    while time.time() < deadline:
        if proc.poll() is not None:
            raise SystemExit("flapi exited during startup")
        try:
            c = http.client.HTTPConnection("127.0.0.1", port, timeout=2)
            c.request("GET", "/health/live")
            if c.getresponse().status == 200:
                return proc, time.time() - t0
        except OSError:
            time.sleep(0.05)
    raise SystemExit("startup timeout")


def stop(proc):
    proc.send_signal(signal.SIGTERM)
    try:
        proc.wait(20)
    except subprocess.TimeoutExpired:
        proc.kill()


def load(port, path, conc, seconds):
    lat, errors = [], [0]
    lock = threading.Lock()
    stop_at = time.time() + seconds

    def worker():
        mine = []
        c = http.client.HTTPConnection("127.0.0.1", port, timeout=30)
        while time.time() < stop_at:
            t = time.perf_counter()
            try:
                c.request("GET", path)
                r = c.getresponse()
                r.read()
                if r.status >= 500:
                    with lock:
                        errors[0] += 1
            except OSError:
                with lock:
                    errors[0] += 1
                c = http.client.HTTPConnection("127.0.0.1", port, timeout=30)
            mine.append((time.perf_counter() - t) * 1000)
        with lock:
            lat.extend(mine)

    threads = [threading.Thread(target=worker) for _ in range(conc)]
    t0 = time.time()
    for t in threads:
        t.start()
    peak = [0]

    def sampler():
        while any(t.is_alive() for t in threads):
            peak[0] = max(peak[0], rss_kb(PID[0]))
            time.sleep(0.2)

    s = threading.Thread(target=sampler)
    s.start()
    for t in threads:
        t.join()
    s.join()
    wall = time.time() - t0
    lat.sort()
    q = lambda p: lat[min(len(lat) - 1, int(len(lat) * p))] if lat else None
    return {"requests": len(lat), "rps": len(lat) / wall, "p50_ms": q(0.50), "p95_ms": q(0.95),
            "p99_ms": q(0.99), "errors": errors[0], "peak_rss_kb": peak[0]}


PID = [0]


def median_of(runs):
    out = {}
    for k in runs[0]:
        vals = [r[k] for r in runs if r[k] is not None]
        out[k] = statistics.median(vals) if vals else None
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--quick", action="store_true")
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--seconds", type=float, default=5)
    a = ap.parse_args()
    runs, secs = (1, 2) if a.quick else (a.runs, a.seconds)
    binary = str(Path(a.binary).resolve())
    work = Path(tempfile.mkdtemp(prefix="flapi-bench-"))
    result = {"binary": binary, "endpoints": ENDPOINTS, "rows": ROWS, "scenarios": {}}
    try:
        # generate the dataset once with a throwaway server (init runs COPY)
        write_project(work, 0, True)
        port = free_port()
        proc, _ = start(binary, work, port)
        stop(proc)
        shutil.rmtree(work / "bench.db", ignore_errors=True)
        (work / "bench.db").unlink(missing_ok=True)
        write_project(work, ENDPOINTS, False)

        startups, idle = [], []
        for _ in range(runs):
            port = free_port()
            proc, t = start(binary, work, port)
            PID[0] = proc.pid
            time.sleep(1)
            startups.append(t)
            idle.append(rss_kb(proc.pid))
            stop(proc)
        result["startup_s"] = statistics.median(startups)
        result["idle_rss_kb"] = statistics.median(idle)

        port = free_port()
        proc, _ = start(binary, work, port)
        PID[0] = proc.pid
        for path in ("/ep0", "/scan", "/paged", "/point"):  # warm up (extensions, page cache)
            load(port, path, 1, 1)
        for path in ("/ep0", "/scan", "/paged", "/point"):
            for conc in (1, 8, 64):
                rs = [load(port, path, conc, secs) for _ in range(runs)]
                result["scenarios"][f"{path[1:]}@{conc}"] = median_of(rs)
        time.sleep(2)
        result["after_load"] = proc_info(proc.pid)
        stop(proc)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    Path(a.out).write_text(json.dumps(result, indent=2))
    print(json.dumps({k: v for k, v in result.items() if k != "scenarios"}, indent=2))
    for k, v in result["scenarios"].items():
        print(f"{k:12s} rps={v['rps']:9.1f} p50={v['p50_ms']:.2f} p95={v['p95_ms']:.2f} p99={v['p99_ms']:.2f} err={v['errors']:.0f} peak_rss={v['peak_rss_kb']/1024:.0f}MB")


if __name__ == "__main__":
    main()
