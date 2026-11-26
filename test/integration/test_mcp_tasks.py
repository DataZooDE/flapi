"""C10-C12: MCP 2026-07-28 Tasks extension.

An async tool returns a taskId immediately (resultType:"task") for a modern
client that declared the tasks capability; the client polls tasks/get until the
task completes. Clients that do not declare the capability — and legacy clients
— always get a synchronous result and never see a task.
"""

import os
import socket
import subprocess
import tempfile
import time
from typing import Iterator

import pytest
import requests

PV = "io.modelcontextprotocol/protocolVersion"
CC = "io.modelcontextprotocol/clientCapabilities"
TASKS_EXT = "io.modelcontextprotocol/tasks"
META_TASKS = {PV: "2026-07-28", CC: {"extensions": {TASKS_EXT: {}}}}
META_NO_TASKS = {PV: "2026-07-28", CC: {}}


def _repo_root() -> str:
    return os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def _flapi_binary() -> str:
    for bt in ("release", "debug"):
        p = os.path.join(_repo_root(), "build", bt, "flapi")
        if os.path.exists(p):
            return p
    pytest.skip("flapi binary not found")


def _free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


@pytest.fixture
def server() -> Iterator[str]:
    binary = _flapi_binary()
    port = _free_port()
    tmp = tempfile.mkdtemp(prefix="flapi_tasks_")
    sqls = os.path.join(tmp, "sqls")
    os.makedirs(sqls)
    with open(os.path.join(tmp, "flapi.yaml"), "w") as f:
        f.write(f"""
project-name: mcp-tasks-test
project-description: tasks E2E
http-port: {port}
template:
  path: ./sqls
connections:
  inmem:
    properties:
      database: ':memory:'
duckdb:
  access_mode: READ_WRITE
  threads: 1
mcp:
  enabled: true
""")
    with open(os.path.join(sqls, "rep.sql"), "w") as f:
        f.write("SELECT 7 AS answer\n")
    with open(os.path.join(sqls, "rep.yaml"), "w") as f:
        f.write("template-source: rep.sql\nconnection: [inmem]\nmcp-tool: {name: report, description: r, async: true}\n")
    # A deliberately long-running query, used to prove tasks/cancel is preemptive
    # (interrupts the in-flight DuckDB query) rather than merely cooperative.
    with open(os.path.join(sqls, "slow.sql"), "w") as f:
        f.write("SELECT count(*) AS n FROM range(100000000000)\n")
    with open(os.path.join(sqls, "slow.yaml"), "w") as f:
        f.write("template-source: slow.sql\nconnection: [inmem]\nmcp-tool: {name: slow, description: s, async: true}\n")

    # A tool whose SQL fails at run time (#275): the task must end `failed`.
    with open(os.path.join(sqls, "broken.sql"), "w") as f:
        f.write("SELECT CAST('not a number' AS INTEGER) AS n\n")
    with open(os.path.join(sqls, "broken.yaml"), "w") as f:
        f.write("template-source: broken.sql\nconnection: [inmem]\nmcp-tool: {name: broken, description: b, async: true}\n")
    with open(os.path.join(sqls, "broken_after.yaml"), "w") as f:
        f.write("template-source: broken.sql\nconnection: [inmem]\n"
                "mcp-tool: {name: broken_after, description: b, async-after-ms: 5000}\n")

    log = open(os.path.join(tmp, "server.log"), "w")
    proc = subprocess.Popen([binary, "-c", os.path.join(tmp, "flapi.yaml"), "--no-telemetry"],
                            cwd=tmp, stdout=log, stderr=subprocess.STDOUT)
    base = f"http://127.0.0.1:{port}"
    try:
        deadline = time.time() + 30
        while time.time() < deadline:
            if proc.poll() is not None:
                raise RuntimeError("flapi exited early")
            try:
                if requests.get(f"{base}/mcp/health", timeout=1).status_code < 500:
                    break
            except requests.exceptions.RequestException:
                time.sleep(0.5)
        yield base
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
        log.close()


def _headers(method, name=None, version="2026-07-28"):
    h = {"Content-Type": "application/json", "MCP-Protocol-Version": version, "Mcp-Method": method}
    if name:
        h["Mcp-Name"] = name
    return h


def _call(base, meta, name="report"):
    return requests.post(f"{base}/mcp/jsonrpc", headers=_headers("tools/call", name),
                         json={"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                               "params": {"name": name, "arguments": {}, "_meta": meta}}, timeout=10)


def _get(base, meta, task_id):
    return requests.post(f"{base}/mcp/jsonrpc", headers=_headers("tasks/get"),
                         json={"jsonrpc": "2.0", "id": 2, "method": "tasks/get",
                               "params": {"taskId": task_id, "_meta": meta}}, timeout=10)


@pytest.mark.standalone_server
class TestTasks:
    def test_async_tool_returns_task_then_completes(self, server):
        r = _call(server, META_TASKS)
        assert r.status_code == 200, r.text
        result = r.json()["result"]
        assert result["resultType"] == "task"
        task_id = result["task"]["taskId"]
        assert result["task"]["status"] in ("working", "completed")

        # Poll to completion.
        completed = None
        for _ in range(40):
            g = _get(server, META_TASKS, task_id).json()["result"]
            if g["task"]["status"] == "completed":
                completed = g
                break
            time.sleep(0.25)
        assert completed is not None, "task did not complete in time"
        assert "7" in completed["result"]["content"][0]["text"]

    def test_client_without_tasks_capability_is_synchronous(self, server):
        r = _call(server, META_NO_TASKS)
        result = r.json()["result"]
        # No task handle — a normal completed result with the rows inline.
        assert result["resultType"] == "complete"
        assert "task" not in result
        assert "7" in result["content"][0]["text"]

    def test_legacy_client_is_synchronous(self, server):
        # No _meta at all -> legacy path, always synchronous, no task.
        r = requests.post(f"{server}/mcp/jsonrpc", headers={"Content-Type": "application/json"},
                          json={"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                                "params": {"name": "report", "arguments": {}}}, timeout=10)
        result = r.json()["result"]
        assert "task" not in result
        assert "7" in result["content"][0]["text"]

    def test_tasks_get_unknown_id_is_not_found(self, server):
        g = _get(server, META_TASKS, "task_does_not_exist")
        assert "error" in g.json()
        assert g.json()["error"]["code"] == -32602

    def test_tasks_cancel(self, server):
        r = _call(server, META_TASKS)
        task_id = r.json()["result"]["task"]["taskId"]
        c = requests.post(f"{server}/mcp/jsonrpc", headers=_headers("tasks/cancel"),
                          json={"jsonrpc": "2.0", "id": 3, "method": "tasks/cancel",
                                "params": {"taskId": task_id, "_meta": META_TASKS}}, timeout=10)
        body = c.json()
        assert "error" not in body, body
        assert body["result"]["taskId"] == task_id

    def test_tasks_cancel_preempts_long_query(self, server):
        # Submit a query that would run for many minutes, then cancel it. With
        # preemptive cancellation (issue #111) the in-flight DuckDB query is
        # interrupted, so the task reaches `cancelled` in seconds rather than
        # hanging until the query finishes on its own.
        r = _call(server, META_TASKS, name="slow")
        result = r.json()["result"]
        assert result["resultType"] == "task", result
        task_id = result["task"]["taskId"]

        # Let the worker actually start the query before cancelling.
        time.sleep(0.5)
        c = requests.post(f"{server}/mcp/jsonrpc", headers=_headers("tasks/cancel"),
                          json={"jsonrpc": "2.0", "id": 3, "method": "tasks/cancel",
                                "params": {"taskId": task_id, "_meta": META_TASKS}}, timeout=10)
        assert "error" not in c.json(), c.text

        # Must transition to cancelled quickly — proof the query was interrupted.
        cancelled = _wait_status(server, task_id, "cancelled", tries=20)
        assert cancelled is not None, "long query was not cancelled preemptively"

    def test_failing_tool_ends_task_failed_with_request_id(self, server):
        # #275: the work returned an isError envelope instead of throwing, so the
        # task ended `completed`; and the worker had no request context, so the
        # error text said "...in the server log." without a request id.
        r = _call(server, META_TASKS, name="broken")
        assert r.status_code == 200, r.text
        result = r.json()["result"]
        assert result["resultType"] == "task", result
        task_id = result["task"]["taskId"]
        call_request_id = r.headers.get("X-Request-Id")
        assert call_request_id, dict(r.headers)

        terminal = None
        for _ in range(40):
            g = _get(server, META_TASKS, task_id).json()["result"]
            if g["task"]["status"] != "working":
                terminal = g
                break
            time.sleep(0.25)
        assert terminal is not None, "task did not finish"
        assert terminal["task"]["status"] == "failed", terminal
        # The error names the request id of the tools/call that submitted it.
        assert f"request_id {call_request_id}" in terminal["error"], terminal
        # The tool's own isError result is still there for the client.
        assert terminal["result"]["isError"] is True, terminal
        assert call_request_id in terminal["result"]["content"][0]["text"], terminal

    def test_failing_async_after_tool_answers_inline_with_is_error(self, server):
        # async-after: a failure inside the grace window is answered inline, as
        # a synchronous call would be, not handed back as a task.
        r = _call(server, META_TASKS, name="broken_after")
        assert r.status_code == 200, r.text
        result = r.json()["result"]
        assert result["resultType"] == "complete", result
        assert result["isError"] is True, result
        assert "request_id req-" in result["content"][0]["text"], result


def _wait_status(base, task_id, want, tries=40):
    for _ in range(tries):
        g = _get(base, META_TASKS, task_id).json().get("result", {})
        if g.get("task", {}).get("status") == want:
            return g
        time.sleep(0.25)
    return None


@pytest.mark.standalone_server
class TestTasksDurability:
    """A task persists to a file-backed DuckDB and survives a server restart."""

    def _boot(self, binary, tmp, port):
        log = open(os.path.join(tmp, f"server-{port}-{time.time()}.log"), "w")
        proc = subprocess.Popen([binary, "-c", os.path.join(tmp, "flapi.yaml"), "--no-telemetry"],
                                cwd=tmp, stdout=log, stderr=subprocess.STDOUT)
        base = f"http://127.0.0.1:{port}"
        deadline = time.time() + 30
        while time.time() < deadline:
            if proc.poll() is not None:
                raise RuntimeError("flapi exited early")
            try:
                if requests.get(f"{base}/mcp/health", timeout=1).status_code < 500:
                    return proc, base, log
            except requests.exceptions.RequestException:
                time.sleep(0.5)
        raise RuntimeError("flapi did not become healthy")

    def test_task_survives_restart(self):
        binary = _flapi_binary()
        port = _free_port()
        tmp = tempfile.mkdtemp(prefix="flapi_taskdur_")
        sqls = os.path.join(tmp, "sqls")
        os.makedirs(sqls)
        with open(os.path.join(tmp, "flapi.yaml"), "w") as f:
            f.write(f"""
project-name: mcp-taskdur-test
project-description: task durability
http-port: {port}
template:
  path: ./sqls
connections:
  inmem:
    properties:
      database: ':memory:'
duckdb:
  db_path: {os.path.join(tmp, 'flapi.db')}
  access_mode: READ_WRITE
  threads: 1
mcp:
  enabled: true
""")
        with open(os.path.join(sqls, "rep.sql"), "w") as f:
            f.write("SELECT 7 AS answer\n")
        with open(os.path.join(sqls, "rep.yaml"), "w") as f:
            f.write("template-source: rep.sql\nconnection: [inmem]\nmcp-tool: {name: report, description: r, async: true}\n")

        # Boot 1: create a task, wait for it to complete (and persist).
        proc, base, log = self._boot(binary, tmp, port)
        try:
            r = _call(base, META_TASKS)
            task_id = r.json()["result"]["task"]["taskId"]
            assert _wait_status(base, task_id, "completed") is not None
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
            log.close()

        # Boot 2 (same db_path): the task is recovered and still queryable.
        proc, base, log = self._boot(binary, tmp, port)
        try:
            g = _get(base, META_TASKS, task_id).json()["result"]
            assert g["task"]["status"] == "completed"
            assert "7" in g["result"]["content"][0]["text"]
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
            log.close()
