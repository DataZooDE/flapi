"""`flapi pack` refuses an invalid config and writes nothing (#162)."""

import os
import subprocess
import tempfile

from otel_helpers import flapi_binary

GOOD = ("project-name: p\nproject-description: d\ntemplate:\n  path: ./sqls\n"
        "connections:\n  inmem:\n    properties:\n      database: ':memory:'\n")


def _pack(config, *extra):
    tmp = tempfile.mkdtemp(prefix="flapi_packval_")
    src = os.path.join(tmp, "in")
    os.makedirs(os.path.join(src, "sqls"))
    if config is not None:
        with open(os.path.join(src, "flapi.yaml"), "w") as f:
            f.write(config)
    out = os.path.join(tmp, "out.bin")
    r = subprocess.run([flapi_binary(), "pack", "--in", src, "--out", out, *extra],
                       capture_output=True, text=True, timeout=120)
    return r.returncode, r.stdout + r.stderr, out


def test_valid_config_packs():
    code, out, path = _pack(GOOD)
    assert code == 0, out
    assert os.path.exists(path)


def test_invalid_config_is_refused_and_writes_no_file():
    code, out, path = _pack(GOOD + "server:\n  port: 9000\n")
    assert code != 0, out
    assert "server:" in out and "refusing" in out, out
    assert not os.path.exists(path), "a bundle was written for an invalid config"


def test_skip_validation_packs_anyway():
    code, out, path = _pack(GOOD + "server:\n  port: 9000\n", "--skip-validation")
    assert code == 0, out
    assert os.path.exists(path)
