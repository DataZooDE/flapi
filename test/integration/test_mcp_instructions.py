"""Integration tests for MCP server instructions field."""

import json
import subprocess
import time
import requests
import os
import tempfile
import shutil
from pathlib import Path
import pytest
import socket

# Get project root directory (test/integration -> project root)
PROJECT_ROOT = Path(__file__).parent.parent.parent

def find_free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("", 0))
        s.listen(1)
        return s.getsockname()[1]


@pytest.mark.standalone_server
class TestMCPInstructions:
    """Test MCP server instructions configuration and delivery."""

    def test_initialize_with_file_instructions(self):
        """Test that instructions from file appear in initialize response."""
        # Create a temporary directory for the test
        with tempfile.TemporaryDirectory() as tmpdir:
            # Create the configuration files
            config_path = Path(tmpdir) / "flapi.yaml"
            instructions_path = Path(tmpdir) / "mcp_instructions.md"
            sqls_dir = Path(tmpdir) / "sqls"
            sqls_dir.mkdir()

            # Write instructions file
            instructions_content = """# Test MCP Instructions

This is a test instruction file.

## Features
- Feature 1
- Feature 2
"""
            instructions_path.write_text(instructions_content)

            # Write minimal flapi.yaml with instructions
            port = find_free_port()
            config_content = f"""project-name: test-instructions
project-description: Test project for MCP instructions

template:
  path: ./sqls

connections:
  test-data:
    properties:
      path: ./data/test.parquet

mcp:
  enabled: true
  port: {port}
  instructions-file: ./mcp_instructions.md
"""
            config_path.write_text(config_content)

            # Create a minimal endpoint
            endpoint_path = sqls_dir / "test.yaml"
            endpoint_path.write_text("""url-path: /test
method: GET
template-source: test.sql
connection: [test-data]
mcp-tool:
  name: test_tool
  description: Test tool
""")

            # Create SQL template
            sql_path = sqls_dir / "test.sql"
            sql_path.write_text("SELECT 1")

            # Start the server
            log_path = Path(tmpdir) / "mcp_instructions.log"
            log_file = open(log_path, "w")
            server_process = subprocess.Popen(
                [
                    "./build/release/flapi",
                    "-c",
                    str(config_path),
                    "--port",
                    str(port),
                ],
                cwd=str(PROJECT_ROOT),
                stdout=log_file,
                stderr=subprocess.STDOUT,
            )

            try:
                # Wait for server to start
                time.sleep(2)

                # Test MCP initialize request
                response = requests.post(
                    f"http://localhost:{port}/mcp/jsonrpc",
                    json={
                        "jsonrpc": "2.0",
                        "id": 1,
                        "method": "initialize",
                        "params": {"protocolVersion": "2024-11-05"},
                    },
                    timeout=10,
                )

                # Verify response
                assert response.status_code == 200
                data = response.json()

                # Verify instructions are in the response
                assert "result" in data
                result = data["result"]

                # Parse the result if it's a JSON string
                if isinstance(result, str):
                    result = json.loads(result)

                assert "instructions" in result
                assert "Test MCP Instructions" in result["instructions"]
                assert "Feature 1" in result["instructions"]

                print("✓ Instructions from file successfully loaded and returned")

            finally:
                # Clean up server
                server_process.terminate()
                server_process.wait(timeout=5)
                log_file.flush()
                log_file.close()

    def test_initialize_with_inline_instructions(self):
        """Test that inline instructions appear in initialize response."""
        # Create a temporary directory for the test
        with tempfile.TemporaryDirectory() as tmpdir:
            # Create the configuration files
            config_path = Path(tmpdir) / "flapi.yaml"
            sqls_dir = Path(tmpdir) / "sqls"
            sqls_dir.mkdir()

            # Write minimal flapi.yaml with inline instructions
            port = find_free_port()
            config_content = f"""project-name: test-inline-instructions
project-description: Test project for inline MCP instructions

template:
  path: ./sqls

connections:
  test-data:
    properties:
      path: ./data/test.parquet

mcp:
  enabled: true
  port: {port}
  instructions: |
    # Inline Test Instructions

    This is an inline instruction.

    ## Section
    - Item 1
    - Item 2
"""
            config_path.write_text(config_content)

            # Create a minimal endpoint
            endpoint_path = sqls_dir / "test.yaml"
            endpoint_path.write_text("""url-path: /test
method: GET
template-source: test.sql
connection: [test-data]
mcp-tool:
  name: test_tool
  description: Test tool
""")

            # Create SQL template
            sql_path = sqls_dir / "test.sql"
            sql_path.write_text("SELECT 1")

            # Start the server
            log_path = Path(tmpdir) / "mcp_inline_instructions.log"
            log_file = open(log_path, "w")
            server_process = subprocess.Popen(
                [
                    "./build/release/flapi",
                    "-c",
                    str(config_path),
                    "--port",
                    str(port),
                ],
                cwd=str(PROJECT_ROOT),
                stdout=log_file,
                stderr=subprocess.STDOUT,
            )

            try:
                # Wait for server to start
                time.sleep(2)

                # Test MCP initialize request
                response = requests.post(
                    f"http://localhost:{port}/mcp/jsonrpc",
                    json={
                        "jsonrpc": "2.0",
                        "id": 1,
                        "method": "initialize",
                        "params": {"protocolVersion": "2024-11-05"},
                    },
                    timeout=10,
                )

                # Verify response
                assert response.status_code == 200
                data = response.json()

                # Verify instructions are in the response
                assert "result" in data
                result = data["result"]

                # Parse the result if it's a JSON string
                if isinstance(result, str):
                    result = json.loads(result)

                assert "instructions" in result
                assert "Inline Test Instructions" in result["instructions"]
                assert "Item 1" in result["instructions"]

                print("✓ Inline instructions successfully loaded and returned")

            finally:
                # Clean up server
                server_process.terminate()
                server_process.wait(timeout=5)
                log_file.flush()
                log_file.close()

    def test_initialize_without_instructions(self):
        """Test that initialize works fine without instructions configured."""
        # Create a temporary directory for the test
        with tempfile.TemporaryDirectory() as tmpdir:
            # Create the configuration files
            config_path = Path(tmpdir) / "flapi.yaml"
            sqls_dir = Path(tmpdir) / "sqls"
            sqls_dir.mkdir()

            # Write minimal flapi.yaml without instructions
            port = find_free_port()
            config_content = f"""project-name: test-no-instructions
project-description: Test project without MCP instructions

template:
  path: ./sqls

connections:
  test-data:
    properties:
      path: ./data/test.parquet

mcp:
  enabled: true
  port: {port}
"""
            config_path.write_text(config_content)

            # Create a minimal endpoint
            endpoint_path = sqls_dir / "test.yaml"
            endpoint_path.write_text("""url-path: /test
method: GET
template-source: test.sql
connection: [test-data]
mcp-tool:
  name: test_tool
  description: Test tool
""")

            # Create SQL template
            sql_path = sqls_dir / "test.sql"
            sql_path.write_text("SELECT 1")

            # Start the server
            log_path = Path(tmpdir) / "mcp_no_instructions.log"
            log_file = open(log_path, "w")
            server_process = subprocess.Popen(
                [
                    "./build/release/flapi",
                    "-c",
                    str(config_path),
                    "--port",
                    str(port),
                ],
                cwd=str(PROJECT_ROOT),
                stdout=log_file,
                stderr=subprocess.STDOUT,
            )

            try:
                # Wait for server to start
                time.sleep(2)

                # Test MCP initialize request
                response = requests.post(
                    f"http://localhost:{port}/mcp/jsonrpc",
                    json={
                        "jsonrpc": "2.0",
                        "id": 1,
                        "method": "initialize",
                        "params": {"protocolVersion": "2024-11-05"},
                    },
                    timeout=10,
                )

                # Verify response
                assert response.status_code == 200
                data = response.json()

                # Verify response is valid without instructions
                assert "result" in data
                result = data["result"]

                # Parse the result if it's a JSON string
                if isinstance(result, str):
                    result = json.loads(result)

                # Instructions should be absent or empty
                if "instructions" in result:
                    assert result["instructions"] == ""

                print("✓ Server initializes successfully without instructions")

            finally:
                # Clean up server
                server_process.terminate()
                server_process.wait(timeout=5)
                log_file.flush()
                log_file.close()


if __name__ == "__main__":
    test = TestMCPInstructions()

    try:
        test.test_initialize_with_file_instructions()
    except Exception as e:
        print(f"✗ test_initialize_with_file_instructions failed: {e}")

    try:
        test.test_initialize_with_inline_instructions()
    except Exception as e:
        print(f"✗ test_initialize_with_inline_instructions failed: {e}")

    try:
        test.test_initialize_without_instructions()
    except Exception as e:
        print(f"✗ test_initialize_without_instructions failed: {e}")

    print("\nAll tests completed!")
