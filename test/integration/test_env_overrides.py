"""12-factor env-var precedence tests (issue #47).

Verifies that `FLAPI_CONFIG` and `FLAPI_LOG_LEVEL` work as documented:
    CLI flag > env var > built-in default.
Plus: invalid `FLAPI_LOG_LEVEL` values are rejected with a clear
single-line error, not silently coerced.

These tests build a tiny fixture config and invoke `flapi --validate-config`
as a subprocess -- no HTTP server lifecycle needed.
"""

from __future__ import annotations

import os
import pathlib
import subprocess
import sys

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
