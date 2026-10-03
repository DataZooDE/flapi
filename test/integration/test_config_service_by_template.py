"""POST /api/v1/_config/endpoints/by-template must find API-created endpoints.

It compared the endpoint's template-source as stored; an endpoint created through the
API keeps it relative to the templates directory, so the VS Code extension's "test
this SQL file" could not find the endpoint it had just created (found while testing
the extension's webviews against a real server).
"""

import os
import subprocess
import tempfile
import time

import pytest
import requests

from otel_helpers import flapi_binary, free_port

TOKEN = "by-template-token"


@pytest.fixture(scope="module")
def server():
    tmp = tempfile.mkdtemp(prefix="flapi_bytemplate_")
    os.makedirs(os.path.join(tmp, "sqls"))
    port = free_port()
    with open(os.path.join(tmp, "flapi.yaml"), "w") as f:
        f.write("project-name: p\nproject-description: d\ntemplate:\n  path: ./sqls\n"
                "connections:\n  inmem:\n    properties:\n      database: ':memory:'\n")
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
    yield base, tmp
    proc.terminate()
    proc.wait(timeout=30)


def test_an_api_created_endpoint_is_found_by_its_absolute_template_path(server):
    base, tmp = server
    h = {"Authorization": f"Bearer {TOKEN}", "Content-Type": "application/json"}
    r = requests.post(f"{base}/api/v1/_config/endpoints", headers=h, timeout=10,
                      json={"url-path": "/made", "method": "GET", "template-source": "made.sql",
                            "connection": ["inmem"]})
    assert r.status_code == 201, r.text
    found = requests.post(f"{base}/api/v1/_config/endpoints/by-template", headers=h, timeout=10,
                          json={"template_path": os.path.join(os.path.realpath(tmp), "sqls", "made.sql")}).json()
    assert found["count"] == 1, found
    assert found["endpoints"][0]["url_path"] == "/made", found
