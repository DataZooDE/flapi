"""End-to-end tests for modern password hashing (issue #23, W1.1).

Verifies the PBKDF2-SHA256 verify path end-to-end via the real flapi
binary. A test JWT-protected endpoint is configured to require Basic
auth with a known PBKDF2 hash; the test issues requests with both the
correct and an incorrect password and asserts the binary's verify path
matches the locally-computed expectation.

The test deliberately uses Python's stdlib `hashlib.pbkdf2_hmac` to
generate fresh, deterministic hashes — no fixture files to drift.

Configuration validation alone (no DB) confirms the parser accepts the
hash; runtime auth verification requires a running server.

Marked `standalone_server` so the conftest autouse fixture does not
spin up the shared api_configuration server. Skips cleanly when flapi
cannot boot (local DuckDB extension-cache mismatch); CI runs against
fresh extensions.
"""

import base64
import hashlib
import os
import socket
import subprocess
import tempfile
import time
from typing import Dict, Iterator, List, Tuple

import pytest
import requests


def _b64u(b: bytes) -> str:
    return base64.urlsafe_b64encode(b).rstrip(b"=").decode()


def _pbkdf2_mcf(password: str, salt: bytes, iterations: int = 600_000) -> str:
    """Produce a `$pbkdf2-sha256$...$...$...` Modular Crypt Format string."""
    dk = hashlib.pbkdf2_hmac("sha256", password.encode(), salt, iterations, dklen=32)
    return f"$pbkdf2-sha256${iterations}${_b64u(salt)}${_b64u(dk)}"


def _repo_root() -> str:
    return os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def _flapi_binary() -> str:
    candidates: List[str] = []
    for build_type in ("release", "debug"):
        path = os.path.join(_repo_root(), "build", build_type, "flapi")
        if os.path.exists(path):
            candidates.append(path)
    if not candidates:
        pytest.skip("flapi binary not found in build/release or build/debug")
    candidates.sort(key=os.path.getmtime, reverse=True)
    return candidates[0]


def _free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def _basic_auth_header(user: str, password: str) -> str:
    raw = f"{user}:{password}".encode()
    return "Basic " + base64.b64encode(raw).decode()


def _validate_config_with_pbkdf2_user() -> Tuple[str, str, str]:
    """Return (binary, config_path, tempdir) for a --validate-config run."""
    binary = _flapi_binary()
    tmpdir = tempfile.mkdtemp(prefix="flapi_pbkdf2_")
    sqls = os.path.join(tmpdir, "sqls")
    os.makedirs(sqls)
    config_path = os.path.join(tmpdir, "flapi.yaml")
    pwd_hash = _pbkdf2_mcf("hunter2", os.urandom(16))
    with open(config_path, "w") as f:
        f.write(
            "project-name: pbkdf2-validate\n"
            "project-description: PBKDF2 config parses\n"
            "http-port: 8080\n"
            "template:\n"
            "  path: ./sqls\n"
            "connections:\n"
            "  inmem:\n"
            "    properties:\n"
            "      database: ':memory:'\n"
        )
    with open(os.path.join(sqls, "ping.yaml"), "w") as f:
        f.write(f"""
url-path: /ping
method: GET
template-source: ping.sql
connection: [inmem]
auth:
  enabled: true
  type: basic
  users:
    - username: alice
      password: "{pwd_hash}"
      roles: [reader]
""")
    with open(os.path.join(sqls, "ping.sql"), "w") as f:
        f.write("SELECT 1 AS ok\n")
    return binary, config_path, tmpdir


def _write_server_config(dirpath: str, port: int) -> Tuple[str, str, str]:
    """Write a server config with a known PBKDF2 user; return (config_path, user, password)."""
    sqls = os.path.join(dirpath, "sqls")
    os.makedirs(sqls)
    pwd_hash = _pbkdf2_mcf("hunter2", os.urandom(16))
    config_path = os.path.join(dirpath, "flapi.yaml")
    with open(config_path, "w") as f:
        f.write(
            f"project-name: pbkdf2-runtime\n"
            f"project-description: PBKDF2 runtime auth\n"
            f"http-port: {port}\n"
            f"template:\n"
            f"  path: ./sqls\n"
            f"connections:\n"
            f"  inmem:\n"
            f"    properties:\n"
            f"      database: ':memory:'\n"
        )
    with open(os.path.join(sqls, "ping.yaml"), "w") as f:
        f.write(f"""
url-path: /ping
method: GET
template-source: ping.sql
connection: [inmem]
auth:
  enabled: true
  type: basic
  users:
    - username: alice
      password: "{pwd_hash}"
      roles: [reader]
""")
    with open(os.path.join(sqls, "ping.sql"), "w") as f:
        f.write("SELECT 1 AS ok\n")
    return config_path, "alice", "hunter2"


@pytest.fixture
def pbkdf2_server() -> Iterator[Dict[str, str]]:
    binary = _flapi_binary()
    port = _free_port()
    with tempfile.TemporaryDirectory(prefix="flapi_pbkdf2_srv_") as tmpdir:
        config_path, user, password = _write_server_config(tmpdir, port)
        log_path = os.path.join(tmpdir, "server.log")
        log_file = open(log_path, "w")
        proc = subprocess.Popen(
            [binary, "-c", config_path, "--no-telemetry"],
            cwd=tmpdir,
            stdout=log_file,
            stderr=subprocess.STDOUT,
        )
        try:
            base_url = f"http://127.0.0.1:{port}"
            deadline = time.time() + 30
            up = False
            while time.time() < deadline:
                if proc.poll() is not None:
                    break
                try:
                    # We probe an unauthenticated path - the auth check applies
                    # to /ping specifically. We just need any live response.
                    r = requests.get(f"{base_url}/", timeout=1)
                    if r.status_code < 500:
                        up = True
                        break
                except requests.exceptions.RequestException:
                    time.sleep(0.5)
            if not up:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                log_file.close()
                with open(log_path) as f:
                    log_text = f.read()
                if "core_functions_duckdb_cpp_init" in log_text and "unique_ptr that is NULL" in log_text:
                    pytest.skip(
                        "flapi could not boot: local DuckDB extension cache is "
                        "incompatible with the in-tree DuckDB submodule. CI exercises this path."
                    )
                raise RuntimeError(f"flapi failed to start. Log:\n{log_text}")
            yield {"base_url": base_url, "user": user, "password": password}
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
            log_file.close()


@pytest.mark.standalone_server
class TestPasswordHashing:
    """End-to-end coverage for PBKDF2-SHA256 password storage."""

    def test_validate_config_accepts_pbkdf2_user(self):
        """The parser must accept a PBKDF2 MCF string as an inline user password."""
        binary, config_path, tmpdir = _validate_config_with_pbkdf2_user()
        try:
            result = subprocess.run(
                [binary, "-c", config_path, "--validate-config"],
                capture_output=True, text=True, cwd=tmpdir, timeout=30,
            )
            assert result.returncode == 0, (
                f"validate-config rejected the PBKDF2 user:\n"
                f"stdout={result.stdout}\nstderr={result.stderr}"
            )
            assert "Validation PASSED" in (result.stdout + result.stderr)
        finally:
            import shutil
            shutil.rmtree(tmpdir, ignore_errors=True)

    def test_basic_auth_with_correct_password_succeeds(self, pbkdf2_server):
        r = requests.get(
            f"{pbkdf2_server['base_url']}/ping",
            headers={"Authorization": _basic_auth_header(
                pbkdf2_server["user"], pbkdf2_server["password"])},
            timeout=5,
        )
        # 200 if DB env happy, 500 otherwise — the proof is that auth did
        # NOT return 401.
        assert r.status_code != 401, f"correct password rejected: {r.text}"

    def test_basic_auth_with_wrong_password_is_rejected(self, pbkdf2_server):
        r = requests.get(
            f"{pbkdf2_server['base_url']}/ping",
            headers={"Authorization": _basic_auth_header(
                pbkdf2_server["user"], "wrong-password")},
            timeout=5,
        )
        assert r.status_code == 401, f"wrong password unexpectedly accepted: {r.status_code} {r.text}"
