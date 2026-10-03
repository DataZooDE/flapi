"""template/expand?validate_only must validate the text it is GIVEN.

The VS Code extension's "Validate SQL Template" validated the saved YAML and then
announced that the SQL was valid. The server could only validate the template on disk,
so unsaved edits were never checked. `validate_only` now takes an optional `template`
and validates that, without writing anything.
"""

import os
import subprocess
import tempfile
import time

import pytest
import requests

from otel_helpers import flapi_binary, free_port

TOKEN = "validate-unsaved-token"


@pytest.fixture(scope="module")
def server():
    tmp = tempfile.mkdtemp(prefix="flapi_valtpl_")
    os.makedirs(os.path.join(tmp, "sqls"))
    port = free_port()
    with open(os.path.join(tmp, "flapi.yaml"), "w") as f:
        f.write("project-name: p\nproject-description: d\ntemplate:\n  path: ./sqls\n"
                "connections:\n  inmem:\n    properties:\n      database: ':memory:'\n")
    with open(os.path.join(tmp, "sqls", "v.yaml"), "w") as f:
        f.write("url-path: /v\nmethod: GET\ntemplate-source: v.sql\nconnection: [inmem]\n")
    with open(os.path.join(tmp, "sqls", "v.sql"), "w") as f:
        f.write("SELECT 1 AS stored\n")
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


def _validate(base, body):
    r = requests.post(f"{base}/api/v1/_config/endpoints/-v/template/expand?validate_only=1",
                      headers={"Authorization": f"Bearer {TOKEN}", "Content-Type": "application/json"},
                      json=body, timeout=15)
    assert r.status_code == 200, r.text
    return r.json()


def test_the_stored_template_is_still_validated_without_a_candidate(server):
    base, _ = server
    assert _validate(base, {"parameters": {}})["valid"] is True


def test_valid_unsaved_sql_is_valid(server):
    base, _ = server
    assert _validate(base, {"parameters": {}, "template": "SELECT 42 AS answer"})["valid"] is True


def test_broken_unsaved_sql_is_reported_even_though_the_saved_file_is_fine(server):
    base, _ = server
    result = _validate(base, {"parameters": {}, "template": "SELEC FROM nowhere"})
    assert result["valid"] is False, result
    assert any(e["type"] == "sql_syntax" for e in result["errors"]), result


def test_a_broken_mustache_template_is_reported(server):
    base, _ = server
    result = _validate(base, {"parameters": {}, "template": "SELECT {{#open"})
    assert result["valid"] is False, result


def test_validating_writes_nothing(server):
    base, tmp = server
    _validate(base, {"parameters": {}, "template": "SELECT 'candidate' AS c"})
    with open(os.path.join(tmp, "sqls", "v.sql")) as f:
        assert f.read() == "SELECT 1 AS stored\n"
