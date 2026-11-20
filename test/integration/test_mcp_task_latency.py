"""Async MCP task end-to-end latency (#215).

A task worker has no request in scope, so the tool-call series measured only the
execution time: a task that waited seconds behind a busy worker looked fast.
The manager now records submit/finish timestamps and publishes the
submit-to-finish latency as its own `mcp_task` series in the metrics endpoint.
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

TOKEN = "task-latency-token"
PV = "io.modelcontextprotocol/protocolVersion"
CC = "io.modelcontextprotocol/clientCapabilities"
META = {PV: "2026-07-28", CC: {"extensions": {"io.modelcontextprotocol/tasks": {}}}}


@pytest.fixture(scope="module")
def server():
    root = tempfile.mkdtemp(prefix="flapi_tasklat_")
    sqls = os.path.join(root, "sqls")
    os.makedirs(sqls)
    with open(os.path.join(root, "flapi.yaml"), "w") as f:
        f.write("""project-name: task-latency
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
  tasks:
    workers: 1
""")
    for name, sql in (("report", "SELECT 7 AS answer"),
                      ("slow", "SELECT count(*) AS n FROM range(100000000000)")):
        with open(os.path.join(sqls, f"{name}.sql"), "w") as f:
            f.write(sql + "\n")
        with open(os.path.join(sqls, f"{name}.yaml"), "w") as f:
            f.write(f"template-source: {name}.sql\nconnection: [inmem]\n"
                    f"mcp-tool: {{name: {name}, description: d, async: true}}\n")
    port = free_port()
    log = open(os.path.join(root, "server.log"), "w")
    proc = subprocess.Popen(
        [flapi_binary(), "-c", "flapi.yaml", "-p", str(port), "--log-level", "warning",
         "--config-service", "--config-service-token", TOKEN],
        cwd=root, stdout=log, stderr=subprocess.STDOUT, preexec_fn=os.setsid,
        env={**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"})
    base = f"http://127.0.0.1:{port}"
    deadline = time.time() + 60
    while time.time() < deadline:
        try:
            if requests.get(f"{base}/health/live", timeout=1).status_code == 200:
                break
        except requests.RequestException:
            time.sleep(0.2)
    else:
        pytest.fail("server never became live")
    yield base
    os.killpg(proc.pid, signal.SIGTERM)
    proc.wait(timeout=20)


def hdr(method, name=None):
    h = {"Content-Type": "application/json", "MCP-Protocol-Version": "2026-07-28", "Mcp-Method": method}
    if name:
        h["Mcp-Name"] = name
    return h


def rpc(base, method, params, name=None):
    return requests.post(f"{base}/mcp/jsonrpc", headers=hdr(method, name), timeout=10,
                         json={"jsonrpc": "2.0", "id": 1, "method": method,
                               "params": {**params, "_meta": META}}).json()


def series(base):
    body = requests.get(f"{base}/api/v1/_config/metrics",
                        headers={"Authorization": f"Bearer {TOKEN}"}, timeout=10).json()
    return body["requests"]["series"]


def test_task_latency_includes_queue_wait(server):
    # One worker. `slow` occupies it; `report` waits in the queue behind it.
    slow = rpc(server, "tools/call", {"name": "slow", "arguments": {}}, "slow")["result"]["task"]["taskId"]
    time.sleep(0.3)
    fast = rpc(server, "tools/call", {"name": "report", "arguments": {}}, "report")["result"]["task"]["taskId"]
    time.sleep(1.5)
    rpc(server, "tasks/cancel", {"taskId": slow})

    deadline = time.time() + 30
    while time.time() < deadline:
        st = rpc(server, "tasks/get", {"taskId": fast}, None)["result"]["task"]["status"]
        if st == "completed":
            break
        time.sleep(0.2)
    else:
        pytest.fail("queued task never completed")
    time.sleep(0.3)

    rows = series(server)
    exec_rows = [r for r in rows if r.get("protocol") == "mcp" and r.get("tool") == "report"]
    task_rows = [r for r in rows if r.get("protocol") == "mcp_task" and r.get("tool") == "report"]
    assert exec_rows, rows
    assert exec_rows[0]["latency_ms"]["max"] < 1000, "execution alone is fast"
    assert task_rows, f"no end-to-end task series: {rows}"
    assert task_rows[0]["requests"] == 1
    assert task_rows[0]["latency_ms"]["max"] >= 1000, (
        f"end-to-end latency must include the ~1.5 s queue wait: {task_rows[0]}")


def test_task_series_does_not_inflate_totals(server):
    body = requests.get(f"{server}/api/v1/_config/metrics",
                        headers={"Authorization": f"Bearer {TOKEN}"}, timeout=10).json()["requests"]
    calls = sum(r["requests"] for r in body["series"] if r.get("protocol") != "mcp_task")
    assert body["total"] == calls
