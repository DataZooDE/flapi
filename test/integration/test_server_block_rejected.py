"""A top-level `server:` block is rejected, and says what to use instead (#153).

flAPI has no `server:` block. The HTTP server is configured by the top-level
`http-port` and `http-host` keys (or -p/--port, --host, FLAPI_PORT, FLAPI_HOST).
`server: {port, host}` appeared in the config-system spec and in one shipped
example, and nothing ever read it, so an operator who set `server.port: 9000`
got a server on 8080 and no explanation.

It is an ERROR, not a warning (unlike the dead `mcp.*` keys of #151): a bind
port is the setting where "silently ignored" does the most damage - traffic
goes to the wrong place - and there is no value of `server:` that has ever done
anything, so nothing working can break.

Asserted through --validate-config, the path operators run in CI, so a bad
config is caught before a deploy.
"""

import glob
import os
import subprocess
import tempfile

import pytest
import yaml

from otel_helpers import flapi_binary

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def _validate(extra, cwd=None, config=None):
    """Run `flapi --validate-config`; returns (exit code, stdout+stderr)."""
    if config is None:
        tmp = tempfile.mkdtemp(prefix="flapi_serverblock_")
        os.makedirs(os.path.join(tmp, "sqls"))
        config = os.path.join(tmp, "flapi.yaml")
        with open(config, "w") as f:
            f.write("project-name: p\nproject-description: server block check\n"
                    "template:\n  path: ./sqls\n"
                    "connections:\n  inmem:\n    properties:\n      database: ':memory:'\n"
                    + extra)
        cwd = tmp
    r = subprocess.run([flapi_binary(), "-c", config, "--validate-config"],
                       capture_output=True, text=True, cwd=cwd, timeout=60)
    return r.returncode, r.stdout + r.stderr


class TestServerBlockIsRejected:

    @pytest.mark.parametrize("block", [
        "server:\n  port: 9000\n",
        "server:\n  host: 127.0.0.1\n",
        "server:\n  port: 9000\n  host: 127.0.0.1\n",
        "server:\n  ssl: true\n",        # any child, not just port/host
        "server: true\n",                # not even a map
        "server:\n",                     # present but empty
    ])
    def test_a_server_block_fails_validation(self, block):
        code, out = _validate(block)
        assert code != 0, (
            "a `server:` block was accepted; nothing reads it, so its port "
            "would have been silently ignored\n" + out[-2000:])
        assert "server:" in out, out[-2000:]

    def test_the_error_names_every_real_key(self):
        code, out = _validate("server:\n  port: 9000\n")
        assert code != 0, out
        # An error that only says "no" would leave the operator guessing.
        for real in ("http-port", "http-host", "FLAPI_PORT", "FLAPI_HOST",
                     "--port", "--host"):
            assert real in out, f"the error does not mention {real}\n" + out[-2000:]

    @pytest.mark.parametrize("config", [
        # An earlier required key missing / an earlier key malformed must not
        # hide the more useful error: the block is checked FIRST.
        "project-name: p\nserver:\n  port: 9000\n",                       # no project-description
        "project-description: d\nserver:\n  port: 9000\n",                # no project-name
        "project-name: p\nproject-description: d\nhttp-port: not-a-number\nserver:\n  port: 9000\n",
    ])
    def test_the_block_is_reported_before_other_config_errors(self, config):
        tmp = tempfile.mkdtemp(prefix="flapi_serverblock_order_")
        os.makedirs(os.path.join(tmp, "sqls"))
        cfg = os.path.join(tmp, "flapi.yaml")
        with open(cfg, "w") as f:
            f.write(config)
        code, out = _validate("", cwd=tmp, config=cfg)
        assert code != 0, out
        assert "no `server:` block" in out, (
            "another error masked the server: block message\n" + out[-2000:])

    def test_it_is_rejected_even_when_http_port_is_also_set(self):
        # Agreement between the two would still be a config with a dead key.
        code, out = _validate("http-port: 9000\nserver:\n  port: 9000\n")
        assert code != 0, out

    def test_no_error_without_a_server_block(self):
        code, out = _validate("http-port: 9000\nhttp-host: 127.0.0.1\n")
        assert code == 0, out
        # "server:" specifically - ordinary log lines say "HTTP server".
        assert "server:" not in out, out[-2000:]

    def test_the_server_name_key_is_unaffected(self):
        # `server-name` is a real, separate key and must keep working.
        code, out = _validate("server-name: my-flapi\n")
        assert code == 0, out


class TestShippedConfigsHaveNoServerBlock:
    """The guard that stops an example reintroducing it. bigquery-procedure
    still had one when this was written; the sibling s3/gcs/azure examples had
    been fixed, and that one was missed."""

    def _main_configs(self):
        found = []
        for path in glob.glob(os.path.join(REPO, "examples", "**", "*.yaml"),
                              recursive=True):
            try:
                with open(path) as f:
                    doc = yaml.safe_load(f)
            except Exception:
                continue
            # A main config declares project-name; endpoint yamls do not.
            if isinstance(doc, dict) and "project-name" in doc:
                found.append((path, doc))
        return found

    def test_the_glob_actually_finds_the_examples(self):
        # Without this, an empty glob would make the guard below vacuous.
        assert len(self._main_configs()) >= 5

    def test_no_example_has_a_top_level_server_block(self):
        offenders = [os.path.relpath(p, REPO)
                     for p, doc in self._main_configs() if "server" in doc]
        assert not offenders, (
            f"these examples set a `server:` block flAPI rejects: {offenders}")

    # flapi.yaml was excluded while --validate-config wrongly validated an MCP
    # prompt file as an endpoint (#156); flapi-test.yaml while two endpoints named
    # a connection it excludes (#167). All three validate now.
    @pytest.mark.parametrize("name", ["flapi.yaml", "flapi-bigquery-procedure.yaml", "flapi-test.yaml"])
    def test_the_offline_examples_validate(self, name):
        examples = os.path.join(REPO, "examples")
        code, out = _validate("", cwd=examples, config=os.path.join(examples, name))
        assert code == 0, f"{name} no longer validates\n" + out[-2500:]
