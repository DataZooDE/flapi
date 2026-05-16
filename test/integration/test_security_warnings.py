"""End-to-end tests for startup security warnings (issue #22, W0.2).

These tests invoke the real flapi binary with crafted configurations and assert
that the security auditor surfaces the expected warning codes on stderr. The
binary exits cleanly via --validate-config; no server is started.

Warning codes asserted here are part of the audit's public contract.
"""

import os
import subprocess
import tempfile

import pytest
import yaml


# bcrypt cost-12 hash for the literal string "hunter2" — used to verify the
# auditor recognises a safe password and does NOT emit a warning. Cost is
# irrelevant for classification (only the $2b$ prefix is inspected).
BCRYPT_HUNTER2 = "$2b$12$5Q3kQqEhM2Zz5fJ7iZyqHe7eTfYpKqJzv8oU/0bI3rXlW6Tk9c1tS"

# MD5("hunter2") — used to trigger the deprecated-hash warning.
MD5_HUNTER2 = "2ab96390c7dbe3439de74d0c9b0b1767"


@pytest.fixture
def flapi_binary():
    """Locate the flapi binary.

    When both builds exist, prefer the more recently modified one so local TDD
    iterations against the debug build pick up new code without a release
    rebuild. CI environments typically build only one variant.
    """
    repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    candidates = []
    for build_type in ("release", "debug"):
        path = os.path.join(repo_root, "build", build_type, "flapi")
        if os.path.exists(path):
            candidates.append(path)
    if not candidates:
        pytest.skip("flapi binary not found in build/release or build/debug")
    candidates.sort(key=os.path.getmtime, reverse=True)
    return candidates[0]


@pytest.fixture
def temp_config_dir():
    """Yield a temp directory containing a sqls subdir; cleaned up on exit."""
    with tempfile.TemporaryDirectory() as tmpdir:
        os.makedirs(os.path.join(tmpdir, "sqls"))
        yield tmpdir


def _write_main_config(config_dir: str, extra: dict | None = None) -> str:
    """Write a minimal valid flapi.yaml plus any extra keys; return its path."""
    config = {
        "project-name": "security-warning-test",
        "project-description": "Integration test for startup security warnings",
        "http-port": 8080,
        "template": {"path": "./sqls"},
        "connections": {
            "test": {"properties": {"path": "./data.parquet"}}
        },
    }
    if extra:
        config.update(extra)
    path = os.path.join(config_dir, "flapi.yaml")
    with open(path, "w") as f:
        yaml.dump(config, f)
    return path


def _write_endpoint(config_dir: str, name: str, content: str) -> None:
    """Write an endpoint YAML and a trivial SQL template to sqls/."""
    sqls = os.path.join(config_dir, "sqls")
    with open(os.path.join(sqls, f"{name}.yaml"), "w") as f:
        f.write(content)
    with open(os.path.join(sqls, f"{name}.sql"), "w") as f:
        f.write("SELECT 1 AS ok\n")


def _run_validate(flapi_binary: str, config_path: str) -> subprocess.CompletedProcess:
    """Invoke `flapi --validate-config` and return the completed process."""
    return subprocess.run(
        [flapi_binary, "-c", config_path, "--validate-config"],
        capture_output=True,
        text=True,
        cwd=os.path.dirname(config_path),
        timeout=30,
    )


class TestSecurityWarnings:
    """End-to-end coverage for SecurityAuditor wiring in main.cpp."""

    def test_clean_config_emits_no_security_warnings(self, flapi_binary, temp_config_dir):
        config_path = _write_main_config(temp_config_dir)
        result = _run_validate(flapi_binary, config_path)

        assert result.returncode == 0, f"validate-config failed: {result.stderr}"
        # The banner is only printed when warnings exist; absent banner == clean.
        assert "SECURITY WARNINGS" not in (result.stderr + result.stdout)

    def test_plaintext_password_emits_warning(self, flapi_binary, temp_config_dir):
        config_path = _write_main_config(temp_config_dir)
        _write_endpoint(temp_config_dir, "secret", """
url-path: /secret
method: GET
template-source: secret.sql
connection: [test]
auth:
  enabled: true
  type: basic
  users:
    - username: alice
      password: hunter2
      roles: [admin]
""")
        result = _run_validate(flapi_binary, config_path)

        assert result.returncode == 0
        combined = result.stderr + result.stdout
        assert "AUTH_PLAINTEXT_PASSWORD" in combined, combined
        assert "alice" in combined

    def test_md5_password_emits_deprecated_warning(self, flapi_binary, temp_config_dir):
        config_path = _write_main_config(temp_config_dir)
        _write_endpoint(temp_config_dir, "secret", f"""
url-path: /secret
method: GET
template-source: secret.sql
connection: [test]
auth:
  enabled: true
  type: basic
  users:
    - username: alice
      password: {MD5_HUNTER2}
      roles: [admin]
""")
        result = _run_validate(flapi_binary, config_path)

        assert result.returncode == 0
        combined = result.stderr + result.stdout
        assert "AUTH_MD5_PASSWORD" in combined, combined
        assert "AUTH_PLAINTEXT_PASSWORD" not in combined, combined

    def test_bcrypt_password_emits_no_warning(self, flapi_binary, temp_config_dir):
        config_path = _write_main_config(temp_config_dir)
        _write_endpoint(temp_config_dir, "secret", f"""
url-path: /secret
method: GET
template-source: secret.sql
connection: [test]
auth:
  enabled: true
  type: basic
  users:
    - username: alice
      password: {BCRYPT_HUNTER2}
      roles: [admin]
""")
        result = _run_validate(flapi_binary, config_path)

        assert result.returncode == 0
        combined = result.stderr + result.stdout
        assert "AUTH_PLAINTEXT_PASSWORD" not in combined, combined
        assert "AUTH_MD5_PASSWORD" not in combined, combined

    def test_unauthenticated_mcp_tool_emits_warning(self, flapi_binary, temp_config_dir):
        config_path = _write_main_config(temp_config_dir, extra={
            "mcp": {"enabled": True, "auth": {"enabled": False}}
        })
        _write_endpoint(temp_config_dir, "lookup", """
template-source: lookup.sql
connection: [test]
mcp-tool:
  name: customer_lookup
  description: Look up a customer by id
""")
        result = _run_validate(flapi_binary, config_path)

        assert result.returncode == 0
        combined = result.stderr + result.stdout
        assert "MCP_UNAUTHENTICATED_TOOLS" in combined, combined

    def test_authenticated_mcp_tool_emits_no_mcp_warning(self, flapi_binary, temp_config_dir):
        config_path = _write_main_config(temp_config_dir, extra={
            "mcp": {
                "enabled": True,
                "auth": {
                    "enabled": True,
                    "type": "bearer",
                    "jwt-secret": "test-secret",
                    "jwt-issuer": "test-issuer",
                },
            }
        })
        _write_endpoint(temp_config_dir, "lookup", """
template-source: lookup.sql
connection: [test]
mcp-tool:
  name: customer_lookup
  description: Look up a customer by id
""")
        result = _run_validate(flapi_binary, config_path)

        assert result.returncode == 0
        combined = result.stderr + result.stdout
        assert "MCP_UNAUTHENTICATED_TOOLS" not in combined, combined
