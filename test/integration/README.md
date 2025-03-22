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
- `test_customers.tavern.yaml`: Tests for the customers endpoint
- `test_data_types.tavern.yaml`: Tests for data type handling

### Test Infrastructure
- `conftest.py`: Contains pytest fixtures and helper functions:
  - `flapi_server`: Starts/stops the FLAPI binary for testing
  - `auth_headers`: Provides authentication headers
  - `wait_for_api`: Ensures API is ready before testing
  - `verify_data_types`: Helper for validating response data types

## How It Works

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

## Adding New Tests

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
