# Integration Test Status Report

**Date**: 2026-01-29
**Test Run**: `make integration-test-ci`
**Results**: 65 failed, 209 passed, 10 skipped, 75 errors (359 total tests)

---

## Executive Summary

The integration test suite has significant failures across multiple categories. The primary issues are:

1. **Examples Server Fixture Crash** (75 errors) - The `examples_server` pytest fixture crashes with SIGABRT when starting
2. **Server Connectivity Issues** (30+ failures) - Many tests fail with "Connection refused" errors
3. **Missing Test Fixtures** (5+ failures) - Some tests have misconfigured fixtures

The failures appear to be **pre-existing infrastructure issues**, not regressions from recent code changes.

---

## Failure Categories

### Category 1: Examples Server Crash (75 ERROR tests)

**Affected Test Files:**
- `test_arrow_streaming.py` (30 tests)
- `test_examples_customers.py` (5 tests)
- `test_examples_mcp.py` (5 tests)
- `test_examples_northwind.py` (9 tests)
- `test_mcp_integration.py` (26 tests - partial)

**Symptom:**
```
Exception: Examples server exited unexpectedly with code -6
```

**Root Cause Analysis:**

The `examples_server` fixture (`conftest.py:395-490`) attempts to start a flapi server using `examples/flapi.yaml`. The server crashes with SIGABRT (signal 6) during startup.

Key observations:
1. **Works standalone**: Running `./build/universal/flapi -c examples/flapi.yaml -p 9999` succeeds
2. **Crashes during tests**: The same server crashes when started via the test fixture
3. **ERPL extension loading**: The crash occurs after DuckDB extensions (including ERPL/SAP) are loaded

**Likely Causes:**
1. **Resource conflict**: Multiple DuckDB processes may conflict when loading extensions
2. **ERPL extension instability**: The SAP ERPL extension creates persistent secrets which may conflict
3. **DuckLake file locking**: Despite isolation attempts, there may be shared state

**Evidence from logs:**
```
STDOUT: -- Loading ERPL Trampoline Extension. --
ERPL RFC extension installed and loaded.
ERPL TUNNEL extension installed and loaded.
ERPL BICS extension installed and loaded.
...
Examples server exited with code -6
```

**Proposed Fix:**

Option A (Recommended): **Isolate examples_server completely**
```python
# In conftest.py examples_server fixture:
# Create completely isolated DuckDB home directory
env = os.environ.copy()
env['DUCKDB_HOME'] = tempfile.mkdtemp(prefix="duckdb_home_")
env['HOME'] = tempfile.mkdtemp(prefix="flapi_home_")  # Isolate extension cache

process = subprocess.Popen(
    [...],
    env=env,  # Use isolated environment
)
```

Option B: **Disable ERPL extensions for tests**
```yaml
# Create examples/flapi-test.yaml without sap-abap-trial connection
```

Option C: **Run examples tests in separate process**
```bash
# Run examples tests after all other tests complete
pytest test_examples_*.py --forked
```

---

### Category 2: Server Connectivity Failures (30+ FAILED tests)

**Affected Test Files:**
- `test_customers.tavern.yaml` (3 tests)
- `test_customers_cached.tavern.yaml` (1 test)
- `test_data_types.tavern.yaml` (2 tests)
- `test_northwind_*.tavern.yaml` (3 tests)
- `test_rate_limiting.tavern.yaml` (3 tests)
- `test_write_operations*.tavern.yaml` (3 tests)
- `test_multi_statement_execution.tavern.yaml` (1 test)
- `test_request_validation.tavern.yaml` (1 test)

**Symptom:**
```
ConnectionError: HTTPConnectionPool(host='localhost', port=51277):
Max retries exceeded with url: /customers/
(Caused by NewConnectionError: Failed to establish a new connection: [Errno 61] Connection refused)
```

**Root Cause Analysis:**

The `flapi_server` fixture has **function scope** (default), meaning a new server starts for each test. The server appears to crash or stop between fixture setup and test execution.

Possible causes:
1. **Server crash during warmup**: DuckLake cache warming may fail
2. **Port binding race**: The port may be released before the server binds
3. **Fixture scope mismatch**: Tavern tests may expect session-scoped server

**Evidence:**
- Server starts on port 51277 (from logs)
- Tests immediately get "Connection refused" on the same port
- Different ports are used for different test runs (51277, 55932, etc.)

**Proposed Fix:**

Option A (Recommended): **Change fixture scope to session**
```python
@pytest.fixture(scope="session")
def flapi_server():
    # Single server for all tests
    ...
```

Option B: **Add server health check after startup**
```python
# After starting server, verify it responds to requests
def wait_for_server(port, timeout=30):
    for _ in range(timeout):
        try:
            r = requests.get(f"http://localhost:{port}/api/v1/_ping")
            if r.status_code == 200:
                return True
        except:
            pass
        time.sleep(1)
    return False
```

Option C: **Investigate server crash logs**
```python
# Add more detailed error capture
if process.poll() is not None:
    # Read all remaining output
    stdout, stderr = process.communicate()
    # Write to file for debugging
    with open('/tmp/flapi_crash.log', 'w') as f:
        f.write(f"STDOUT:\n{stdout.decode()}\nSTDERR:\n{stderr.decode()}")
```

---

### Category 3: Missing Format Variables (5+ FAILED tests)

**Affected Test Files:**
- `test_config_service_endpoints.tavern.yaml` (1 test)
- `api_configuration/sqls/test_auth_debug.tavern.yaml` (1 test)

**Symptom:**
```
MissingFormatError: base_url
Format variables:
  base_url = '???'
```

**Root Cause:**

The Tavern tests require a `base_url` format variable that should be provided by a pytest fixture. The fixture either:
1. Is not being invoked (wrong scope or marker)
2. Returns an invalid value
3. Is missing from the test file

**Evidence:**
```yaml
# test_config_service_endpoints.tavern.yaml
Source test stage (line 11):
  - name: List all endpoints
    request:
      url: "{base_url}/api/v1/_config/endpoints"
```

**Proposed Fix:**

Check that Tavern tests use the correct fixture:
```yaml
# In tavern test file, add marks:
marks:
  - usefixtures:
      - base_url
      - config_service_server
```

Or add conftest hook:
```python
@pytest.fixture
def base_url(config_service_server):
    """Provide base_url for Tavern tests."""
    return f"http://localhost:{config_service_server.port}"
```

---

### Category 4: MCP Config Tools Failures (20 FAILED tests)

**Affected Test Files:**
- `test_mcp_config_tools.py` (20 tests)
- `test_mcp_config_workflow.py` (6 tests)

**Symptom:**
Various assertion failures in MCP tool operations.

**Root Cause Analysis:**

These tests exercise the MCP configuration tools (`flapi_get_template`, `flapi_update_endpoint`, etc.). The failures cascade from server connectivity issues in Category 2.

When the server is unavailable:
1. MCP tool calls fail
2. Assertions on response content fail
3. Cleanup operations fail

**Proposed Fix:**

Fix server connectivity (Category 2) first. These tests should pass once the server is stable.

---

### Category 5: Load Testing Failures (8 FAILED tests)

**Affected Test Files:**
- `test_load_testing.py` (8 tests)

**Tests:**
- `test_concurrent_get_requests`
- `test_concurrent_post_requests`
- `test_mixed_read_write_operations`
- `test_no_deadlocks_or_timeouts`
- `test_sustained_load_1000_requests`
- `test_consistent_performance`
- `test_maximum_concurrent_connections`
- `test_large_payload_handling`

**Root Cause:**

Load tests stress the server with concurrent requests. Failures occur because:
1. Server may not handle high concurrency well
2. Server may crash under load
3. Connection pool exhaustion

**Proposed Fix:**

1. Investigate server stability under load
2. Add connection pooling configuration
3. Consider rate limiting in tests
4. Run load tests in isolation

---

### Category 6: MCP Session/Instructions Failures (8 FAILED tests)

**Affected Test Files:**
- `test_mcp_sessions.py` (5 tests)
- `test_mcp_instructions.py` (3 tests)

**Tests:**
- Session persistence, concurrency, isolation tests
- MCP instructions initialization tests

**Root Cause:**

These tests validate MCP session management and instruction handling. Failures may indicate:
1. Session state not persisting across requests
2. Instruction loading from file failing
3. Concurrent session handling issues

**Proposed Fix:**

Debug specific test failures after fixing server connectivity.

---

## Prioritized Fix Recommendations

### Priority 1: Fix Server Connectivity (High Impact)

**File:** `test/integration/conftest.py`

**Changes:**
1. Change `flapi_server` fixture to `scope="session"` or `scope="module"`
2. Add robust health check after server startup
3. Add detailed crash logging

**Expected Impact:** Fix ~40 tests (Category 2, 4)

### Priority 2: Fix Examples Server Isolation (High Impact)

**File:** `test/integration/conftest.py`

**Changes:**
1. Isolate DuckDB home directory for examples server
2. Consider disabling ERPL extension for tests
3. Add retry logic for server startup

**Expected Impact:** Fix ~75 tests (Category 1)

### Priority 3: Fix Missing Format Variables (Low Impact)

**File:** `test/integration/conftest.py`

**Changes:**
1. Add `base_url` fixture for config service tests
2. Verify Tavern test markers

**Expected Impact:** Fix ~5 tests (Category 3)

### Priority 4: Investigate Load Test Stability (Medium Impact)

**Files:** `test/integration/test_load_testing.py`, server code

**Changes:**
1. Profile server under concurrent load
2. Add connection pooling
3. Consider test isolation

**Expected Impact:** Fix ~8 tests (Category 5)

---

## Test Infrastructure Improvements

### Recommended Changes to conftest.py

```python
# 1. Use session-scoped server for most tests
@pytest.fixture(scope="session")
def flapi_server_session():
    """Session-scoped server for all non-isolated tests."""
    # ... current implementation
    pass

# 2. Keep function-scoped server for isolation tests
@pytest.fixture(scope="function")
def flapi_server_isolated():
    """Function-scoped server for tests needing isolation."""
    pass

# 3. Robust server health check
def wait_for_server_healthy(port, timeout=30):
    """Wait for server to be fully ready."""
    import requests
    start = time.time()
    while time.time() - start < timeout:
        try:
            # Check both REST and MCP endpoints
            r = requests.post(
                f"http://localhost:{port}/api/v1/_ping",
                timeout=2
            )
            if r.status_code == 200:
                return True
        except requests.exceptions.RequestException:
            pass
        time.sleep(0.5)
    return False

# 4. Isolated environment for examples server
@pytest.fixture(scope="module")
def examples_server():
    """Completely isolated examples server."""
    # Create isolated directories
    temp_base = tempfile.mkdtemp(prefix="flapi_examples_")
    env = os.environ.copy()
    env['DUCKDB_HOME'] = os.path.join(temp_base, 'duckdb')
    env['HOME'] = os.path.join(temp_base, 'home')
    os.makedirs(env['DUCKDB_HOME'])
    os.makedirs(env['HOME'])

    # Start with isolated environment
    process = subprocess.Popen(
        [...],
        env=env,
    )
```

---

## Summary

| Category | Tests | Root Cause | Fix Priority |
|----------|-------|------------|--------------|
| Examples Server Crash | 75 | ERPL extension / resource conflict | P2 |
| Server Connectivity | 30+ | Fixture scope / server crash | P1 |
| Missing Format Vars | 5+ | Fixture configuration | P3 |
| MCP Tools | 20 | Cascading from connectivity | P1 |
| Load Testing | 8 | Server stability | P4 |
| MCP Sessions | 8 | Various | P4 |

**Total estimated fixes needed:** 3-4 infrastructure changes to fix ~130 tests.
