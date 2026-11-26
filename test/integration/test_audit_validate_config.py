"""`--validate-config` must not create the audit file (#279).

The audit logger was built, and its file opened, while the configuration was
parsed, so a validation run (a CI check, `flapi pack`) created an empty
audit.jsonl, and a missing audit directory failed validation as a bare
"Configuration error" instead of a validation error naming the key.

Serving still opens the file at startup, so a server whose audit file cannot be
opened does not start.
"""
import os
import subprocess
import tempfile
import time

import pytest
import requests

from otel_helpers import flapi_binary, free_port

pytestmark = pytest.mark.standalone_server


def _project(audit_block):
    tmp = tempfile.mkdtemp(prefix="flapi_auditval_")
    os.makedirs(os.path.join(tmp, "sqls"))
    with open(os.path.join(tmp, "sqls", "ep.yaml"), "w") as f:
        f.write("url-path: /ep\nmethod: GET\ntemplate-source: ep.sql\nconnection: [inmem]\n")
    with open(os.path.join(tmp, "sqls", "ep.sql"), "w") as f:
        f.write("SELECT 1 AS n\n")
    cfg = os.path.join(tmp, "flapi.yaml")
    with open(cfg, "w") as f:
        f.write("project-name: p\nproject-description: audit validation\n"
                "template:\n  path: ./sqls\n"
                "connections:\n  inmem:\n    properties:\n      database: ':memory:'\n"
                + audit_block)
    return tmp, cfg


def _validate(tmp, cfg):
    r = subprocess.run([flapi_binary(), "-c", cfg, "--validate-config"],
                       capture_output=True, text=True, cwd=tmp, timeout=60,
                       env={**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"})
    return r.returncode, r.stdout + r.stderr


def _audit(path):
    return f"audit:\n  enabled: true\n  sink: file\n  path: {path}\n"


class TestValidateConfigLeavesNoAuditFile:
    def test_a_valid_audit_config_validates_and_creates_no_file(self):
        tmp, cfg = _project(_audit("./audit.jsonl"))
        code, out = _validate(tmp, cfg)
        assert code == 0, out[-2000:]
        assert not os.path.exists(os.path.join(tmp, "audit.jsonl")), (
            "--validate-config created the audit file\n" + out[-2000:])

    def test_an_absolute_path_creates_no_file_either(self):
        tmp, cfg = _project("")
        path = os.path.join(tmp, "logs", "audit.jsonl")
        os.makedirs(os.path.dirname(path))
        with open(cfg, "a") as f:
            f.write(_audit(path))
        code, out = _validate(tmp, cfg)
        assert code == 0, out[-2000:]
        assert not os.path.exists(path), out[-2000:]

    def test_a_missing_directory_is_a_validation_error(self):
        tmp, cfg = _project(_audit("./no-such-dir/audit.jsonl"))
        code, out = _validate(tmp, cfg)
        assert code == 1, out[-2000:]
        assert "audit.path" in out and "no-such-dir" in out, out[-2000:]
        # Reported by the validator, not as a configuration that failed to load.
        assert "Configuration error" not in out, out[-2000:]
        assert not os.path.exists(os.path.join(tmp, "no-such-dir")), out[-2000:]

    def test_a_file_sink_without_a_path_is_a_validation_error(self):
        tmp, cfg = _project("audit:\n  enabled: true\n  sink: file\n")
        code, out = _validate(tmp, cfg)
        assert code == 1, out[-2000:]
        assert "audit.path" in out, out[-2000:]


class TestServingStillOpensTheFile:
    def _serve(self, tmp, cfg):
        port = free_port()
        log = open(os.path.join(tmp, "server.log"), "w")
        proc = subprocess.Popen([flapi_binary(), "-c", cfg, "-p", str(port), "--log-level", "info"],
                                cwd=tmp, stdout=log, stderr=subprocess.STDOUT,
                                env={**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"})
        return proc, port

    def test_the_server_creates_the_file_at_startup(self):
        tmp, cfg = _project(_audit("./audit.jsonl"))
        proc, port = self._serve(tmp, cfg)
        try:
            deadline = time.time() + 60
            while time.time() < deadline:
                assert proc.poll() is None, open(os.path.join(tmp, "server.log")).read()[-3000:]
                try:
                    if requests.get(f"http://127.0.0.1:{port}/health/live", timeout=1).status_code == 200:
                        break
                except requests.RequestException:
                    time.sleep(0.2)
            # Opened at startup, before any request.
            assert os.path.exists(os.path.join(tmp, "audit.jsonl"))
            r = requests.get(f"http://127.0.0.1:{port}/ep", timeout=10)
            assert r.status_code == 200
            time.sleep(0.2)
            with open(os.path.join(tmp, "audit.jsonl")) as f:
                assert any('"target":"/ep"' in line for line in f)
        finally:
            proc.terminate()
            proc.wait(timeout=30)

    def test_a_server_whose_audit_file_cannot_open_does_not_start(self):
        tmp, cfg = _project(_audit("./no-such-dir/audit.jsonl"))
        proc, _ = self._serve(tmp, cfg)
        try:
            code = proc.wait(timeout=60)
        except subprocess.TimeoutExpired:
            proc.kill()
            pytest.fail("the server started without its audit file")
        out = open(os.path.join(tmp, "server.log")).read()
        assert code == 1, f"exit {code}\n" + out[-3000:]
        assert "audit" in out and "no-such-dir" in out, out[-3000:]
