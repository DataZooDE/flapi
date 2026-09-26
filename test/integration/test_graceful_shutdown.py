"""A clean SIGTERM must visibly detach DuckLake and checkpoint (#147).

The graceful shutdown - DETACH the DuckLake catalog, CHECKPOINT the WAL - used
to live in ~DatabaseManager. That destructor runs from a shared_ptr release
inside exit(), after the statics SQL execution depends on have been destroyed.
On a failed startup it segfaulted 6 times in 12 (#145); on a normal shutdown it
had a luckier destruction order, and nothing ever showed whether it worked.

These tests assert the shutdown the way an operator would need to see it: the
log says it happened, while logging is still alive, and a restart can take the
DuckLake lock immediately.
"""

import os
import signal
import subprocess
import tempfile
import time

import requests

from otel_helpers import flapi_binary, free_port


def _write_config(tmp, port):
    os.makedirs(os.path.join(tmp, "sqls"), exist_ok=True)
    os.makedirs(os.path.join(tmp, "data"), exist_ok=True)
    with open(os.path.join(tmp, "sqls", "t.yaml"), "w") as f:
        f.write("url-path: /t\nmethod: GET\n"
                "template-source: t.sql\nconnection: [inmem]\n")
    with open(os.path.join(tmp, "sqls", "t.sql"), "w") as f:
        f.write("SELECT 1 AS n\n")
    path = os.path.join(tmp, "flapi.yaml")
    with open(path, "w") as f:
        f.write(
            "project-name: graceful-shutdown\n"
            "project-description: DuckLake is detached while the process lives\n"
            f"http-port: {port}\n"
            "template:\n  path: ./sqls\n"
            "duckdb:\n"
            f"  db_path: {os.path.join(tmp, 'flapi.db')}\n"
            "ducklake:\n  enabled: true\n  alias: cache\n"
            f"  metadata-path: {os.path.join(tmp, 'meta.ducklake')}\n"
            f"  data-path: {os.path.join(tmp, 'data')}\n"
            "connections:\n  inmem:\n    properties:\n      database: ':memory:'\n")
    return path


def _start(cfg, tmp, port, log_path):
    env = {**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"}
    return subprocess.Popen(
        [flapi_binary(), "-c", cfg, "-p", str(port), "--log-level", "info"],
        stdout=open(log_path, "w"), stderr=subprocess.STDOUT,
        cwd=tmp, env=env)


def _wait_live(port, proc, timeout=60):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if proc.poll() is not None:
            return False
        try:
            if requests.get(f"http://127.0.0.1:{port}/health/live",
                            timeout=1).status_code == 200:
                return True
        except requests.RequestException:
            pass
        time.sleep(0.2)
    return False


def _terminate(proc):
    if proc.poll() is None:
        proc.send_signal(signal.SIGTERM)
        try:
            return proc.wait(timeout=60)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
            return None
    return proc.returncode


class TestGracefulShutdown:

    def test_sigterm_detaches_and_checkpoints_while_logging_is_alive(self):
        tmp = tempfile.mkdtemp(prefix="flapi_graceful_")
        port = free_port()
        cfg = _write_config(tmp, port)
        log_path = os.path.join(tmp, "server.log")
        proc = _start(cfg, tmp, port, log_path)
        try:
            assert _wait_live(port, proc), open(log_path).read()[-3000:]
            code = _terminate(proc)
            log = open(log_path).read()

            assert code == 0, f"SIGTERM exited {code}\n" + log[-3000:]
            # The whole point of #147: these used to run in a destructor
            # during exit(), and never once appeared in a real shutdown log.
            assert "Detached DuckLake catalog: cache" in log, (
                "a clean SIGTERM did not visibly detach DuckLake\n" + log[-3000:])
            assert "Checkpointed database" in log, (
                "a clean SIGTERM did not visibly checkpoint\n" + log[-3000:])
        finally:
            _terminate(proc)

    def test_a_restart_takes_the_ducklake_lock_immediately(self):
        """The operational property: a restart must not race the previous
        process for the DuckLake file lock. With #145, losing that race is a
        prompt, logged non-zero exit - better than a core dump, but still an
        outage a clean shutdown should never cause."""
        tmp = tempfile.mkdtemp(prefix="flapi_graceful_restart_")
        port = free_port()
        cfg = _write_config(tmp, port)
        first = _start(cfg, tmp, port, os.path.join(tmp, "first.log"))
        try:
            assert _wait_live(port, first)
            assert _terminate(first) == 0
        finally:
            _terminate(first)

        second_log = os.path.join(tmp, "second.log")
        port2 = free_port()
        second = _start(cfg, tmp, port2, second_log)
        try:
            assert _wait_live(port2, second), (
                "a restart immediately after a clean shutdown could not start\n"
                + open(second_log).read()[-3000:])
        finally:
            _terminate(second)


def _write_tasks_config(tmp, port):
    """_write_config plus an async MCP tool whose query runs for minutes."""
    cfg = _write_config(tmp, port)
    with open(cfg, "a") as f:
        f.write("mcp:\n  enabled: true\n")
    with open(os.path.join(tmp, "sqls", "slow.sql"), "w") as f:
        f.write("SELECT count(*) AS n FROM range(100000000000)\n")
    with open(os.path.join(tmp, "sqls", "slow.yaml"), "w") as f:
        f.write("template-source: slow.sql\nconnection: [inmem]\n"
                "mcp-tool: {name: slow, description: s, async: true}\n")
    return cfg


class TestShutdownWithAnAsyncTaskInFlight:
    """MCP async tasks run on the task manager's OWN worker threads, not the
    handler pool, and are stopped only by ~MCPTaskManager at exit - i.e. AFTER
    main() now closes the database (#147). This pins that the ordering is safe.

    It is, and for a reason worth recording: duckdb_close() releases the
    handle, but DuckDB refcounts the database instance through its open
    connections, so the task's running query keeps it alive until the task
    manager interrupts and joins it. Measured with the task confirmed
    `working` at SIGTERM: 15 of 15 exited 0, in about 60ms each."""

    def test_sigterm_with_a_long_task_running_interrupts_it_and_exits_cleanly(self):
        tmp = tempfile.mkdtemp(prefix="flapi_graceful_task_")
        port = free_port()
        cfg = _write_tasks_config(tmp, port)
        log_path = os.path.join(tmp, "server.log")
        proc = _start(cfg, tmp, port, log_path)
        try:
            assert _wait_live(port, proc), open(log_path).read()[-3000:]
            meta = {"io.modelcontextprotocol/protocolVersion": "2026-07-28",
                    "io.modelcontextprotocol/clientCapabilities":
                        {"extensions": {"io.modelcontextprotocol/tasks": {}}}}
            r = requests.post(
                f"http://127.0.0.1:{port}/mcp/jsonrpc",
                headers={"Content-Type": "application/json",
                         "MCP-Protocol-Version": "2026-07-28",
                         "Mcp-Method": "tools/call", "Mcp-Name": "slow"},
                json={"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                      "params": {"name": "slow", "arguments": {}, "_meta": meta}},
                timeout=10)
            result = r.json()["result"]
            assert result["resultType"] == "task", result
            task_id = result["task"]["taskId"]
            time.sleep(1.0)   # let the task's query actually start

            # Without this the test could go vacuous: if the task had already
            # finished, SIGTERM would find nothing in flight and pass trivially.
            g = requests.post(
                f"http://127.0.0.1:{port}/mcp/jsonrpc",
                headers={"Content-Type": "application/json",
                         "MCP-Protocol-Version": "2026-07-28",
                         "Mcp-Method": "tasks/get"},
                json={"jsonrpc": "2.0", "id": 2, "method": "tasks/get",
                      "params": {"taskId": task_id, "_meta": meta}},
                timeout=10).json()["result"]
            assert g["task"]["status"] == "working", (
                f"the task was not running when SIGTERM was sent: {g}")

            started = time.time()
            proc.send_signal(signal.SIGTERM)
            try:
                code = proc.wait(timeout=60)
            except subprocess.TimeoutExpired:
                code = None
            elapsed = time.time() - started
            log = open(log_path).read()

            assert code is not None, (
                "SIGTERM with a long task running did not exit within 60s - "
                "the task's query was not interrupted\n" + log[-3000:])
            assert code == 0, (
                f"SIGTERM with a task in flight exited {code} (negative = "
                "killed by a signal, e.g. a crash in teardown)\n" + log[-3000:])
            assert elapsed < 30, f"shutdown took {elapsed:.1f}s\n" + log[-3000:]
            assert "Detached DuckLake catalog: cache" in log, log[-3000:]
        finally:
            _terminate(proc)
