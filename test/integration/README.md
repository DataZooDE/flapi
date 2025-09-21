# FLAPI Integration Tests

This directory contains integration tests for the FLAPI service using Tavern, a pytest-based API testing framework.

## Setup

1. Create and activate a Python virtual environment:
```bash
uv venv
source .venv/bin/activate
```

2. Install dependencies:
```bash
uv pip install -e .
```

## Running Tests

Run all tests:
```bash
uv run python -m pytest *.tavern.yaml -v
```

Run a specific test file:
```bash
uv run python -m pytest test_customers.tavern.yaml -v
```

## Test Structure

The test suite consists of several components:

### Configuration Files
- `pyproject.toml`: Python project configuration, dependencies, and pytest settings
- `api_configuration/`: Contains FLAPI service configuration files
  - `flapi.yaml`: Main service configuration
  - `sqls/`: SQL templates and endpoint configurations

### Test Files

#### REST API Tests (Tavern-based)
- `test_customers.tavern.yaml`: Tests for the customers endpoint
- `test_customers_cached.tavern.yaml`: Tests for cached customers endpoint
- `test_data_types.tavern.yaml`: Tests for data type handling
- `test_request_validation.tavern.yaml`: Tests for request field validation

#### MCP Tests (pytest-based)
- `test_mcp_client.py`: Basic MCP client integration tests
- `test_mcp_comprehensive.py`: Comprehensive MCP functionality tests

### Test Infrastructure
- `conftest.py`: Contains pytest fixtures and helper functions:
  - `flapi_server`: Starts/stops the FLAPI binary for testing
  - `auth_headers`: Provides authentication headers
  - `wait_for_api`: Ensures API is ready before testing
  - `verify_data_types`: Helper for validating response data types

### MCP Test Infrastructure
- `FLAPIMCPClient`: MCP client for testing FLAPI MCP server functionality
- `FLAPIMCPTester`: Comprehensive MCP testing client
- `flapi_mcp_client`: pytest fixture for MCP client testing
- `flapi_mcp_tester`: pytest fixture for comprehensive MCP testing

## How It Works

### REST API Testing

1. When tests start, the `flapi_server` fixture:
   - Locates the FLAPI binary
   - Starts it with the test configuration
   - Waits for the service to be ready

2. Each test file (*.tavern.yaml) defines:
   - Test scenarios with descriptive names
   - Request specifications (URL, method, parameters)
   - Expected responses (status codes, JSON structure)

3. Tavern executes each test by:
   - Making HTTP requests to the FLAPI service
   - Validating responses against expected values
   - Handling authentication and data type verification

### MCP Testing

1. When MCP tests start, the `flapi_mcp_client` or `flapi_mcp_tester` fixture:
   - Connects to the FLAPI MCP server using Streamable HTTP transport
   - Initializes the MCP session
   - Discovers available tools and resources

2. Each MCP test:
   - Tests MCP protocol compliance
   - Validates tool discovery and execution
   - Ensures proper JSON-RPC 2.0 formatting
   - Tests error handling and edge cases

3. The MCP client tests:
   - Use the standard Python MCP client library
   - Support both basic and comprehensive testing scenarios
   - Include performance and error handling tests

## MCP Testing

### Running MCP Tests

Run MCP-specific tests:
```bash
# Run all MCP tests
uv run python -m pytest test_mcp_*.py -v

# Run basic MCP client tests
uv run python -m pytest test_mcp_client.py -v

# Run comprehensive MCP tests
uv run python -m pytest test_mcp_comprehensive.py -v
```

### MCP Test Configuration

The MCP tests use the following configuration:
- **Transport**: Streamable HTTP
- **URL**: `http://localhost:8080/mcp/jsonrpc`
- **Protocol**: JSON-RPC 2.0
- **Content-Type**: `application/json`

### MCP Test Features

#### Basic MCP Client Tests (`test_mcp_client.py`)
- Connection establishment
- Tool discovery
- Tool execution
- Error handling
- Parameter validation

#### Comprehensive MCP Tests (`test_mcp_comprehensive.py`)
- Performance testing
- Multiple tool calls
- Tool schema validation
- Resource discovery
- Error handling edge cases
- Concurrent operations

### MCP Test Infrastructure

#### FLAPIMCPClient
Basic MCP client for simple testing scenarios:
```python
client = FLAPIMCPClient("http://localhost:8080")
await client.connect()
tools = await client.list_tools()
result = await client.call_tool("get_customers", {"id": "1"})
```

#### FLAPIMCPTester
Advanced MCP client for comprehensive testing:
```python
tester = FLAPIMCPTester("http://localhost:8080")
await tester.connect()
tools = await tester.list_tools()
tool = tester.get_tool_by_name("get_customers")
```

### Manual MCP Testing

You can also test MCP endpoints manually:

```bash
# Initialize
curl -s http://localhost:8080/mcp/jsonrpc -X POST -H "Content-Type: application/json" -d '{"jsonrpc":"2.0","id":"1","method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{"tools":{},"resources":{},"prompts":{},"sampling":{}}}}'

# List Tools
curl -s http://localhost:8080/mcp/jsonrpc -X POST -H "Content-Type: application/json" -d '{"jsonrpc":"2.0","id":"1","method":"tools/list","params":{}}'

# Call Tool
curl -s http://localhost:8080/mcp/jsonrpc -X POST -H "Content-Type: application/json" -d '{"jsonrpc":"2.0","id":"1","method":"tools/call","params":{"name":"get_customers","arguments":{"id":"1"}}}'
```

## Adding New Tests

### REST API Tests

1. Create a new `test_*.tavern.yaml` file
2. Define test scenarios following this structure:

### MCP Tests

#### Adding New MCP Test Files

1. Create a new `test_mcp_*.py` file
2. Use the provided fixtures:
```python
@pytest.mark.asyncio
async def test_my_mcp_feature(flapi_mcp_client):
    """Test specific MCP functionality"""
    tools = await flapi_mcp_client.list_tools()
    # Your test logic here
```

#### Using MCP Test Fixtures

- `flapi_mcp_client`: Basic MCP client for simple tests
- `flapi_mcp_tester`: Advanced MCP client for comprehensive tests

#### Example MCP Test

```python
@pytest.mark.asyncio
async def test_mcp_tool_execution(flapi_mcp_client):
    """Test MCP tool execution with parameters"""
    result = await flapi_mcp_client.call_tool("get_customers", {
        "id": "1",
        "segment": "retail"
    })

    assert result is not None
    assert len(result) > 0
```

### REST API Tests

1. Create a new `test_*.tavern.yaml` file
2. Define test scenarios following this structure:
```yaml
test_name: Description of test group

stages:
  - name: Description of test case
    request:
      url: http://localhost:8080/endpoint/
      method: GET
      auth:
        - admin
        - secret
    response:
      status_code: 200
      json:
        data: !anything
        total_count: !anything
```

## Common Patterns

- Use `!anything` to match any value
- Use auth credentials `admin:secret` for protected endpoints
- Check response headers with `headers` block
- Validate specific data types using the `verify_data_types` helper

## Debugging

- Use `-v` flag for verbose output
- Check FLAPI logs (debug level enabled in test config)
- Examine `flapi_cache.db` for database state
