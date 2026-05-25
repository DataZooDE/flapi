"""12-factor env-var precedence tests (issues #47, #63).

Verifies that `FLAPI_CONFIG`, `FLAPI_LOG_LEVEL`, `FLAPI_PORT` and
`FLAPI_HOST` all work as documented:
    CLI flag > env var > config file > built-in default.
Plus: invalid `FLAPI_LOG_LEVEL` and `FLAPI_PORT` values are rejected
with a clear single-line error, not silently coerced.

Most tests build a tiny fixture config and invoke `flapi --validate-config`
as a subprocess -- no HTTP server lifecycle needed. The FLAPI_PORT /
FLAPI_HOST bind tests actually start the server and observe the listening
socket, since `--validate-config` exits before binding.
"""

from __future__ import annotations

import os
import pathlib
import socket
import subprocess
import sys
import time

import pytest

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from conftest import get_flapi_binary  # noqa: E402


pytestmark = pytest.mark.standalone_server


def _flapi() -> pathlib.Path:
    return get_flapi_binary()


def _write_minimal_config(root: pathlib.Path) -> pathlib.Path:
    """A tiny config that validates cleanly (no endpoints required)."""
    (root / "sqls").mkdir(parents=True, exist_ok=True)
    config = root / "flapi.yaml"
    config.write_text(
        "project-name: env-override-test\n"
        "project-description: integration test fixture\n"
        "template:\n"
        "  path: ./sqls\n"
        "connections: {}\n"
        "duckdb:\n"
        "  access_mode: READ_WRITE\n"
        "  threads: 1\n"
        "  max_memory: 256MB\n"
    )
    return config


def _run(cmd, env_overrides=None, timeout=30):
    """Run flapi with a controlled env. Always strips FLAPI_* by default."""
    env = {k: v for k, v in os.environ.items() if not k.startswith("FLAPI_")}
    if env_overrides:
        env.update(env_overrides)
    return subprocess.run(
        cmd,
        check=False,
        capture_output=True,
        text=True,
        timeout=timeout,
        env=env,
    )


def test_invalid_FLAPI_LOG_LEVEL_is_rejected(tmp_path: pathlib.Path):
    config = _write_minimal_config(tmp_path)
    res = _run(
        [str(_flapi()), "--validate-config", "-c", str(config)],
        env_overrides={"FLAPI_LOG_LEVEL": "verbose"},
    )
    assert res.returncode == 1
    combined = (res.stderr + res.stdout).lower()
    assert "invalid log level" in combined
    assert "verbose" in combined


def test_valid_FLAPI_LOG_LEVEL_is_honoured(tmp_path: pathlib.Path):
    config = _write_minimal_config(tmp_path)
    res = _run(
        [str(_flapi()), "--validate-config", "-c", str(config)],
        env_overrides={"FLAPI_LOG_LEVEL": "debug"},
    )
    assert res.returncode == 0, (
        f"validate-config failed unexpectedly: "
        f"stdout={res.stdout} stderr={res.stderr}"
    )
    # Debug-level should emit the "ConfigLoader initialized" line that
    # info-level suppresses.
    combined = res.stdout + res.stderr
    assert "[DEBUG" in combined, "no DEBUG lines visible at FLAPI_LOG_LEVEL=debug"


def test_CLI_log_level_wins_over_env(tmp_path: pathlib.Path):
    config = _write_minimal_config(tmp_path)
    res = _run(
        [str(_flapi()), "--validate-config", "-c", str(config),
         "--log-level", "error"],
        env_overrides={"FLAPI_LOG_LEVEL": "debug"},
    )
    assert res.returncode == 0, res.stderr
    combined = res.stdout + res.stderr
    # CLI said `error`, so no DEBUG lines should appear despite the env.
    assert "[DEBUG" not in combined, (
        "CLI --log-level should have suppressed debug output but didn't"
    )


def test_FLAPI_CONFIG_used_when_no_c_flag(tmp_path: pathlib.Path):
    config = _write_minimal_config(tmp_path)
    # No `-c` flag -- the binary should pick FLAPI_CONFIG instead of the
    # default `flapi.yaml` in cwd.
    res = _run(
        [str(_flapi()), "--validate-config"],
        env_overrides={"FLAPI_CONFIG": str(config)},
    )
    assert res.returncode == 0, res.stderr
    # The config path should appear in the load message.
    assert str(config) in (res.stdout + res.stderr)


def test_CLI_c_flag_wins_over_FLAPI_CONFIG(tmp_path: pathlib.Path):
    cli_config = _write_minimal_config(tmp_path / "cli")
    env_dir = tmp_path / "env"
    env_dir.mkdir()
    env_config = env_dir / "flapi.yaml"
    # Deliberately broken env config -- if it gets loaded, validate-config
    # will fail loudly. Since `-c` should win, it must not be touched.
    env_config.write_text("this: is not valid yaml: at all:\n - - - !!!")

    res = _run(
        [str(_flapi()), "--validate-config", "-c", str(cli_config)],
        env_overrides={"FLAPI_CONFIG": str(env_config)},
    )
    assert res.returncode == 0, (
        f"-c was ignored in favour of FLAPI_CONFIG: "
        f"stdout={res.stdout} stderr={res.stderr}"
    )


def test_default_config_path_unchanged_when_no_env(tmp_path: pathlib.Path, monkeypatch):
    """If neither flag nor env is set, the default is still flapi.yaml."""
    _write_minimal_config(tmp_path)
    monkeypatch.chdir(tmp_path)  # so the default `flapi.yaml` exists in cwd

    res = _run([str(_flapi()), "--validate-config"])
    assert res.returncode == 0, (
        f"default flapi.yaml lookup failed: "
        f"stdout={res.stdout} stderr={res.stderr}"
    )


# --- FLAPI_PORT / FLAPI_HOST (issue #63) --------------------------------------

def _pick_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def _wait_for_bind(host: str, port: int, timeout_s: float = 10.0) -> bool:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.settimeout(0.5)
            try:
                s.connect((host, port))
                return True
            except OSError:
                time.sleep(0.1)
    return False


def _start_server(config: pathlib.Path, env_overrides: dict, extra_args=None):
    env = {k: v for k, v in os.environ.items() if not k.startswith("FLAPI_")}
    env.update(env_overrides)
    args = [str(_flapi()), "-c", str(config)] + (extra_args or [])
    return subprocess.Popen(
        args,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )


@pytest.mark.parametrize("bogus", ["abc", "0", "99999", "-1"])
def test_invalid_FLAPI_PORT_is_rejected(tmp_path: pathlib.Path, bogus: str):
    config = _write_minimal_config(tmp_path)
    res = _run(
        [str(_flapi()), "--validate-config", "-c", str(config)],
        env_overrides={"FLAPI_PORT": bogus},
    )
    assert res.returncode == 1
    combined = (res.stderr + res.stdout).lower()
    assert "invalid flapi_port" in combined
    assert bogus.lower() in combined


def test_FLAPI_PORT_used_when_no_port_flag(tmp_path: pathlib.Path):
    config = _write_minimal_config(tmp_path)
    port = _pick_free_port()
    proc = _start_server(config, {"FLAPI_PORT": str(port)})
    try:
        assert _wait_for_bind("127.0.0.1", port), (
            f"FLAPI_PORT={port} did not result in a listening socket; "
            f"server output: {(proc.stdout.read() if proc.stdout else '')[:500]}"
        )
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)


def test_CLI_port_wins_over_FLAPI_PORT(tmp_path: pathlib.Path):
    config = _write_minimal_config(tmp_path)
    cli_port = _pick_free_port()
    env_port = _pick_free_port()
    while env_port == cli_port:
        env_port = _pick_free_port()
    proc = _start_server(
        config,
        {"FLAPI_PORT": str(env_port)},
        extra_args=["--port", str(cli_port)],
    )
    try:
        assert _wait_for_bind("127.0.0.1", cli_port), (
            f"--port {cli_port} did not bind despite being given on the CLI"
        )
        # And the env-supplied port must NOT be listening.
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.settimeout(0.5)
            assert s.connect_ex(("127.0.0.1", env_port)) != 0, (
                f"FLAPI_PORT={env_port} bound despite CLI override; "
                f"CLI precedence broken"
            )
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)


def test_FLAPI_HOST_used_when_no_host_flag(tmp_path: pathlib.Path):
    config = _write_minimal_config(tmp_path)
    port = _pick_free_port()
    proc = _start_server(
        config,
        {"FLAPI_PORT": str(port), "FLAPI_HOST": "127.0.0.1"},
    )
    try:
        assert _wait_for_bind("127.0.0.1", port), (
            "server did not bind on 127.0.0.1 as FLAPI_HOST requested"
        )
        # External-iface check: binding 127.0.0.1 should NOT make the
        # port reachable from a non-loopback address. Skip if no
        # non-loopback iface available (e.g. CI sandboxes).
        try:
            hostname_ip = socket.gethostbyname(socket.gethostname())
        except OSError:
            hostname_ip = "127.0.0.1"
        if hostname_ip != "127.0.0.1":
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
                s.settimeout(0.5)
                assert s.connect_ex((hostname_ip, port)) != 0, (
                    f"FLAPI_HOST=127.0.0.1 leaked onto {hostname_ip}; "
                    f"bindaddr not respected"
                )
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)
