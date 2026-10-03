"""GET /api/v1/_config/endpoints must list every endpoint, including MCP entities.

It keyed the response by url-path. An MCP tool, resource or prompt has no
url-path, so they all landed on the key "" and only the LAST one survived: the
CLI's `mcp tools|resources|prompts list` could not see the others (found by the
CLI/extension review, with the real CLI against a real server).
"""

import os
import subprocess
import tempfile
import time

import pytest
import requests

from otel_helpers import flapi_binary, free_port

TOKEN = "list-mcp-token"


def _w(base, rel, text):
    path = os.path.join(base, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(text)


@pytest.fixture(scope="module")
def server():
    tmp = tempfile.mkdtemp(prefix="flapi_listmcp_")
    port = free_port()
    _w(tmp, "flapi.yaml",
       "project-name: p\nproject-description: d\ntemplate:\n  path: ./sqls\n"
       "connections:\n  inmem:\n    properties:\n      database: ':memory:'\nmcp:\n  enabled: true\n")
    _w(tmp, "sqls/rest.yaml", "url-path: /rest\nmethod: GET\ntemplate-source: rest.sql\nconnection: [inmem]\n")
    _w(tmp, "sqls/rest.sql", "SELECT 1 AS n\n")
    _w(tmp, "sqls/tool.yaml",
       "mcp-tool:\n  name: my_tool\n  description: a tool\n  result_mime_type: application/json\n"
       "template-source: tool.sql\nconnection: [inmem]\n")
    _w(tmp, "sqls/tool.sql", "SELECT 2 AS n\n")
    _w(tmp, "sqls/res.yaml",
       "mcp-resource:\n  name: my_resource\n  description: a resource\n  mime_type: application/json\n"
       "template-source: res.sql\nconnection: [inmem]\n")
    _w(tmp, "sqls/res.sql", "SELECT 3 AS n\n")
    _w(tmp, "sqls/prompt.yaml",
       "mcp-prompt:\n  name: my_prompt\n  description: a prompt\n  template: 'Say hello'\n")
    proc = subprocess.Popen(
        [flapi_binary(), "-c", os.path.join(tmp, "flapi.yaml"), "-p", str(port),
         "--config-service", "--config-service-token", TOKEN, "--log-level", "warning"],
        stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT, cwd=tmp,
        env={**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"})
    base = f"http://127.0.0.1:{port}"
    for _ in range(300):
        try:
            if requests.get(f"{base}/health/live", timeout=2).status_code == 200:
                break
        except requests.RequestException:
            pass
        time.sleep(0.2)
    yield base
    proc.terminate()
    proc.wait(timeout=30)


def test_every_endpoint_kind_is_listed(server):
    r = requests.get(f"{server}/api/v1/_config/endpoints",
                     headers={"Authorization": f"Bearer {TOKEN}"}, timeout=10)
    assert r.status_code == 200, r.text
    listed = r.json()
    kinds = {name: sorted(k for k in v if k in ("mcpTool", "mcpResource", "mcpPrompt", "urlPath"))
             for name, v in listed.items()}
    assert any("mcpTool" in v for v in listed.values()), f"tool missing: {kinds}"
    assert any("mcpResource" in v for v in listed.values()), f"resource missing: {kinds}"
    assert any("mcpPrompt" in v for v in listed.values()), f"prompt missing: {kinds}"
    assert "/rest" in listed, kinds
    assert len(listed) == 4, kinds
