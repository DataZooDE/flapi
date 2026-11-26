"""`flapi pack` refuses an invalid config and writes nothing (#162)."""

import contextlib
import os
import shutil
import subprocess
import tempfile

import pytest

from otel_helpers import flapi_binary

GOOD = ("project-name: p\nproject-description: d\ntemplate:\n  path: ./sqls\n"
        "connections:\n  inmem:\n    properties:\n      database: ':memory:'\n")


@pytest.fixture(scope="module", autouse=True)
def no_pack_dirs_left_behind():
    """Every scratch dir this module makes must be gone when it ends (#263).

    A pack output is a copy of the ~120 MB flapi binary; leaking one per test
    filled /tmp by gigabytes over a few runs.
    """
    made = []
    real_mkdtemp = tempfile.mkdtemp

    def recording_mkdtemp(*args, **kwargs):
        path = real_mkdtemp(*args, **kwargs)
        if os.path.basename(path).startswith("flapi_packval_"):
            made.append(path)
        return path

    with pytest.MonkeyPatch.context() as mp:
        mp.setattr(tempfile, "mkdtemp", recording_mkdtemp)
        yield
    left = [p for p in made if os.path.exists(p)]
    assert made, "the module made no scratch dir; the check is not watching anything"
    assert not left, f"pack scratch dirs left behind: {left}"


@contextlib.contextmanager
def _pack(config, *extra):
    """Pack `config`; yield (returncode, output, out_path), then remove everything."""
    tmp = tempfile.mkdtemp(prefix="flapi_packval_")
    try:
        src = os.path.join(tmp, "in")
        os.makedirs(os.path.join(src, "sqls"))
        if config is not None:
            with open(os.path.join(src, "flapi.yaml"), "w") as f:
                f.write(config)
        out = os.path.join(tmp, "out.bin")
        r = subprocess.run([flapi_binary(), "pack", "--in", src, "--out", out, *extra],
                           capture_output=True, text=True, timeout=120)
        yield r.returncode, r.stdout + r.stderr, out
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def test_valid_config_packs():
    with _pack(GOOD) as (code, out, path):
        assert code == 0, out
        assert os.path.exists(path)


def test_invalid_config_is_refused_and_writes_no_file():
    with _pack(GOOD + "server:\n  port: 9000\n") as (code, out, path):
        assert code != 0, out
        assert "server:" in out and "refusing" in out, out
        assert not os.path.exists(path), "a bundle was written for an invalid config"


def test_skip_validation_packs_anyway():
    with _pack(GOOD + "server:\n  port: 9000\n", "--skip-validation") as (code, out, path):
        assert code == 0, out
        assert os.path.exists(path)
