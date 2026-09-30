"""Environment-variable handling must not disclose or mislead (#166).

1. A config that fails to load dumps its (post-substitution) node; a password
   that came from {{env.X}} was printed verbatim.
2. GET /api/v1/_config/environment-variables treated whitelist PATTERNS as variable
   names, so a regex entry such as `^DB_.*` reported nothing at all.
"""

import os
import subprocess
import tempfile
import time

import pytest
import requests

from otel_helpers import flapi_binary, free_port

SECRET = "hunter2-probe-166"


def _cfg(tmp, extra, port=None):
    os.makedirs(os.path.join(tmp, "sqls"), exist_ok=True)
    path = os.path.join(tmp, "flapi.yaml")
    with open(path, "w") as f:
        f.write("project-name: p\nproject-description: d\n"
                + (f"http-port: {port}\n" if port else "")
                + "template:\n  path: ./sqls\n  environment-whitelist:\n"
                  "    - '^FLAPI_PROBE_.*'\n"
                  "connections:\n  c:\n    properties:\n"
                  "      database: ':memory:'\n"
                  "      password: '{{env.FLAPI_PROBE_PASSWORD}}'\n" + extra)
    return path


ENV = {**os.environ, "FLAPI_PROBE_PASSWORD": SECRET, "FLAPI_PROBE_REGION": "eu-west-1",
       "DATAZOO_DISABLE_TELEMETRY": "1"}


def test_a_failed_load_does_not_dump_a_substituted_password():
    tmp = tempfile.mkdtemp(prefix="flapi_envsec_")
    cfg = _cfg(tmp, "cors: 5\n")  # a YAML::Exception after the file parsed
    r = subprocess.run([flapi_binary(), "-c", cfg, "--validate-config"],
                       capture_output=True, text=True, cwd=tmp, env=ENV, timeout=60)
    out = r.stdout + r.stderr
    assert r.returncode != 0, out
    assert SECRET not in out, "the config dump printed a password from the environment\n" + out[-2500:]


def test_the_environment_listing_reports_variables_matching_a_pattern():
    tmp = tempfile.mkdtemp(prefix="flapi_envlist_")
    port = free_port()
    cfg = _cfg(tmp, "", port)
    proc = subprocess.Popen(
        [flapi_binary(), "-c", cfg, "-p", str(port), "--config-service",
         "--config-service-token", "tok", "--log-level", "warning"],
        stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT, cwd=tmp, env=ENV)
    try:
        base = f"http://127.0.0.1:{port}"
        for _ in range(300):
            try:
                if requests.get(f"{base}/health/live", timeout=2).status_code == 200:
                    break
            except requests.RequestException:
                pass
            time.sleep(0.2)
        r = requests.get(f"{base}/api/v1/_config/environment-variables",
                         headers={"Authorization": "Bearer tok"}, timeout=10)
        assert r.status_code == 200, r.text
        got = {v["name"]: v for v in r.json()["variables"]}
        assert "FLAPI_PROBE_REGION" in got, f"a pattern-matched variable was not listed: {got}"
        assert got["FLAPI_PROBE_REGION"]["value"] == "eu-west-1"
        assert not any(n.startswith("^") for n in got), "a pattern was listed as a name"
        assert SECRET not in r.text, "a credential-named variable's value was returned"
        assert "PATH" not in got, "a variable outside the whitelist was listed"
    finally:
        proc.terminate()
        proc.wait(timeout=30)
