"""Dead mcp.* keys must say they are dead (#151).

MCP is served by the unified server on the HTTP port. `mcp.port` was
documented as "the MCP server port", parsed, logged at debug level and never
read again - so an operator who set it believed MCP was on that port. A
launch tutorial did exactly that, and every MCP command in it got
"connection refused".

Two neighbours in the same reference table had no reader either:
`mcp.host` (MCP binds wherever the HTTP server binds) and
`mcp.allow-list-changed-notifications` (flAPI has no server-to-client
notification transport; listChanged is hard-coded false).

Asserted through --validate-config, the path operators run in CI, so the
warning reaches them before a deploy rather than after one.
"""

import os
import subprocess
import tempfile

import pytest

from otel_helpers import flapi_binary


def _validate(extra_mcp):
    tmp = tempfile.mkdtemp(prefix="flapi_mcpport_")
    os.makedirs(os.path.join(tmp, "sqls"))
    cfg = os.path.join(tmp, "flapi.yaml")
    with open(cfg, "w") as f:
        f.write("project-name: p\nproject-description: mcp.port check\n"
                "template:\n  path: ./sqls\n"
                "connections:\n  inmem:\n    properties:\n      database: ':memory:'\n"
                "mcp:\n  enabled: true\n" + extra_mcp)
    r = subprocess.run([flapi_binary(), "-c", cfg, "--validate-config"],
                       capture_output=True, text=True, cwd=tmp, timeout=60)
    return r.returncode, r.stdout + r.stderr


class TestMcpPortIsIgnored:

    def test_setting_mcp_port_warns_that_it_is_ignored(self):
        code, out = _validate("  port: 8081\n")
        assert code == 0, out          # a warning, not a config error
        assert "mcp.port" in out and "ignored" in out, (
            "setting mcp.port produced no warning; an operator would believe "
            "MCP is on 8081\n" + out[-2000:])
        # It must say where MCP actually is, not just that the key is dead.
        assert "HTTP port" in out, out[-2000:]

    def test_no_warning_when_mcp_port_is_not_set(self):
        code, out = _validate("")
        assert code == 0, out
        assert "mcp.port" not in out, out[-2000:]


@pytest.mark.parametrize("key,yaml_line,points_to", [
    ("mcp.host", "  host: 127.0.0.1\n", "HTTP"),
    ("mcp.allow-list-changed-notifications",
     "  allow-list-changed-notifications: true\n", "notification"),
])
def test_other_dead_mcp_keys_warn_too(key, yaml_line, points_to):
    code, out = _validate(yaml_line)
    assert code == 0, out
    assert key in out and "ignored" in out, (
        f"setting {key} produced no warning\n" + out[-2000:])
    assert points_to in out, out[-2000:]
