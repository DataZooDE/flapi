"""
HTTPS/TLS Configuration Integration Tests

Tests HTTPS configuration parsing and validation:
- Valid certificate configuration
- Missing certificate file handling
- Missing key file handling
- Configuration error messages

Note: These tests verify configuration validation only.
Full HTTPS server startup requires a separate server instance.
"""

import pytest
import os
import tempfile
import subprocess
import yaml


class TestHttpsConfigValidation:
    """Tests for HTTPS configuration validation."""

    @pytest.fixture
    def flapi_binary(self):
        """Get path to flapi binary."""
        # Try release first, then debug
        for build_type in ["release", "debug"]:
            binary_path = f"build/{build_type}/flapi"
            if os.path.exists(binary_path):
                return binary_path
        pytest.skip("flapi binary not found")

    @pytest.fixture
    def fixtures_dir(self):
        """Get path to test fixtures directory."""
        return os.path.join(os.path.dirname(__file__), "fixtures")

    @pytest.fixture
    def temp_config_dir(self):
        """Create a temporary directory for test configs."""
        with tempfile.TemporaryDirectory() as tmpdir:
            # Create sqls directory
            sqls_dir = os.path.join(tmpdir, "sqls")
            os.makedirs(sqls_dir)
            yield tmpdir

    def write_config(self, config_dir: str, config: dict) -> str:
        """Write a config file and return its path."""
        config_path = os.path.join(config_dir, "flapi.yaml")
        with open(config_path, "w") as f:
            yaml.dump(config, f)
        return config_path

    def test_https_disabled_validates_successfully(self, flapi_binary, temp_config_dir):
        """Test that config with HTTPS disabled validates successfully."""
        config = {
            "project-name": "test-project",
            "http-port": 8080,
            "template": {"path": "./sqls"},
            "connections": {
                "test": {"properties": {"path": "./data.parquet"}}
            },
            "enforce-https": {"enabled": False}
        }

        config_path = self.write_config(temp_config_dir, config)

        result = subprocess.run(
            [flapi_binary, "-c", config_path, "--validate-config"],
            capture_output=True,
            text=True,
            cwd=temp_config_dir
        )

        assert result.returncode == 0, f"Validation failed: {result.stderr}"

    def test_https_enabled_with_valid_certs_validates(self, flapi_binary, temp_config_dir, fixtures_dir):
        """Test that config with valid cert paths validates successfully."""
        cert_path = os.path.join(fixtures_dir, "test_cert.pem")
        key_path = os.path.join(fixtures_dir, "test_key.pem")

        if not os.path.exists(cert_path) or not os.path.exists(key_path):
            pytest.skip("Test certificates not found in fixtures directory")

        config = {
            "project-name": "test-project",
            "http-port": 8080,
            "template": {"path": "./sqls"},
            "connections": {
                "test": {"properties": {"path": "./data.parquet"}}
            },
            "enforce-https": {
                "enabled": True,
                "ssl-cert-file": cert_path,
                "ssl-key-file": key_path
            }
        }

        config_path = self.write_config(temp_config_dir, config)

        result = subprocess.run(
            [flapi_binary, "-c", config_path, "--validate-config"],
            capture_output=True,
            text=True,
            cwd=temp_config_dir
        )

        assert result.returncode == 0, f"Validation failed: {result.stderr}"

    def test_https_enabled_missing_cert_fails(self, flapi_binary, temp_config_dir, fixtures_dir):
        """Test that HTTPS enabled without cert file fails validation."""
        key_path = os.path.join(fixtures_dir, "test_key.pem")

        config = {
            "project-name": "test-project",
            "http-port": 8080,
            "template": {"path": "./sqls"},
            "connections": {
                "test": {"properties": {"path": "./data.parquet"}}
            },
            "enforce-https": {
                "enabled": True,
                "ssl-key-file": key_path
            }
        }

        config_path = self.write_config(temp_config_dir, config)

        result = subprocess.run(
            [flapi_binary, "-c", config_path, "--validate-config"],
            capture_output=True,
            text=True,
            cwd=temp_config_dir
        )

        assert result.returncode != 0, "Validation should fail with missing cert"
        assert "ssl" in result.stderr.lower() or "certificate" in result.stderr.lower() or "enforce-https" in result.stderr.lower()

    def test_https_enabled_missing_key_fails(self, flapi_binary, temp_config_dir, fixtures_dir):
        """Test that HTTPS enabled without key file fails validation."""
        cert_path = os.path.join(fixtures_dir, "test_cert.pem")

        config = {
            "project-name": "test-project",
            "http-port": 8080,
            "template": {"path": "./sqls"},
            "connections": {
                "test": {"properties": {"path": "./data.parquet"}}
            },
            "enforce-https": {
                "enabled": True,
                "ssl-cert-file": cert_path
            }
        }

        config_path = self.write_config(temp_config_dir, config)

        result = subprocess.run(
            [flapi_binary, "-c", config_path, "--validate-config"],
            capture_output=True,
            text=True,
            cwd=temp_config_dir
        )

        assert result.returncode != 0, "Validation should fail with missing key"
        assert "ssl" in result.stderr.lower() or "key" in result.stderr.lower() or "enforce-https" in result.stderr.lower()

    def test_https_enabled_missing_both_fails(self, flapi_binary, temp_config_dir):
        """Test that HTTPS enabled without cert and key fails validation."""
        config = {
            "project-name": "test-project",
            "http-port": 8080,
            "template": {"path": "./sqls"},
            "connections": {
                "test": {"properties": {"path": "./data.parquet"}}
            },
            "enforce-https": {
                "enabled": True
            }
        }

        config_path = self.write_config(temp_config_dir, config)

        result = subprocess.run(
            [flapi_binary, "-c", config_path, "--validate-config"],
            capture_output=True,
            text=True,
            cwd=temp_config_dir
        )

        assert result.returncode != 0, "Validation should fail with missing cert and key"

    def test_https_as_scalar_fails(self, flapi_binary, temp_config_dir):
        """Test that enforce-https as scalar (not map) fails validation."""
        # Write config directly to avoid YAML lib interpretation
        config_path = os.path.join(temp_config_dir, "flapi.yaml")
        with open(config_path, "w") as f:
            f.write("""
project-name: test-project
http-port: 8080
template:
  path: ./sqls
connections:
  test:
    properties:
      path: ./data.parquet
enforce-https: true
""")

        result = subprocess.run(
            [flapi_binary, "-c", config_path, "--validate-config"],
            capture_output=True,
            text=True,
            cwd=temp_config_dir
        )

        assert result.returncode != 0, "Validation should fail with enforce-https as scalar"
        assert "enforce-https" in result.stderr.lower() or "map" in result.stderr.lower()

    def test_https_with_empty_cert_path_fails(self, flapi_binary, temp_config_dir, fixtures_dir):
        """Test that empty cert path fails validation."""
        key_path = os.path.join(fixtures_dir, "test_key.pem")

        config = {
            "project-name": "test-project",
            "http-port": 8080,
            "template": {"path": "./sqls"},
            "connections": {
                "test": {"properties": {"path": "./data.parquet"}}
            },
            "enforce-https": {
                "enabled": True,
                "ssl-cert-file": "",
                "ssl-key-file": key_path
            }
        }

        config_path = self.write_config(temp_config_dir, config)

        result = subprocess.run(
            [flapi_binary, "-c", config_path, "--validate-config"],
            capture_output=True,
            text=True,
            cwd=temp_config_dir
        )

        assert result.returncode != 0, "Validation should fail with empty cert path"


class TestHttpsCertificateFiles:
    """Tests for certificate file handling."""

    @pytest.fixture
    def fixtures_dir(self):
        """Get path to test fixtures directory."""
        return os.path.join(os.path.dirname(__file__), "fixtures")

    def test_cert_file_exists(self, fixtures_dir):
        """Verify test certificate file exists."""
        cert_path = os.path.join(fixtures_dir, "test_cert.pem")
        assert os.path.exists(cert_path), "Test certificate not found"

    def test_key_file_exists(self, fixtures_dir):
        """Verify test key file exists."""
        key_path = os.path.join(fixtures_dir, "test_key.pem")
        assert os.path.exists(key_path), "Test key file not found"

    def test_cert_file_is_valid_pem(self, fixtures_dir):
        """Verify certificate file has valid PEM format."""
        cert_path = os.path.join(fixtures_dir, "test_cert.pem")
        with open(cert_path, "r") as f:
            content = f.read()

        assert "-----BEGIN CERTIFICATE-----" in content
        assert "-----END CERTIFICATE-----" in content

    def test_key_file_is_valid_pem(self, fixtures_dir):
        """Verify key file has valid PEM format."""
        key_path = os.path.join(fixtures_dir, "test_key.pem")
        with open(key_path, "r") as f:
            content = f.read()

        assert "-----BEGIN PRIVATE KEY-----" in content or "-----BEGIN RSA PRIVATE KEY-----" in content
        assert "-----END PRIVATE KEY-----" in content or "-----END RSA PRIVATE KEY-----" in content
