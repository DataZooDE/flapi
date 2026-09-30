"""An unrecognised top-level key is reported, not silently ignored (#160)."""

import glob
import os
import subprocess
import tempfile

import pytest
import yaml

from otel_helpers import flapi_binary

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def _validate(extra):
    tmp = tempfile.mkdtemp(prefix="flapi_unkkey_")
    os.makedirs(os.path.join(tmp, "sqls"))
    cfg = os.path.join(tmp, "flapi.yaml")
    with open(cfg, "w") as f:
        f.write("project-name: p\nproject-description: d\ntemplate:\n  path: ./sqls\n"
                "connections:\n  inmem:\n    properties:\n      database: ':memory:'\n" + extra)
    r = subprocess.run([flapi_binary(), "-c", cfg, "--validate-config"],
                       capture_output=True, text=True, cwd=tmp, timeout=60)
    return r.returncode, r.stdout + r.stderr


def test_an_unknown_key_warns_and_does_not_fail():
    code, out = _validate("my-annotation: hello\n")
    assert code == 0, out
    assert "Unknown top-level configuration key `my-annotation`" in out, out[-1500:]


@pytest.mark.parametrize("key,hint", [
    ("rate-limit:\n  enabled: true\n", "rate_limit"),
    ("https:\n  enabled: true\n", "enforce-https"),
])
def test_near_misses_say_what_was_meant(key, hint):
    code, out = _validate(key)
    assert code == 0, out
    assert hint in out, out[-1500:]


def test_known_keys_do_not_warn():
    code, out = _validate("version: 1.0.0\nhttp-port: 9000\nlog-level: info\n")
    assert code == 0, out
    assert "Unknown top-level" not in out, out[-1500:]


def test_the_shipped_examples_raise_no_unknown_key_warning():
    examples = os.path.join(REPO, "examples")
    checked = 0
    for name in ("flapi.yaml", "flapi-bigquery-procedure.yaml"):
        r = subprocess.run([flapi_binary(), "-c", os.path.join(examples, name), "--validate-config"],
                           capture_output=True, text=True, cwd=examples, timeout=60)
        assert "Unknown top-level" not in r.stdout + r.stderr, name + "\n" + (r.stdout + r.stderr)[-1500:]
        checked += 1
    assert checked == 2
