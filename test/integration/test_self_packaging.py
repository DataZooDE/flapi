"""End-to-end round-trip tests for self-packaging (issue #46).

Mirrors the spike's 9 behaviours
(github.com/jrosskopf/research/tree/main/2026-05-21-flapi-self-packaging)
against the real `flapi` binary built by `make debug` or `make release`.

Each test builds an isolated fixture tree under `tmp_path`, invokes
`flapi pack` / `info` / `unpack` as subprocesses, and asserts on the
artifacts produced. The CLI-level tests do not start the HTTP server.

We mark everything with `standalone_server` so the shared conftest
fixture doesn't try to spin up the API-configuration server for us --
each test is self-contained.
"""

from __future__ import annotations

import os
import pathlib
import subprocess
import sys

import pytest

# Make the helpers in conftest.py importable without the autouse fixtures
# trying to start the api_configuration server for us.
sys.path.insert(0, str(pathlib.Path(__file__).parent))
from conftest import get_flapi_binary  # noqa: E402


pytestmark = pytest.mark.standalone_server


# ---------------------------------------------------------------------------
# Fixture helpers
# ---------------------------------------------------------------------------


def _write_fixture_tree(root: pathlib.Path) -> None:
    """Tiny config tree usable in both filesystem and bundled modes.

    Two endpoints:
      - GET /hello             -> SELECT 'world' AS greeting  (no I/O)
      - GET /cities-embed      -> read_csv('embed://data/cities.csv')

    The /cities-embed endpoint deliberately uses an embed:// path so it
    fails in filesystem mode and succeeds in bundled mode -- that's how
    we prove the embed FS (#44) is wired through.
    """
    (root / "sqls").mkdir(parents=True, exist_ok=True)
    (root / "data").mkdir(parents=True, exist_ok=True)

    (root / "flapi.yaml").write_text(
        "project-name: self-packaging-roundtrip\n"
        "template:\n"
        "  path: ./sqls\n"
        "connections:\n"
        "  default-conn:\n"
        "    properties:\n"
        "      path: ./data\n"
        "    log-queries: false\n"
        "    log-parameters: false\n"
        "    allow: '*'\n"
        "duckdb:\n"
        "  access_mode: READ_WRITE\n"
        "  threads: 1\n"
        "  max_memory: 256MB\n"
    )

    (root / "sqls" / "hello.yaml").write_text(
        "url-path: /hello\n"
        "method: GET\n"
        "template-source: hello.sql\n"
        "connection:\n"
        "  - default-conn\n"
    )
    (root / "sqls" / "hello.sql").write_text("SELECT 'world' AS greeting\n")

    (root / "sqls" / "cities-embed.yaml").write_text(
        "url-path: /cities-embed\n"
        "method: GET\n"
        "template-source: cities-embed.sql\n"
        "connection:\n"
        "  - default-conn\n"
    )
    (root / "sqls" / "cities-embed.sql").write_text(
        "SELECT * FROM read_csv('embed://data/cities.csv')\n"
    )

    (root / "data" / "cities.csv").write_text(
        "city,population\nBerlin,3700000\nMunich,1500000\nStuttgart,630000\n"
    )


def _flapi() -> pathlib.Path:
    """Path to the flapi binary (release/debug, per FLAPI_BUILD_TYPE)."""
    return get_flapi_binary()


def _run(cmd, **kwargs):
    """Run a subprocess and capture stdout+stderr."""
    return subprocess.run(
        cmd,
        check=False,
        capture_output=True,
        text=True,
        timeout=120,
        **kwargs,
    )


# ---------------------------------------------------------------------------
# Behaviour #2 -- `flapi pack` produces an executable with a bundle
# ---------------------------------------------------------------------------


def test_pack_produces_bundled_executable(tmp_path: pathlib.Path):
    fixture = tmp_path / "fixture"
    _write_fixture_tree(fixture)
    out = tmp_path / "flapi-bundled"

    res = _run([str(_flapi()), "pack", "--in", str(fixture), "--out", str(out)])
    assert res.returncode == 0, f"pack failed: stdout={res.stdout} stderr={res.stderr}"
    assert out.exists(), "bundled binary not created"
    assert "Packed" in res.stdout
    # Should be at least as big as the host binary.
    assert out.stat().st_size >= _flapi().stat().st_size

    # Executable bits preserved on Unix.
    if os.name == "posix":
        assert os.access(out, os.X_OK), "bundled binary missing exec bit"


# ---------------------------------------------------------------------------
# Behaviour #4 -- `flapi info` reports EOCD offset + entry list
# ---------------------------------------------------------------------------


def test_info_reports_bundle_contents(tmp_path: pathlib.Path):
    fixture = tmp_path / "fixture"
    _write_fixture_tree(fixture)
    out = tmp_path / "flapi-bundled"

    pack_res = _run([str(_flapi()), "pack", "--in", str(fixture), "--out", str(out)])
    assert pack_res.returncode == 0

    info_res = _run([str(out), "info"])
    assert info_res.returncode == 0, f"info failed: {info_res.stderr}"
    text = info_res.stdout
    assert "Bundle offset:" in text
    assert "Bundle size:" in text
    # Entry list should mention the fixture files.
    assert "flapi.yaml" in text
    assert "sqls/hello.sql" in text
    assert "sqls/cities-embed.sql" in text
    assert "data/cities.csv" in text


# ---------------------------------------------------------------------------
# Behaviour #5 -- self-hosting: bundled binary can produce a new bundle
# ---------------------------------------------------------------------------


def test_self_hosting_rebundle(tmp_path: pathlib.Path):
    fixture = tmp_path / "fixture"
    _write_fixture_tree(fixture)
    first = tmp_path / "first-bundled"
    second = tmp_path / "second-bundled"

    r1 = _run([str(_flapi()), "pack", "--in", str(fixture), "--out", str(first)])
    assert r1.returncode == 0

    # Now use the produced binary as the packager.
    r2 = _run([str(first), "pack", "--in", str(fixture), "--out", str(second)])
    assert r2.returncode == 0, f"self-host pack failed: {r2.stderr}"
    assert second.exists()

    # The self-hosted output should also have a valid bundle.
    r_info = _run([str(second), "info"])
    assert r_info.returncode == 0


# ---------------------------------------------------------------------------
# Behaviour #6 -- re-pack idempotence: size stable across N packs
# ---------------------------------------------------------------------------


def test_repack_idempotence(tmp_path: pathlib.Path):
    fixture = tmp_path / "fixture"
    _write_fixture_tree(fixture)
    first = tmp_path / "first"
    second = tmp_path / "second"
    third = tmp_path / "third"

    assert _run([str(_flapi()), "pack", "--in", str(fixture), "--out", str(first)]).returncode == 0
    size1 = first.stat().st_size

    # Re-pack using the bundled output as the host.
    assert _run([str(first), "pack", "--in", str(fixture), "--out", str(second)]).returncode == 0
    size2 = second.stat().st_size

    # And a third round.
    assert _run([str(second), "pack", "--in", str(fixture), "--out", str(third)]).returncode == 0
    size3 = third.stat().st_size

    assert size1 == size2 == size3, (
        f"binary grew across re-packs: {size1} -> {size2} -> {size3}"
    )


# ---------------------------------------------------------------------------
# Behaviour #7 -- truncating the trailing 1 KiB falls back to filesystem mode
# ---------------------------------------------------------------------------


def test_truncated_bundle_falls_back_to_filesystem(tmp_path: pathlib.Path):
    fixture = tmp_path / "fixture"
    _write_fixture_tree(fixture)
    bundled = tmp_path / "flapi-bundled"

    assert _run([str(_flapi()), "pack", "--in", str(fixture), "--out", str(bundled)]).returncode == 0

    # Sanity: bundle is present before truncation.
    assert _run([str(bundled), "info"]).returncode == 0

    # Chop 1 KiB off the end.
    with bundled.open("r+b") as f:
        f.seek(0, os.SEEK_END)
        size = f.tell()
        assert size > 1024
        f.truncate(size - 1024)

    # `info` should now report no bundle (exit 1, "none" in output).
    info_res = _run([str(bundled), "info"])
    assert info_res.returncode == 1, f"truncated info exit={info_res.returncode}"
    assert "none" in info_res.stdout, info_res.stdout

    # Running it in --validate-config mode against an external fixture
    # should still work (filesystem fallback, no crash).
    val_res = _run(
        [str(bundled), "--validate-config", "-c", str(fixture / "flapi.yaml")],
    )
    # Exit code 0 = clean validation, 1 = warnings-but-no-crash. Anything
    # below 128 = no signal kill (139=SIGSEGV, 134=SIGABRT, etc).
    assert val_res.returncode < 128, (
        f"truncated binary crashed (exit {val_res.returncode}):\n"
        f"stdout={val_res.stdout}\nstderr={val_res.stderr}"
    )


# ---------------------------------------------------------------------------
# Behaviour #8 -- `flapi unpack` dumps the archive to a directory
# ---------------------------------------------------------------------------


def test_unpack_restores_all_entries(tmp_path: pathlib.Path):
    fixture = tmp_path / "fixture"
    _write_fixture_tree(fixture)
    bundled = tmp_path / "flapi-bundled"
    dst = tmp_path / "unpacked"

    assert _run([str(_flapi()), "pack", "--in", str(fixture), "--out", str(bundled)]).returncode == 0

    unpack_res = _run([str(bundled), "unpack", "--to", str(dst)])
    assert unpack_res.returncode == 0, f"unpack failed: {unpack_res.stderr}"
    assert "Unpacked" in unpack_res.stdout

    # Every fixture file should be back.
    assert (dst / "flapi.yaml").exists()
    assert (dst / "sqls" / "hello.yaml").exists()
    assert (dst / "sqls" / "hello.sql").exists()
    assert (dst / "sqls" / "cities-embed.yaml").exists()
    assert (dst / "sqls" / "cities-embed.sql").exists()
    assert (dst / "data" / "cities.csv").exists()

    # Bytes should match.
    src_csv = (fixture / "data" / "cities.csv").read_bytes()
    dst_csv = (dst / "data" / "cities.csv").read_bytes()
    assert src_csv == dst_csv


# ---------------------------------------------------------------------------
# Pack rejects secret-looking files unless --allow-secrets
# ---------------------------------------------------------------------------


def test_pack_refuses_secret_files_by_default(tmp_path: pathlib.Path):
    fixture = tmp_path / "fixture"
    _write_fixture_tree(fixture)
    (fixture / ".env").write_text("DB_PASSWORD=hunter2\n")
    out = tmp_path / "flapi-bundled"

    res = _run([str(_flapi()), "pack", "--in", str(fixture), "--out", str(out)])
    assert res.returncode != 0
    assert "secret" in res.stderr.lower() or "secret" in res.stdout.lower()
    assert not out.exists() or out.stat().st_size == 0


def test_pack_with_allow_secrets_bundles_env_files(tmp_path: pathlib.Path):
    fixture = tmp_path / "fixture"
    _write_fixture_tree(fixture)
    (fixture / ".env").write_text("DB_PASSWORD=hunter2\n")
    out = tmp_path / "flapi-bundled"

    res = _run(
        [str(_flapi()), "pack", "--in", str(fixture), "--out", str(out), "--allow-secrets"]
    )
    assert res.returncode == 0, f"--allow-secrets pack failed: {res.stderr}"
    assert out.exists()

    info = _run([str(out), "info"])
    assert ".env" in info.stdout
