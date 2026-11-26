"""An endpoint key nothing reads is reported, not silently ignored (#282).

A section include keeps the section's own name: `{{include:auth-prod from
common.yaml}}` added an `auth-prod:` key that nothing reads, and the endpoint
it was meant to protect stayed public. `{{include:auth-prod as auth from ...}}`
inserts the section under `auth:`.
"""

import os
import signal
import subprocess
import tempfile
import time

import pytest
import requests

from otel_helpers import flapi_binary, free_port

pytestmark = pytest.mark.standalone_server

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
TOKEN = "unknown-endpoint-keys-token"

CONFIG = (
    "project-name: p\nproject-description: d\ntemplate:\n  path: ./sqls\n"
    "connections:\n  inmem:\n    properties:\n      database: ':memory:'\n"
)

COMMON = """\
auth-prod:
  enabled: true
  type: basic
  users:
    - username: ops
      password: ops-pw
      roles: [reader]
"""


def endpoint(path, extra=""):
    return f"url-path: {path}\ntemplate-source: one.sql\nconnection: [inmem]\n{extra}"


def make_root(endpoints):
    root = tempfile.mkdtemp(prefix="flapi_unkendpointkey_")
    os.makedirs(os.path.join(root, "sqls"))
    with open(os.path.join(root, "flapi.yaml"), "w") as f:
        f.write(CONFIG)
    files = {"one.sql": "SELECT 1 AS one", "common.yaml": COMMON, **endpoints}
    for name, body in files.items():
        with open(os.path.join(root, "sqls", name), "w") as f:
            f.write(body)
    return root


def validate(endpoints):
    root = make_root(endpoints)
    r = subprocess.run([flapi_binary(), "-c", "flapi.yaml", "--validate-config"],
                       capture_output=True, text=True, cwd=root, timeout=60,
                       env={**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"})
    return r.returncode, r.stdout + r.stderr


class TestValidateConfig:
    def test_an_unknown_key_warns_and_does_not_fail(self):
        code, out = validate({"e.yaml": endpoint("/e", "my-annotation: hello\n")})
        assert code == 0, out
        assert "Unknown endpoint key `my-annotation`" in out, out[-2000:]
        # Under the endpoint, counted as a warning.
        assert "⚠ Endpoint: /e" in out, out[-2000:]
        assert "1 warning(s)" in out, out[-2000:]

    def test_a_misnamed_section_include_gets_the_as_hint(self):
        code, out = validate({"e.yaml": endpoint("/e", "{{include:auth-prod from common.yaml}}\n")})
        assert code == 0, out
        assert "Unknown endpoint key `auth-prod`" in out, out[-2000:]
        assert "did you mean `auth`?" in out, out[-2000:]
        assert "{{include:auth-prod as auth from" in out, out[-2000:]

    @pytest.mark.parametrize("key,hint", [
        ("rate_limit:\n  enabled: true\n", "`rate-limit`"),
        ("template_source: one.sql\n", "`template-source`"),
        ("urlpath: /x\n", "`url-path`"),
    ])
    def test_near_misses_say_what_was_meant(self, key, hint):
        code, out = validate({"e.yaml": endpoint("/e", key)})
        assert code == 0, out
        assert "did you mean " + hint in out, out[-2000:]

    def test_known_keys_and_shared_fragments_do_not_warn(self):
        # common.yaml is a fragment (no url-path / mcp-*): skipped, never warned about.
        code, out = validate({"e.yaml": endpoint("/e", "description: d\nwith-pagination: false\n"
                                                       "{{include:auth-prod as auth from common.yaml}}\n")})
        assert code == 0, out
        assert "Unknown endpoint key" not in out, out[-2000:]
        assert "auth-prod" not in out, out[-2000:]

    def test_the_shipped_examples_raise_no_unknown_endpoint_key_warning(self):
        examples = os.path.join(REPO, "examples")
        checked = 0
        for name in ("flapi.yaml", "flapi-bigquery-procedure.yaml"):
            r = subprocess.run([flapi_binary(), "-c", os.path.join(examples, name), "--validate-config"],
                               capture_output=True, text=True, cwd=examples, timeout=120)
            out = r.stdout + r.stderr
            assert "Unknown endpoint key" not in out, name + "\n" + out[-2000:]
            checked += 1
        assert checked == 2


class TestReview:
    """Review of #282: duplicates, mistyped endpoint files, the skipped count."""

    def test_a_duplicate_auth_is_a_security_warning(self):
        code, out = validate({"e.yaml": endpoint("/e", "auth:\n  enabled: false\n"
                                                       "{{include:auth-prod as auth from common.yaml}}\n")})
        assert code == 0, out
        assert "SECURITY: endpoint key `auth` appears more than once" in out, out[-2000:]

    @pytest.mark.parametrize("key", ["urlpath", "url_path"])
    def test_a_mistyped_url_path_is_not_skipped_silently(self, key):
        code, out = validate({"typo.yaml": f"{key}: /typo\ntemplate-source: one.sql\nconnection: [inmem]\n"})
        assert code == 0, out
        assert "typo.yaml" in out, out[-2000:]
        assert "did you mean `url-path`?" in out, out[-2000:]

    def test_files_that_fail_to_load_are_not_counted_as_skipped(self):
        code, out = validate({"broken.yaml": "url-path: /b\nmethod: FETCH\ntemplate-source: one.sql\n"
                                             "connection: [inmem]\n"})
        assert code != 0, out
        # common.yaml is the only non-endpoint file.
        assert "Skipped 1 non-endpoint YAML files" in out, out[-2000:]


@pytest.fixture(scope="module")
def server():
    root = make_root({
        "protected.yaml": endpoint("/protected", "{{include:auth-prod as auth from common.yaml}}\n"),
        "misnamed.yaml": endpoint("/misnamed", "{{include:auth-prod from common.yaml}}\n"),
    })
    port = free_port()
    log_path = os.path.join(root, "server.log")
    log = open(log_path, "w")
    proc = subprocess.Popen(
        [flapi_binary(), "-c", "flapi.yaml", "-p", str(port), "--log-level", "info",
         "--config-service", "--config-service-token", TOKEN],
        cwd=root, stdout=log, stderr=subprocess.STDOUT, preexec_fn=os.setsid,
        env={**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"},
    )
    base = f"http://127.0.0.1:{port}"
    deadline = time.time() + 60
    while time.time() < deadline:
        if proc.poll() is not None:
            pytest.fail("server exited:\n" + open(log_path).read())
        try:
            if requests.get(f"{base}/health/live", timeout=1).status_code == 200:
                break
        except requests.RequestException:
            time.sleep(0.3)
    else:
        pytest.fail("server never became live")
    try:
        yield base, log_path
    finally:
        os.killpg(proc.pid, signal.SIGTERM)
        try:
            proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            os.killpg(proc.pid, signal.SIGKILL)
        log.close()


class TestServer:
    def test_include_as_auth_protects_the_endpoint(self, server):
        base, _ = server
        assert requests.get(f"{base}/protected", timeout=10).status_code == 401
        assert requests.get(f"{base}/protected", auth=("ops", "nope"), timeout=10).status_code == 401
        r = requests.get(f"{base}/protected", auth=("ops", "ops-pw"), timeout=10)
        assert r.status_code == 200, r.text

    def test_the_misnamed_include_warns_at_startup(self, server):
        base, log_path = server
        log = open(log_path).read()
        assert "Unknown endpoint key `auth-prod`" in log, log[-3000:]
        assert "misnamed.yaml" in log, log[-3000:]
        # The hazard the warning is for: nothing reads `auth-prod`, so it is public.
        assert requests.get(f"{base}/misnamed", timeout=10).status_code == 200

    def test_the_config_service_validate_api_reports_it(self, server):
        base, _ = server
        body = endpoint("/fresh", "{{include:auth-prod from common.yaml}}\n")
        r = requests.post(f"{base}/api/v1/_config/endpoints/-fresh/validate", data=body,
                          headers={"Authorization": f"Bearer {TOKEN}", "Content-Type": "application/x-yaml"},
                          timeout=10)
        assert r.status_code == 200, r.text
        result = r.json()
        assert result["valid"] is True, result
        assert any("Unknown endpoint key `auth-prod`" in w for w in result.get("warnings", [])), result
