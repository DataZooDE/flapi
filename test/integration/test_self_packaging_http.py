"""End-to-end HTTP tests for bundled-mode self-packaging (issue #62).

Covers the three behaviours that were explicitly deferred from PR #56:

  Behaviour #1 -- unbundled flapi against a fixture config dir serves /hello
                  with the expected JSON body.
  Behaviour #3 -- the same fixture packed via `flapi pack` and started
                  from a clean cwd (the source tree no longer reachable)
                  serves /hello with the same body.
  Behaviour #9 -- a SQL template that calls read_csv('embed://...') resolves
                  through the EmbeddedFileSystem DuckDB VFS and returns the
                  expected rows over the REST surface.

Behaviour #2/#4/#5/#6/#7/#8 are covered at the CLI level by
test_self_packaging.py; this file is HTTP-only.
"""

from __future__ import annotations

import os
import pathlib
import socket
import subprocess
import sys
import time
from typing import List

import pytest
import urllib.request

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from conftest import get_flapi_binary  # noqa: E402


pytestmark = pytest.mark.standalone_server


def _flapi() -> pathlib.Path:
    return get_flapi_binary()


def _pick_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def _wait_for_bind(host: str, port: int, timeout_s: float = 30.0) -> bool:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.settimeout(0.5)
            try:
                s.connect((host, port))
                return True
            except OSError:
                time.sleep(0.2)
    return False


def _write_minimal_fixture(root: pathlib.Path) -> None:
    """A small but functional config tree exercising both #1/#3 (a plain
    /hello endpoint) and #9 (read_csv from embed://data/cities.csv).

    The endpoint uses inline VALUES for /hello so the success path doesn't
    depend on read_csv at all; a separate /cities endpoint reads from
    embed:// to specifically verify behaviour #9.
    """
    (root / "sqls").mkdir(parents=True, exist_ok=True)
    (root / "data").mkdir(parents=True, exist_ok=True)

    (root / "flapi.yaml").write_text(
        "project-name: bundled-http-test\n"
        "project-description: integration test fixture for issue #62\n"
        "template:\n"
        "  path: ./sqls\n"
        "connections: {}\n"
        "duckdb:\n"
        "  access_mode: READ_WRITE\n"
        "  threads: 1\n"
        "  max_memory: 256MB\n"
    )

    (root / "sqls" / "hello.yaml").write_text(
        "url-path: /hello\n"
        "method: GET\n"
        "template-source: hello.sql\n"
    )
    # No trailing semicolon -- flapi wraps the user query in
    # `SELECT * FROM (...)` for pagination, so the inner statement must
    # be a bare SELECT expression.
    (root / "sqls" / "hello.sql").write_text(
        "SELECT 'world' AS greeting\n"
    )

    (root / "sqls" / "cities.yaml").write_text(
        "url-path: /cities\n"
        "method: GET\n"
        "template-source: cities.sql\n"
    )
    (root / "sqls" / "cities.sql").write_text(
        "SELECT * FROM read_csv('embed://data/cities.csv', header=true) "
        "ORDER BY name\n"
    )

    (root / "data" / "cities.csv").write_text(
        "name,country\n"
        "Berlin,DE\n"
        "Karlsruhe,DE\n"
        "Vienna,AT\n"
    )


def _start_server(
    args: List[str], cwd: pathlib.Path, port: int
) -> subprocess.Popen:
    env = {k: v for k, v in os.environ.items() if not k.startswith("FLAPI_")}
    # NOTE: FLAPI_PORT lands in a separate PR (#63); for now we pass --port
    # on the CLI so this suite works against main and against PR #63 alike.
    cmd = list(args) + ["--port", str(port)]
    return subprocess.Popen(
        cmd,
        cwd=str(cwd),
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )


def _stop(proc: subprocess.Popen) -> None:
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=5)


def _http_json(port: int, path: str):
    req = urllib.request.Request(
        f"http://127.0.0.1:{port}{path}",
        headers={"Accept": "application/json"},
    )
    with urllib.request.urlopen(req, timeout=10) as resp:
        assert resp.status == 200, f"unexpected status {resp.status}"
        import json
        return json.loads(resp.read())


def _read_log(proc: subprocess.Popen, max_chars: int = 2000) -> str:
    if proc.stdout is None:
        return ""
    out = ""
    try:
        out = proc.stdout.read() or ""
    except Exception:
        pass
    return out[:max_chars]


# --- Behaviour #1 ------------------------------------------------------------

def test_unbundled_server_serves_hello(tmp_path: pathlib.Path):
    fixture = tmp_path / "fixture"
    _write_minimal_fixture(fixture)
    port = _pick_free_port()

    proc = _start_server(
        [str(_flapi()), "-c", "flapi.yaml"],
        cwd=fixture,
        port=port,
    )
    try:
        assert _wait_for_bind("127.0.0.1", port), (
            f"unbundled server failed to bind on {port}; log:\n{_read_log(proc)}"
        )
        body = _http_json(port, "/hello")
    finally:
        _stop(proc)

    rows = body.get("data", []) if isinstance(body, dict) else body
    assert isinstance(rows, list) and len(rows) == 1, body
    assert rows[0].get("greeting") == "world", body


# --- Behaviour #3 ------------------------------------------------------------

def test_bundled_server_serves_hello_from_clean_cwd(tmp_path: pathlib.Path):
    fixture = tmp_path / "fixture"
    _write_minimal_fixture(fixture)

    bundled = tmp_path / "flapi-bundled"
    pack_res = subprocess.run(
        [str(_flapi()), "pack", "--in", str(fixture), "--out", str(bundled)],
        check=False,
        capture_output=True,
        text=True,
        timeout=60,
    )
    assert pack_res.returncode == 0, (
        f"flapi pack failed: stdout={pack_res.stdout} stderr={pack_res.stderr}"
    )
    assert bundled.exists(), f"pack did not produce {bundled}"

    # Delete the source tree to prove the bundled binary doesn't reach
    # back into it for any file it serves.
    import shutil
    shutil.rmtree(fixture)

    clean_cwd = tmp_path / "clean_cwd"
    clean_cwd.mkdir()
    port = _pick_free_port()

    proc = _start_server([str(bundled)], cwd=clean_cwd, port=port)
    try:
        assert _wait_for_bind("127.0.0.1", port), (
            f"bundled server failed to bind on {port}; log:\n{_read_log(proc)}"
        )
        body = _http_json(port, "/hello")
    finally:
        _stop(proc)

    rows = body.get("data", []) if isinstance(body, dict) else body
    assert isinstance(rows, list) and len(rows) == 1, body
    assert rows[0].get("greeting") == "world", body


# --- Behaviour #9 ------------------------------------------------------------

def test_bundled_server_read_csv_via_embed_scheme(tmp_path: pathlib.Path):
    fixture = tmp_path / "fixture"
    _write_minimal_fixture(fixture)

    bundled = tmp_path / "flapi-bundled"
    pack_res = subprocess.run(
        [str(_flapi()), "pack", "--in", str(fixture), "--out", str(bundled)],
        check=False,
        capture_output=True,
        text=True,
        timeout=60,
    )
    assert pack_res.returncode == 0, (
        f"flapi pack failed: stdout={pack_res.stdout} stderr={pack_res.stderr}"
    )

    import shutil
    shutil.rmtree(fixture)

    clean_cwd = tmp_path / "clean_cwd"
    clean_cwd.mkdir()
    port = _pick_free_port()

    proc = _start_server([str(bundled)], cwd=clean_cwd, port=port)
    try:
        assert _wait_for_bind("127.0.0.1", port), (
            f"bundled server failed to bind; log:\n{_read_log(proc)}"
        )
        body = _http_json(port, "/cities")
    finally:
        _stop(proc)

    rows = body.get("data", []) if isinstance(body, dict) else body
    assert isinstance(rows, list) and len(rows) == 3, body
    names = [row.get("name") for row in rows]
    assert names == ["Berlin", "Karlsruhe", "Vienna"], body
    assert rows[0].get("country") == "DE"
    assert rows[2].get("country") == "AT"
