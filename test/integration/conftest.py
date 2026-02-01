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
import hashlib
import hmac

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


def wait_for_server_healthy(base_url, max_retries=30, retry_interval=1.0):
    """Wait for server to be healthy with proper health checks.

    Uses exponential backoff and validates HTTP connectivity.
    Returns True if server is healthy, raises Exception otherwise.
    """
    import requests
    from requests.exceptions import ConnectionError, Timeout

    for attempt in range(max_retries):
        try:
            # Try the root endpoint or a known endpoint
            response = requests.get(base_url, timeout=5)
            if response.status_code in [200, 401, 403, 404]:
                # Any HTTP response means server is up
                print(f"Server healthy at {base_url} (status {response.status_code})")
                return True
        except (ConnectionError, Timeout) as e:
            wait_time = min(retry_interval * (1.2 ** attempt), 5.0)  # Exponential backoff, max 5s
            if attempt < max_retries - 1:
                print(f"Waiting for server (attempt {attempt + 1}/{max_retries}): {e}")
                time.sleep(wait_time)

    raise Exception(f"Server at {base_url} failed health check after {max_retries} attempts")

TEST_JWT_SECRET = "test-jwt-secret-key-for-integration-tests"
TEST_JWT_ISSUER = "flapi-test"
_TEST_JWT_TOKEN = None

def _base64url_encode(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode("utf-8")

def _get_test_jwt_token() -> str:
    """Generate a deterministic HS256 JWT for integration tests."""
    global _TEST_JWT_TOKEN
    if _TEST_JWT_TOKEN is not None:
        return _TEST_JWT_TOKEN

    header = {"alg": "HS256", "typ": "JWT"}
    now = int(time.time())
    payload = {
        "iss": TEST_JWT_ISSUER,
        "sub": "integration-test",
        "roles": ["write", "read"],
        "iat": now,
        "exp": now + 3600,
    }

    header_b64 = _base64url_encode(json.dumps(header, separators=(",", ":")).encode("utf-8"))
    payload_b64 = _base64url_encode(json.dumps(payload, separators=(",", ":")).encode("utf-8"))
    signing_input = f"{header_b64}.{payload_b64}".encode("utf-8")
    signature = hmac.new(TEST_JWT_SECRET.encode("utf-8"), signing_input, hashlib.sha256).digest()
    signature_b64 = _base64url_encode(signature)

    _TEST_JWT_TOKEN = f"{header_b64}.{payload_b64}.{signature_b64}"
    return _TEST_JWT_TOKEN


@pytest.fixture(scope="function")
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
    log_path = os.path.join(temp_dir, "flapi_test.log")
    log_file = open(log_path, "w")
    process = subprocess.Popen(
        [
            str(flapi_binary),
            "-c", str(config_path),
            "-p", str(port),
            "--log-level", "debug",
            "--config-service",
            "--config-service-token", "test-token"
        ],
        stdout=log_file,
        stderr=subprocess.STDOUT,
        preexec_fn=os.setsid,
        cwd=temp_dir  # Run in temp directory to use unique db file
    )
    process.log_path = log_path
    process.log_file = log_file
    print(f"Server logs: {log_path}")

    # Check immediately if the process failed to start
    if process.poll() is not None:
        log_file.flush()
        log_file.close()
        with open(log_path, "r") as log_reader:
            log_contents = log_reader.read()
        print(f"Server process failed to start with code {process.returncode}")
        print(f"LOGS: {log_contents}")
        print(f"Command was: {process.args}")
        print(f"Working directory: {temp_dir}")
        raise Exception(f"Server process failed to start with code {process.returncode}")

    # Check if process is still running immediately after starting
    time.sleep(1)  # Short wait to see if process exits immediately

    # Check immediately if the process failed
    if process.poll() is not None:
        log_file.flush()
        log_file.close()
        with open(log_path, "r") as log_reader:
            log_contents = log_reader.read()
        print(f"Server process exited immediately with code {process.returncode}")
        print(f"LOGS: {log_contents}")
        print(f"Command was: {process.args}")
        print(f"Working directory: {temp_dir}")
        raise Exception(f"Server process exited immediately with code {process.returncode}")

    # Wait for server to become healthy using proper health checks
    base_url = f"http://localhost:{port}"
    try:
        wait_for_server_healthy(base_url, max_retries=30, retry_interval=1.0)
    except Exception as e:
        # Server failed to start - capture output for debugging
        if process.poll() is not None:
            log_file.flush()
            log_file.close()
            with open(log_path, "r") as log_reader:
                log_contents = log_reader.read()
            print(f"Server process exited with code {process.returncode}")
            print(f"LOGS: {log_contents}")
        else:
            # Process is running but not responding
            print(f"Server process running (pid {process.pid}) but not responding")
            # Kill the process since it's not healthy
            try:
                os.killpg(os.getpgid(process.pid), signal.SIGTERM)
                process.wait(timeout=5)
            except Exception:
                os.killpg(os.getpgid(process.pid), signal.SIGKILL)
        raise Exception(f"Server failed to become healthy: {e}")
    
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
    finally:
        log_file.flush()
        log_file.close()

    # Clean up environment variables
    os.environ.pop('FLAPI_TEST_DUCKLAKE_METADATA', None)
    os.environ.pop('FLAPI_TEST_DUCKLAKE_DATA', None)
    print("Cleaned up test environment variables")

    # Clean up temporary directory (includes isolated DuckLake files)
    import shutil
    keep_temp = os.getenv("FLAPI_TEST_KEEP_TMP", "").lower() in {"1", "true", "yes"}
    if keep_temp:
        print(f"Keeping temporary directory for inspection: {temp_dir}")
    else:
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

    Note: With session-scoped flapi_server, the server is already health-checked
    at startup. This fixture now just verifies connectivity for each test as a
    fast sanity check.

    Note: This fixture is skipped for examples tests (test_examples_*.py)
    which use their own examples_server fixture.
    """
    global _flapi_server_instance

    # Skip for tests that manage their own servers
    if _is_standalone_server_test(request):
        return

    # Skip this fixture for examples tests - they use examples_server instead
    if _is_examples_test(request) or _needs_examples_server(request):
        return

    if "examples_server" in request.fixturenames:
        return

    # Use the global server instance set by inject_tavern_base_url
    if _flapi_server_instance is None:
        return

    # Quick connectivity check - server should already be healthy from startup
    import requests
    from requests.exceptions import ConnectionError

    port = _flapi_server_instance.port
    base_url = f"http://localhost:{port}"

    try:
        response = requests.get(base_url, timeout=5)
        # Any HTTP response means server is up
        if response.status_code not in [200, 401, 403, 404]:
            print(f"Warning: Unexpected status {response.status_code} from {base_url}")
    except ConnectionError as e:
        # Server may have crashed between tests - give it a moment
        print(f"Server connectivity issue, retrying: {e}")
        wait_for_server_healthy(base_url, max_retries=10, retry_interval=0.5)

@pytest.fixture
def flapi_base_url(flapi_server):
    """Provide the base URL for the FLAPI server."""
    return f"http://localhost:{flapi_server.port}"

@pytest.fixture
def base_url(flapi_server):
    """Provide base_url for Tavern tests (uses flapi_server.port)."""
    return f"http://localhost:{flapi_server.port}"

@pytest.fixture
def jwt_token():
    """Provide a test JWT for bearer-auth endpoints."""
    return _get_test_jwt_token()


# Global variable to store base_url for Tavern hook
_flapi_base_url_for_tavern = None
_flapi_server_instance = None


def _is_examples_test(request):
    """Check if the current test is an examples test (test_examples_*.py)."""
    # Check the test module name
    if hasattr(request, 'fspath') and request.fspath:
        return 'test_examples' in str(request.fspath)
    if hasattr(request, 'node') and hasattr(request.node, 'fspath'):
        return 'test_examples' in str(request.node.fspath)
    return False

def _needs_examples_server(request):
    """Check if the current test should use the examples server."""
    if _is_examples_test(request):
        return True
    if hasattr(request, "node") and request.node.get_closest_marker("examples") is not None:
        return True
    return False


def _is_standalone_server_test(request):
    """Check if the test manages its own server (marked with standalone_server)."""
    if hasattr(request, "node") and request.node.get_closest_marker("standalone_server") is not None:
        return True
    return False


@pytest.fixture(autouse=True)
def inject_tavern_base_url(request):
    """Per-test autouse fixture that ensures base_url is available for Tavern tests.

    This fixture:
    1. Skips for standalone_server tests (they manage their own servers)
    2. Skips for examples tests (they use examples_server)
    3. Triggers flapi_server startup (session-scoped, runs once)
    4. Sets the global base_url for Tavern hook

    Note: This fixture is skipped for examples tests (test_examples_*.py)
    which use their own examples_server fixture.
    """
    global _flapi_base_url_for_tavern, _flapi_server_instance

    # Skip for tests that manage their own servers
    if _is_standalone_server_test(request):
        return

    if _needs_examples_server(request) or "examples_server" in request.fixturenames:
        examples_server = request.getfixturevalue("examples_server")
        _flapi_server_instance = examples_server
        _flapi_base_url_for_tavern = f"http://localhost:{examples_server.port}"
        return

    # Get the session-scoped flapi_server (will only start once per session)
    flapi_server = request.getfixturevalue("flapi_server")
    _flapi_server_instance = flapi_server
    _flapi_base_url_for_tavern = f"http://localhost:{flapi_server.port}"


def pytest_tavern_beta_before_every_test_run(test_dict, variables):
    """Hook to inject base_url into Tavern tests.

    This is called after fixtures are loaded but before test execution.
    We read the base_url from the global variable set by inject_tavern_base_url.
    """
    global _flapi_base_url_for_tavern
    if _flapi_base_url_for_tavern:
        variables["base_url"] = _flapi_base_url_for_tavern
    variables["jwt_token"] = _get_test_jwt_token()


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


@pytest.fixture(scope="session")
def examples_server():
    """Start flapi server with examples configuration.

    Reuses the same infrastructure as flapi_server but points to examples/flapi-test.yaml.
    Uses session scope so server starts once for the test session.

    Note: This fixture runs the examples server with its native configuration,
    using the DuckLake cache files in examples/data/. This tests the examples
    as they would actually run in production.

    ISOLATION: To prevent SIGABRT crashes from DuckDB extension conflicts (especially
    ERPL), we isolate the DuckDB environment by setting DUCKDB_NO_EXTENSION_AUTOLOADING
    and providing a clean HOME directory for extension caching.
    """
    import tempfile
    import shutil

    # Get the current directory where conftest.py is located
    current_dir = pathlib.Path(__file__).parent
    # Project root is two levels up from test/integration
    project_root = current_dir.parent.parent
    # Config path relative to project root
    config_path = project_root / "examples" / "flapi-test.yaml"

    if not config_path.exists():
        raise FileNotFoundError(f"Examples config not found at {config_path}")

    # Reuse existing helper functions
    flapi_binary = get_flapi_binary()
    port = find_free_port()

    # Create a temp directory for test artifacts AND DuckDB isolation
    temp_dir = tempfile.mkdtemp(prefix="flapi_examples_test_")
    temp_examples_root = pathlib.Path(temp_dir) / "examples"
    shutil.copytree(project_root / "examples", temp_examples_root)

    # Remove DuckLake metadata files - they contain embedded paths to the original location
    # which causes "data path mismatch" errors when running from the temp directory.
    # Removing these files forces DuckLake to create fresh metadata with correct paths.
    ducklake_patterns = ["*.ducklake", "*.ducklake.wal"]
    for pattern in ducklake_patterns:
        for ducklake_file in temp_examples_root.rglob(pattern):
            try:
                ducklake_file.unlink()
                print(f"Removed stale DuckLake metadata: {ducklake_file}")
            except Exception as e:
                print(f"Warning: Could not remove {ducklake_file}: {e}")

    print(f"Starting examples server from: {flapi_binary} on port {port}")
    print(f"Using examples config: {config_path}")
    print(f"Working directory: {temp_dir}")

    # Start server from project root (examples config paths are relative to project root)
    # Using flapi-test.yaml which excludes ERPL extension to avoid SIGABRT crashes
    log_path = os.path.join(temp_dir, "flapi_examples.log")
    log_file = open(log_path, "w")
    process = subprocess.Popen(
        [
            str(flapi_binary),
            "-c", "examples/flapi-test.yaml",  # Test config without ERPL extension
            "-p", str(port),
            "--log-level", "info"
        ],
        stdout=log_file,
        stderr=subprocess.STDOUT,
        preexec_fn=os.setsid,
        cwd=str(temp_dir)  # Run from isolated copy of examples
    )
    process.log_path = log_path
    process.log_file = log_file
    print(f"Examples server logs: {log_path}")

    # Check immediately if the process failed to start
    time.sleep(1)
    if process.poll() is not None:
        log_file.flush()
        log_file.close()
        with open(log_path, "r") as log_reader:
            log_contents = log_reader.read()
        print(f"Examples server failed to start with code {process.returncode}")
        print(f"LOGS: {log_contents}")
        raise Exception(f"Examples server failed to start with code {process.returncode}")

    # Wait for server to become healthy using proper health checks
    # Examples server may take longer due to data loading and extensions
    base_url = f"http://localhost:{port}"
    try:
        wait_for_server_healthy(base_url, max_retries=60, retry_interval=1.0)
    except Exception as e:
        # Server failed to start - capture output for debugging
        if process.poll() is not None:
            log_file.flush()
            log_file.close()
            with open(log_path, "r") as log_reader:
                log_contents = log_reader.read()
            print(f"Examples server exited with code {process.returncode}")
            print(f"LOGS: {log_contents}")
        else:
            print(f"Examples server running (pid {process.pid}) but not responding")
            try:
                os.killpg(os.getpgid(process.pid), signal.SIGTERM)
                process.wait(timeout=5)
            except Exception:
                os.killpg(os.getpgid(process.pid), signal.SIGKILL)
        raise Exception(f"Examples server failed to become healthy: {e}")

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
    finally:
        log_file.flush()
        log_file.close()

    # Clean up temporary directory
    keep_temp = os.getenv("FLAPI_TEST_KEEP_TMP", "").lower() in {"1", "true", "yes"}
    if keep_temp:
        print(f"Keeping examples temp directory for inspection: {temp_dir}")
    else:
        try:
            shutil.rmtree(temp_dir)
            print(f"Cleaned up examples temp directory: {temp_dir}")
        except Exception as e:
            print(f"Warning: Failed to clean up temp directory {temp_dir}: {e}")


@pytest.fixture
def examples_url(examples_server):
    """Provide base URL for examples server."""
    return f"http://localhost:{examples_server.port}"


@pytest.fixture
def examples_auth():
    """Default authentication tuple for examples endpoints."""
    return ("admin", "secret")


@pytest.fixture(scope="session")
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
