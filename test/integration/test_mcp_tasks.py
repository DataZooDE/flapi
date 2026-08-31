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
