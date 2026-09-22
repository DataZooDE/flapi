"""A configuration error must exit cleanly, not abort (#126).

`initializeConfig` was not inside a try/catch, so any exception it threw -
an invalid flapi.yaml, a missing template directory, a malformed endpoint -
reached std::terminate. flAPI binds that to a handler which logs the message
and then calls std::abort().

The diagnostic was printed, which was fine. The abort was not: a plain typo
raised SIGABRT, dumped core, and exited 134. In a container that is reported
as a crash, indistinguishable from a real fault, and on a host with core dumps
enabled every restart attempt wrote one - of a ~77 MB binary.
"""
import os
import subprocess
import tempfile

import pytest

from otel_helpers import flapi_binary

pytestmark = pytest.mark.standalone_server

# 128 + SIGABRT(6). A shell reports an abort this way, and it is what the
# old binary returned.
ABORT_EXIT_CODE = 134


def run_with_config(contents: str):
    tmp = tempfile.mkdtemp(prefix="flapi_badcfg_")
    cfg = os.path.join(tmp, "flapi.yaml")
    with open(cfg, "w") as f:
        f.write(contents)
    return subprocess.run(
        [flapi_binary(), "-c", cfg, "-p", "0", "--log-level", "warning"],
        cwd=tmp, capture_output=True, text=True, timeout=120,
        env={**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"})


class TestConfigErrorExit:
    def test_malformed_yaml_exits_one_not_aborts(self):
        r = run_with_config("project-name: x\nthis is not: [valid\n")
        assert r.returncode == 1, (
            f"expected a clean exit, got {r.returncode} "
            f"({'abort' if r.returncode == ABORT_EXIT_CODE else 'other'})")
        assert r.returncode != ABORT_EXIT_CODE

    def test_the_error_says_what_is_wrong(self):
        # An operator must be able to act on it without a debugger.
        r = run_with_config("project-name: x\nthis is not: [valid\n")
        combined = r.stdout + r.stderr
        assert "Configuration error" in combined, combined[-800:]
        assert "cannot start until the configuration is valid" in combined

    def test_a_missing_config_file_also_exits_cleanly(self):
        tmp = tempfile.mkdtemp(prefix="flapi_nocfg_")
        r = subprocess.run(
            [flapi_binary(), "-c", os.path.join(tmp, "absent.yaml"),
             "-p", "0", "--log-level", "warning"],
            cwd=tmp, capture_output=True, text=True, timeout=120,
            env={**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"})
        assert r.returncode != ABORT_EXIT_CODE
        assert r.returncode != 0
