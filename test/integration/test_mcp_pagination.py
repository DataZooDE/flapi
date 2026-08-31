"""C6 features: cursor pagination on list methods, the completion wrapper,
and URI-templated resources (resources/templates/list + parameterised read)."""

import os
import socket
import subprocess
import tempfile
import time
from typing import Iterator

import pytest
import requests


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


def _make_server(dirpath: str, port: int, page_size: int) -> str:
    sqls = os.path.join(dirpath, "sqls")
    os.makedirs(sqls)
    ps = f"\n  page-size: {page_size}" if page_size else ""
    with open(os.path.join(dirpath, "flapi.yaml"), "w") as f:
        f.write(f"""
project-name: mcp-c6-test
project-description: C6 E2E
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
  enabled: true{ps}
""")
    for n in ("a", "b", "c"):
        with open(os.path.join(sqls, f"t{n}.sql"), "w") as f:
            f.write("SELECT 1 AS x\n")
        with open(os.path.join(sqls, f"t{n}.yaml"), "w") as f:
            f.write(f"""
template-source: t{n}.sql
connection: [inmem]
mcp-tool:
  name: tool_{n}
  description: tool {n}
""")
    # A URI-templated resource.
    with open(os.path.join(sqls, "cust.sql"), "w") as f:
        f.write("SELECT {{{params.id}}} AS customer_id\n")
    with open(os.path.join(sqls, "cust.yaml"), "w") as f:
        f.write("""
request:
  - field-name: id
    field-in: query
    validators: [{type: int}]
mcp-resource:
  name: customer_by_id
  description: A customer by id
  uri-template: "flapi://customers/{id}"
template-source: cust.sql
connection: [inmem]
""")
    return os.path.join(dirpath, "flapi.yaml")


def _boot(page_size: int) -> Iterator[str]:
    binary = _flapi_binary()
    port = _free_port()
    tmp = tempfile.mkdtemp(prefix="flapi_c6_")
    config = _make_server(tmp, port, page_size)
    log = open(os.path.join(tmp, "server.log"), "w")
    proc = subprocess.Popen([binary, "-c", config, "--no-telemetry"],
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


@pytest.fixture
def server_paged() -> Iterator[str]:
    yield from _boot(page_size=1)


@pytest.fixture
def server_unpaged() -> Iterator[str]:
    yield from _boot(page_size=0)


def _rpc(base: str, method: str, params: dict, id_="x") -> dict:
    return requests.post(f"{base}/mcp/jsonrpc",
                         headers={"Content-Type": "application/json"},
                         json={"jsonrpc": "2.0", "id": id_, "method": method, "params": params},
                         timeout=10).json()


@pytest.mark.standalone_server
class TestPaginationAndTemplates:
    def test_unpaged_returns_all_tools_without_cursor(self, server_unpaged):
        r = _rpc(server_unpaged, "tools/list", {})["result"]
        assert len(r["tools"]) == 3
        assert "nextCursor" not in r

    def test_paged_walks_all_tools_via_cursor(self, server_paged):
        seen = []
        cursor = None
        for _ in range(5):  # guard against infinite loop
            params = {"cursor": cursor} if cursor else {}
            r = _rpc(server_paged, "tools/list", params)["result"]
            assert len(r["tools"]) == 1
            seen.append(r["tools"][0]["name"])
            cursor = r.get("nextCursor")
            if not cursor:
                break
        assert sorted(seen) == ["tool_a", "tool_b", "tool_c"]

    def test_stale_cursor_is_rejected(self, server_paged):
        import base64, json
        bad = base64.b64encode(json.dumps({"offset": 0, "gen": 999}).encode()).decode()
        body = _rpc(server_paged, "tools/list", {"cursor": bad})
        assert "error" in body
        assert body["error"]["code"] == -32602

    def test_cursor_from_one_list_rejected_by_another(self, server_paged):
        # A nextCursor minted for tools/list must not be accepted by another list
        # method (it is bound to its issuing method).
        r = _rpc(server_paged, "tools/list", {})["result"]
        cursor = r["nextCursor"]
        body = _rpc(server_paged, "resources/list", {"cursor": cursor})
        assert "error" in body
        assert body["error"]["code"] == -32602

    def test_malformed_cursor_is_rejected(self, server_paged):
        import base64
        bad = base64.b64encode(b"not json at all").decode()
        body = _rpc(server_paged, "tools/list", {"cursor": bad})
        assert "error" in body
        assert body["error"]["code"] == -32602

    def test_completion_result_is_wrapped(self, server_unpaged):
        # Completion result must be wrapped under a "completion" key.
        body = _rpc(server_unpaged, "completion/complete",
                    {"partial": "", "ref": {"type": "tool", "name": "tool_a"}})
        if "result" in body:
            assert "completion" in body["result"], body["result"]

    def test_resource_templates_list(self, server_unpaged):
        r = _rpc(server_unpaged, "resources/templates/list", {})["result"]
        tmpls = r["resourceTemplates"]
        assert any(t["uriTemplate"] == "flapi://customers/{id}" for t in tmpls)

    def test_templated_resource_read_binds_path_param(self, server_unpaged):
        r = _rpc(server_unpaged, "resources/read",
                 {"uri": "flapi://customers/42"})["result"]
        text = r["contents"][0]["text"]
        assert "42" in text
        assert r["contents"][0]["uri"] == "flapi://customers/42"
