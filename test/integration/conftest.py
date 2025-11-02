import pytest
import subprocess
import time
import signal
import os
import pathlib
import base64
from datetime import datetime
import uuid
import json
import asyncio
from contextlib import AsyncExitStack
import socket

def get_flapi_binary():
    """Get the path to the flapi binary based on build type"""
    current_dir = pathlib.Path(__file__).parent
    build_type = os.getenv("FLAPI_BUILD_TYPE", "release").lower()
    
    # Map build types to directory names
    build_dirs = {
        "debug": "debug",
        "release": "release"
    }
    
    build_dir = build_dirs.get(build_type, "release")  # Default to release if unknown
    binary_path = current_dir.parent.parent / "build" / build_dir / "flapi"
    
    if not binary_path.exists():
        raise FileNotFoundError(f"FLAPI binary not found at {binary_path}. "
                              f"Make sure to build FLAPI in {build_type} mode first.")
    
    return binary_path

def find_free_port():
    """Find a free port on the local machine."""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(('', 0))
        s.listen(1)
        port = s.getsockname()[1]
    return port

@pytest.fixture
def flapi_server():
    # Get the current directory where conftest.py is located
    current_dir = pathlib.Path(__file__).parent
    config_path = current_dir / "api_configuration" / "flapi.yaml"
    
    # Get the appropriate flapi binary
    flapi_binary = get_flapi_binary()
    
    # Find a free port
    port = find_free_port()
    
    # Create a unique database file for this test run to avoid conflicts
    import tempfile
    temp_dir = tempfile.mkdtemp(prefix="flapi_test_")
    db_path = os.path.join(temp_dir, "flapi_test.db")
    
    print(f"Starting FLAPI binary from: {flapi_binary} on port {port}")
    print(f"Using temporary database: {db_path}")
    
    # Start flapi binary with configuration and port
    # Enable config service for integration tests with a test token
    process = subprocess.Popen(
        [
            str(flapi_binary),
            "-c", str(config_path),
            "-p", str(port),
            "--log-level", "debug",
            "--config-service",
            "--config-service-token", "test-token"
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        preexec_fn=os.setsid,
        cwd=temp_dir  # Run in temp directory to use unique db file
    )

    # Check immediately if the process failed to start
    if process.poll() is not None:
        stdout, stderr = process.communicate()
        print(f"Server process failed to start with code {process.returncode}")
        print(f"STDOUT: {stdout.decode()}")
        print(f"STDERR: {stderr.decode()}")
        print(f"Command was: {process.args}")
        print(f"Working directory: {temp_dir}")
        raise Exception(f"Server process failed to start with code {process.returncode}")

    # Check if process is still running immediately after starting
    time.sleep(1)  # Short wait to see if process exits immediately

    # Check immediately if the process failed
    if process.poll() is not None:
        stdout, stderr = process.communicate()
        print(f"Server process exited immediately with code {process.returncode}")
        print(f"STDOUT: {stdout.decode()}")
        print(f"STDERR: {stderr.decode()}")
        print(f"Command was: {process.args}")
        print(f"Working directory: {temp_dir}")
        raise Exception(f"Server process exited immediately with code {process.returncode}")

    # Wait for server to start
    time.sleep(5)  # Increased wait time for server startup

    # Check if process is still running
    if process.poll() is not None:
        stdout, stderr = process.communicate()
        print(f"Server process exited with code {process.returncode}")
        print(f"STDOUT: {stdout.decode()}")
        print(f"STDERR: {stderr.decode()}")
        print(f"Command was: {process.args}")
        print(f"Working directory: {temp_dir}")
        raise Exception(f"Server process exited unexpectedly with code {process.returncode}")

    # If process is still running, try to read some output without blocking
    try:
        # Read a small amount of output without blocking
        import select
        if process.poll() is None:  # Process is still running
            # Check if there's data to read
            ready, _, _ = select.select([process.stdout, process.stderr], [], [], 0.1)
            for stream in ready:
                if stream == process.stdout:
                    data = os.read(process.stdout.fileno(), 500)
                    if data:
                        print(f"Server STDOUT: {data.decode()[:500]}")
                elif stream == process.stderr:
                    data = os.read(process.stderr.fileno(), 500)
                    if data:
                        print(f"Server STDERR: {data.decode()[:500]}")
    except Exception as e:
        print(f"Could not read server output: {e}")
    
    # Store the port and temp dir in the process object for other fixtures to access
    process.port = port
    process.temp_dir = temp_dir
    
    yield process
    
    # Cleanup: Kill the server
    os.killpg(os.getpgid(process.pid), signal.SIGTERM)
    process.wait()
    
    # Clean up temporary directory
    import shutil
    try:
        shutil.rmtree(temp_dir)
        print(f"Cleaned up temporary directory: {temp_dir}")
    except Exception as e:
        print(f"Warning: Failed to clean up temporary directory {temp_dir}: {e}")

@pytest.fixture(autouse=True)
def wait_for_api(flapi_server):
    """Ensure API has started before running tests"""
    max_retries = 30
    retry_interval = 1
    
    import requests
    from requests.exceptions import ConnectionError
    
    port = flapi_server.port
    base_url = f"http://localhost:{port}"
    
    for attempt in range(max_retries):
        try:
            # Health endpoint might not require auth
            print(f"Attempt {attempt + 1}/{max_retries}: Checking server at {base_url}")
            response = requests.get(base_url, timeout=5)
            print(f"Response status: {response.status_code}")
            if response.status_code == 200:
                print(f"Server is ready on port {port}")
                return
        except ConnectionError as e:
            print(f"Connection error on attempt {attempt + 1}: {e}")
            time.sleep(retry_interval)
            
    raise Exception(f"API failed to start on port {port}")

@pytest.fixture
def flapi_base_url(flapi_server):
    """Provide the base URL for the FLAPI server."""
    return f"http://localhost:{flapi_server.port}"

@pytest.fixture
def base_url(flapi_server):
    """Provide base_url for Tavern tests (uses flapi_server.port)."""
    return f"http://localhost:{flapi_server.port}"

def verify_data_types(response, expected_types):
    """Verify that the data types in the response match the expected types."""
    if not response.get("data") or not isinstance(response["data"], list):
        raise ValueError("Response data must be a non-empty list")
    
    # Check first row for type verification
    first_row = response["data"][0]
    
    for column, expected_type in expected_types.items():
        if column not in first_row:
            raise ValueError(f"Column {column} not found in response")
        
        value = first_row[column]
        
        # Skip null values
        if value is None:
            continue
            
        # Verify types
        if expected_type == "!integer":
            if not isinstance(value, int):
                raise ValueError(f"Column {column} expected integer, got {type(value)}")
        
        elif expected_type == "!float":
            if not isinstance(value, (float, int)):
                raise ValueError(f"Column {column} expected float, got {type(value)}")
        
        elif expected_type == "!string":
            if not isinstance(value, str):
                raise ValueError(f"Column {column} expected string, got {type(value)}")
        
        elif expected_type == "!boolean":
            if not isinstance(value, bool):
                raise ValueError(f"Column {column} expected boolean, got {type(value)}")
        
        elif expected_type == "!anylist":
            if not isinstance(value, list):
                raise ValueError(f"Column {column} expected array, got {type(value)}")
        
        elif expected_type == "!anyjson":
            # Try to parse if it's a string
            if isinstance(value, str):
                try:
                    json.loads(value)
                except json.JSONDecodeError:
                    raise ValueError(f"Column {column} expected valid JSON")
            elif not isinstance(value, (dict, list)):
                raise ValueError(f"Column {column} expected JSON object or array, got {type(value)}")

    return True

@pytest.fixture
def setup_data_types_table():
    """Fixture to ensure the data_types table exists and has test data."""
    # This would be implemented based on your setup needs
    pass

@pytest.fixture
def flapi_mcp_client(flapi_server, flapi_base_url):
    """Fixture to provide an MCP client for testing FLAPI MCP server."""
    from test_mcp_client import SimpleMCPClient
    
    # Wait for MCP endpoint to be available
    import requests
    from requests.exceptions import ConnectionError
    import time

    max_retries = 10
    retry_interval = 1

    for _ in range(max_retries):
        try:
            response = requests.get(f"{flapi_base_url}/mcp/health", timeout=5)
            if response.status_code in [200, 404]:  # 404 is OK if health endpoint not implemented
                break
        except ConnectionError:
            time.sleep(retry_interval)
    else:
        raise Exception("MCP endpoint failed to start")

    # Return a simple HTTP-based MCP client
    return SimpleMCPClient(flapi_base_url) 