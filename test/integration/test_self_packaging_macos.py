"""macOS notarised-pack tests (issue #48).

These tests prove that the reserved __FLAPI/__bundle Mach-O segment
gets a valid signature after `flapi pack`. They're skipped on
non-Darwin runners since `codesign` doesn't exist there.
"""

from __future__ import annotations

import os
import pathlib
import platform
import shutil
import subprocess
import sys

import pytest


sys.path.insert(0, str(pathlib.Path(__file__).parent))
from conftest import get_flapi_binary  # noqa: E402


pytestmark = [
    pytest.mark.standalone_server,
    pytest.mark.skipif(
        platform.system() != "Darwin",
        reason="macOS notarisation pack tests only run on Darwin",
    ),
]


def _flapi() -> pathlib.Path:
    return get_flapi_binary()


def _has_codesign() -> bool:
    return shutil.which("codesign") is not None


def _has_otool() -> bool:
    return shutil.which("otool") is not None


def _run(cmd, **kwargs):
    return subprocess.run(
        cmd,
        check=False,
        capture_output=True,
        text=True,
        timeout=180,
        **kwargs,
    )


def _write_fixture(root: pathlib.Path) -> None:
    (root / "sqls").mkdir(parents=True, exist_ok=True)
    (root / "flapi.yaml").write_text(
        "project-name: macos-pack-test\n"
        "project-description: macOS reserved-segment fixture\n"
        "template:\n  path: ./sqls\n"
        "connections: {}\n"
        "duckdb:\n  access_mode: READ_WRITE\n  threads: 1\n"
    )
    (root / "sqls" / "hello.yaml").write_text(
        "url-path: /hello\nmethod: GET\n"
        "template-source: hello.sql\nconnection: []\n"
    )
    (root / "sqls" / "hello.sql").write_text("SELECT 'world' AS greeting\n")


def test_unbundled_flapi_has_reserved_FLAPI_bundle_segment():
    """The link-time placeholder section must exist before any pack."""
    if not _has_otool():
        pytest.skip("otool not installed")
    res = _run(["otool", "-l", str(_flapi())])
    assert res.returncode == 0, res.stderr
    out = res.stdout
    assert "segname __FLAPI" in out, "__FLAPI segment missing from unbundled flapi"
    assert "sectname __bundle" in out, "__bundle section missing from unbundled flapi"


def test_pack_reserved_segment_passes_codesign_verify(tmp_path: pathlib.Path):
    """Default pack on macOS overwrites the segment and re-signs."""
    if not _has_codesign():
        pytest.skip("codesign not installed")

    fixture = tmp_path / "fixture"
    _write_fixture(fixture)
    out = tmp_path / "flapi-bundled"

    res = _run([str(_flapi()), "pack", "--in", str(fixture), "--out", str(out)])
    assert res.returncode == 0, f"pack failed: {res.stderr}"
    assert out.exists()

    verify = _run(["codesign", "--verify", "--strict", str(out)])
    assert verify.returncode == 0, (
        f"codesign --verify failed on reserved-segment output:\n"
        f"stdout={verify.stdout}\nstderr={verify.stderr}"
    )


def test_macos_append_legacy_path_still_packs(tmp_path: pathlib.Path):
    """`--macos-append` writes the legacy trailing-bytes layout. The
    result is NOT notarisable, but pack must still produce a runnable
    artifact with a discoverable bundle."""
    fixture = tmp_path / "fixture"
    _write_fixture(fixture)
    out = tmp_path / "flapi-bundled-append"

    res = _run([
        str(_flapi()), "pack",
        "--in", str(fixture), "--out", str(out),
        "--macos-append",
    ])
    assert res.returncode == 0, f"pack --macos-append failed: {res.stderr}"

    info = _run([str(out), "info"])
    assert info.returncode == 0
    assert "flapi.yaml" in info.stdout


def test_oversized_payload_is_rejected_clearly(tmp_path: pathlib.Path):
    """If the archive doesn't fit in the reserved segment, pack must
    refuse with a corrective message rather than corrupt the binary."""
    fixture = tmp_path / "fixture"
    _write_fixture(fixture)

    # Default reserved size is 16 MiB. Stuff in a ~32 MiB blob so the
    # archive can't possibly fit.
    big = fixture / "data" / "huge.bin"
    big.parent.mkdir(parents=True, exist_ok=True)
    with big.open("wb") as f:
        f.write(os.urandom(32 * 1024 * 1024))

    out = tmp_path / "flapi-bundled-too-big"
    res = _run([str(_flapi()), "pack", "--in", str(fixture), "--out", str(out)])
    assert res.returncode != 0
    combined = (res.stdout + res.stderr).lower()
    assert "reserved" in combined or "exceeds" in combined, combined
    assert "flapi_reserved_bundle_mib" in combined.replace("-", "_"), (
        "error message should mention the rebuild knob"
    )
