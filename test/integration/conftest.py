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
    """Get the path to the flapi binary based on build type.

    Handles platform-specific build directories:
    - macOS: prefers universal binary, then architecture-specific (release-arm64, release-x86_64)
    - Linux/Windows: uses standard debug/release directories
    """
    import platform

    current_dir = pathlib.Path(__file__).parent
    build_type = os.getenv("FLAPI_BUILD_TYPE", "release").lower()
    build_base = current_dir.parent.parent / "build"

    # On macOS, prefer universal binary, then architecture-specific builds
    if platform.system() == "Darwin":
        # Check for universal binary first (works on both Intel and Apple Silicon)
        universal_path = build_base / "universal" / "flapi"
        if universal_path.exists():
            return universal_path

        # Check for architecture-specific builds
        arch = platform.machine()  # 'arm64' or 'x86_64'
        if build_type == "release":
            arch_path = build_base / f"release-{arch}" / "flapi"
            if arch_path.exists():
                return arch_path

    # Fall back to standard paths (debug, or non-macOS release)
    standard_path = build_base / build_type / "flapi"
    if standard_path.exists():
        return standard_path

    # List available binaries for better error message
    available = list(build_base.glob("*/flapi"))
    raise FileNotFoundError(
        f"FLAPI binary not found. Checked paths:\n"
        f"  - {standard_path}\n"
        f"Available binaries: {[str(p) for p in available]}\n"
        f"Build FLAPI first with 'make {build_type}' or 'make release'"
    )

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

    # Set unique DuckLake paths for this test run to avoid file locking conflicts
    # Each test gets isolated cache files in its temp directory
    ducklake_metadata = os.path.join(temp_dir, "test_cache.ducklake")
    ducklake_data = os.path.join(temp_dir, "test_cache_data")
    os.makedirs(ducklake_data, exist_ok=True)

    # Set environment variables that will be picked up by config parser
    os.environ['FLAPI_TEST_DUCKLAKE_METADATA'] = ducklake_metadata
    os.environ['FLAPI_TEST_DUCKLAKE_DATA'] = ducklake_data

    print(f"Starting FLAPI binary from: {flapi_binary} on port {port}")
    print(f"Using temporary database: {db_path}")
    print(f"Using isolated DuckLake cache: {ducklake_metadata}")
    
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

    # Cleanup: Graceful shutdown with timeout
    try:
        os.killpg(os.getpgid(process.pid), signal.SIGTERM)
        try:
            process.wait(timeout=5)  # Wait up to 5 seconds for clean shutdown
            print(f"Process {process.pid} exited cleanly")
        except subprocess.TimeoutExpired:
            # Force kill if graceful shutdown fails
            print(f"Warning: Graceful shutdown timed out, force killing process {process.pid}")
            os.killpg(os.getpgid(process.pid), signal.SIGKILL)
            process.wait()
    except ProcessLookupError:
        pass  # Process already terminated

    # Clean up environment variables
    os.environ.pop('FLAPI_TEST_DUCKLAKE_METADATA', None)
    os.environ.pop('FLAPI_TEST_DUCKLAKE_DATA', None)
    print("Cleaned up test environment variables")

    # Clean up temporary directory (includes isolated DuckLake files)
    import shutil
    try:
        shutil.rmtree(temp_dir)
        print(f"Cleaned up temporary directory: {temp_dir}")
    except Exception as e:
        print(f"Warning: Failed to clean up temporary directory {temp_dir}: {e}")

    # Clean up any stray lock files in api_configuration (legacy cleanup for robustness)
    current_dir = pathlib.Path(__file__).parent
    api_config_dir = current_dir / "api_configuration"
    for pattern in ["*.ducklake*", "*.wal"]:
        for lock_file in api_config_dir.glob(pattern):
            try:
                lock_file.unlink()
                print(f"Removed legacy lock file: {lock_file}")
            except (OSError, PermissionError) as e:
                print(f"Warning: Could not remove {lock_file}: {e}")

@pytest.fixture(autouse=True)
def wait_for_api(request):
    """Ensure API has started before running tests.

    Note: This fixture is skipped for examples tests (test_examples_*.py)
    which use their own examples_server fixture.
    """
    # Skip this fixture for examples tests - they use examples_server instead
    if "examples_server" in request.fixturenames:
        return

    # Only proceed if flapi_server is being used
    if "flapi_server" not in request.fixturenames:
        return

    # Get the flapi_server fixture
    flapi_server = request.getfixturevalue("flapi_server")

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


# Global variable to store base_url for Tavern hook
_flapi_base_url_for_tavern = None


@pytest.fixture(autouse=True)
def inject_tavern_base_url(request):
    """Automatically inject base_url for Tavern tests.

    This autouse fixture ensures that flapi_server is started for Tavern tests
    and makes base_url available via pytest_tavern_beta_before_every_test_run hook.

    Note: This fixture is skipped for examples tests (test_examples_*.py)
    which use their own examples_server fixture.
    """
    global _flapi_base_url_for_tavern

    # Skip this fixture for examples tests
    if "examples_server" in request.fixturenames:
        return

    # Only proceed if flapi_server is being used
    if "flapi_server" not in request.fixturenames:
        return

    # Get the flapi_server fixture
    flapi_server = request.getfixturevalue("flapi_server")
    _flapi_base_url_for_tavern = f"http://localhost:{flapi_server.port}"


def pytest_tavern_beta_before_every_test_run(test_dict, variables):
    """Hook to inject base_url into Tavern tests.

    This is called after fixtures are loaded but before test execution.
    We read the base_url from the global variable where inject_tavern_base_url stored it.
    """
    global _flapi_base_url_for_tavern
    if _flapi_base_url_for_tavern:
        variables["base_url"] = _flapi_base_url_for_tavern


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


# =============================================================================
# Examples Server Fixtures
# =============================================================================
# These fixtures run the flapi server with the examples/ configuration
# to verify that the examples actually work.


@pytest.fixture(scope="module")
def examples_server():
    """Start flapi server with examples configuration.

    Reuses the same infrastructure as flapi_server but points to examples/flapi.yaml.
    Uses module scope so server starts once per test file.

    Note: This fixture runs the examples server with its native configuration,
    using the DuckLake cache files in examples/data/. This tests the examples
    as they would actually run in production.
    """
    import tempfile
    import shutil

    # Get the current directory where conftest.py is located
    current_dir = pathlib.Path(__file__).parent
    # Project root is two levels up from test/integration
    project_root = current_dir.parent.parent
    # Config path relative to project root
    config_path = project_root / "examples" / "flapi.yaml"

    if not config_path.exists():
        raise FileNotFoundError(f"Examples config not found at {config_path}")

    # Reuse existing helper functions
    flapi_binary = get_flapi_binary()
    port = find_free_port()

    # Create a temp directory for any test artifacts
    temp_dir = tempfile.mkdtemp(prefix="flapi_examples_test_")

    print(f"Starting examples server from: {flapi_binary} on port {port}")
    print(f"Using examples config: {config_path}")
    print(f"Working directory: {project_root}")

    # Start server from project root (examples config paths are relative to project root)
    # Note: Using debug log level as a workaround for an issue where some endpoints
    # incorrectly return 401 with info log level (possible race condition in auth loading)
    process = subprocess.Popen(
        [
            str(flapi_binary),
            "-c", "examples/flapi.yaml",  # Use relative path from project root
            "-p", str(port),
            "--log-level", "debug"
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        preexec_fn=os.setsid,
        cwd=str(project_root)  # Run from project root
    )

    # Check immediately if the process failed to start
    time.sleep(1)
    if process.poll() is not None:
        stdout, stderr = process.communicate()
        print(f"Examples server failed to start with code {process.returncode}")
        print(f"STDOUT: {stdout.decode()}")
        print(f"STDERR: {stderr.decode()}")
        raise Exception(f"Examples server failed to start with code {process.returncode}")

    # Wait for server to start (examples config loads more data, including ERPL extensions)
    time.sleep(10)

    # Check if process is still running
    if process.poll() is not None:
        stdout, stderr = process.communicate()
        print(f"Examples server exited with code {process.returncode}")
        print(f"STDOUT: {stdout.decode()}")
        print(f"STDERR: {stderr.decode()}")
        raise Exception(f"Examples server exited unexpectedly with code {process.returncode}")

    # Store the port and temp dir in the process object
    process.port = port
    process.temp_dir = temp_dir

    yield process

    # Cleanup: Graceful shutdown with timeout
    try:
        os.killpg(os.getpgid(process.pid), signal.SIGTERM)
        try:
            process.wait(timeout=5)
            print(f"Examples server {process.pid} exited cleanly")
        except subprocess.TimeoutExpired:
            print(f"Warning: Graceful shutdown timed out, force killing examples server")
            os.killpg(os.getpgid(process.pid), signal.SIGKILL)
            process.wait()
    except ProcessLookupError:
        pass  # Process already terminated

    # Clean up temporary directory
    try:
        shutil.rmtree(temp_dir)
        print(f"Cleaned up examples temp directory: {temp_dir}")
    except Exception as e:
        print(f"Warning: Failed to clean up temp directory {temp_dir}: {e}")


@pytest.fixture
def examples_url(examples_server):
    """Provide base URL for examples server."""
    return f"http://localhost:{examples_server.port}"


@pytest.fixture(scope="module")
def wait_for_examples(examples_server):
    """Wait for examples server to be ready.

    Uses module scope to match examples_server fixture scope. This ensures
    we only wait once per test module, not before each test.
    """
    import requests
    from requests.exceptions import ConnectionError, ReadTimeout

    port = examples_server.port
    max_retries = 60  # Increased to handle cache warmup time

    for attempt in range(max_retries):
        try:
            # Try a simple endpoint - northwind has no auth
            # Use longer timeout as server may be busy with cache warmup
            response = requests.get(f"http://localhost:{port}/northwind/products/", timeout=10)
            if response.status_code in [200, 401]:  # 401 OK - server is responding
                print(f"Examples server is ready on port {port}")
                return
        except (ConnectionError, ReadTimeout):
            time.sleep(1)

    raise Exception(f"Examples server failed to start on port {port}") 