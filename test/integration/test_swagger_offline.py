"""Swagger UI at /doc works offline (#217).

The page loaded swagger-ui-dist from unpkg.com, so /doc was blank on an
air-gapped host. The assets are now vendored in the binary and served from
/doc/assets/.
"""
import os
import re
import signal
import subprocess
import tempfile
import time

import pytest
import requests

from otel_helpers import flapi_binary, free_port

pytestmark = pytest.mark.standalone_server

CONFIG = """\
project-name: swagger-offline
template:
  path: ./sqls
connections:
  inmem:
    properties:
      database: ':memory:'
"""


@pytest.fixture(scope="module")
def base():
    root = tempfile.mkdtemp(prefix="flapi_swagger_")
    os.makedirs(os.path.join(root, "sqls"))
    with open(os.path.join(root, "flapi.yaml"), "w") as f:
        f.write(CONFIG)
    port = free_port()
    log = open(os.path.join(root, "server.log"), "w")
    proc = subprocess.Popen(
        [flapi_binary(), "-c", "flapi.yaml", "-p", str(port)],
        cwd=root, stdout=log, stderr=subprocess.STDOUT, preexec_fn=os.setsid,
        env={**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"})
    url = f"http://127.0.0.1:{port}"
    deadline = time.time() + 60
    while time.time() < deadline:
        try:
            if requests.get(f"{url}/health/live", timeout=1).status_code == 200:
                break
        except requests.RequestException:
            time.sleep(0.2)
    else:
        pytest.fail("server never became live")
    yield url
    os.killpg(proc.pid, signal.SIGTERM)
    proc.wait(timeout=20)


def asset_refs(html):
    return re.findall(r'(?:src|href)\s*=\s*"([^"]+)"', html)


class TestSwaggerOffline:
    def test_doc_references_no_external_host(self, base):
        r = requests.get(f"{base}/doc", timeout=10)
        assert r.status_code == 200
        refs = asset_refs(r.text)
        assert refs, "expected script/link references in /doc"
        external = [u for u in refs if re.match(r"(https?:)?//", u)]
        assert not external, f"/doc loads from external hosts: {external}"
        assert "unpkg.com" not in r.text

    def test_every_local_asset_is_served(self, base):
        r = requests.get(f"{base}/doc", timeout=10)
        local = [u for u in asset_refs(r.text) if u.startswith("/")]
        assert len(local) >= 3, local
        for u in local:
            a = requests.get(f"{base}{u}", timeout=10)
            assert a.status_code == 200, f"{u} -> {a.status_code}"
            assert len(a.content) > 1000, f"{u} suspiciously small"
        types = {u: requests.get(f"{base}{u}", timeout=10).headers.get("Content-Type", "")
                 for u in local}
        for u, t in types.items():
            if u.endswith(".js"):
                assert "javascript" in t, (u, t)
            if u.endswith(".css"):
                assert "css" in t, (u, t)

    def test_unknown_asset_is_404_and_traversal_is_refused(self, base):
        assert requests.get(f"{base}/doc/assets/nope.js", timeout=10).status_code == 404
        assert requests.get(f"{base}/doc/assets/../../etc/passwd", timeout=10).status_code in (400, 404)
