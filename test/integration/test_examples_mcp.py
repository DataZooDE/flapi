"""
Integration tests for MCP tools/resources defined in examples.

Tests verify that:
- MCP tools are properly discovered
- MCP tools can be called with valid arguments
- MCP resources are properly discovered

Note: Due to server stability issues with DuckLake cache operations,
these tests are limited to avoid triggering the cache refresh hang bug.

Reuses:
- examples_server fixture (from conftest.py)
- examples_url fixture (from conftest.py)
- wait_for_examples fixture (from conftest.py)
"""
import pytest
import requests
import json
from typing import Dict, Any, Tuple

class SimpleMCPClient:
    """Simple HTTP-based MCP client for testing examples.

    This client implements the MCP JSON-RPC protocol over HTTP.
    """

    def __init__(self, base_url: str):
        self.base_url = base_url
        self.session = requests.Session()
        self.session_id = None
        self.request_count = 0

    def _make_request(self, method: str, params: Dict[str, Any] = None) -> Tuple[Dict[str, Any], requests.Response]:
        """Make a JSON-RPC request to the MCP server."""
        payload = {
            "jsonrpc": "2.0",
            "id": str(self.request_count),
            "method": method,
            "params": params or {}
        }

        headers = {
            'Content-Type': 'application/json',
            'Accept': 'application/json'
        }

        if self.session_id:
            headers['Mcp-Session-Id'] = self.session_id

        self.request_count += 1

        try:
            response = self.session.post(
                f"{self.base_url}/mcp/jsonrpc",
                json=payload,
                headers=headers,
                timeout=10
            )

            # Extract session ID from response headers
            if 'Mcp-Session-Id' in response.headers:
                new_session_id = response.headers['Mcp-Session-Id']
                if not self.session_id:
                    self.session_id = new_session_id

            try:
                return response.json(), response
            except json.JSONDecodeError:
                return {}, response
        except requests.exceptions.RequestException as e:
            raise Exception(f"MCP request failed: {e}")

    def initialize(self, protocol_version: str = "2025-11-25") -> Dict[str, Any]:
        """Initialize MCP session."""
        response_data, _ = self._make_request("initialize", {
            "protocolVersion": protocol_version,
            "capabilities": {},
            "clientInfo": {
                "name": "examples-test-client",
                "version": "1.0"
            }
        })
        return response_data

    def list_tools(self) -> Tuple[Dict[str, Any], requests.Response]:
        """List available MCP tools."""
        return self._make_request("tools/list")

    def call_tool(self, name: str, arguments: Dict[str, Any] = None) -> Tuple[Dict[str, Any], requests.Response]:
        """Call an MCP tool."""
        return self._make_request("tools/call", {
            "name": name,
            "arguments": arguments or {}
        })

    def list_resources(self) -> Tuple[Dict[str, Any], requests.Response]:
        """List available MCP resources."""
        return self._make_request("resources/list")


class TestExamplesMCPTools:
    """Test MCP tools defined in examples.

    Note: Tests are limited to avoid triggering server hang issues
    related to DuckLake cache operations after ~5 requests.
    """

    @pytest.fixture(scope="class", autouse=True)
    def setup_class(self, examples_server, wait_for_examples):
        """Ensure examples server is ready before this test class.

        Uses class scope so we only wait once for the server, not before each test.
        This avoids timing issues with the server's background tasks.
        """
        pass

    @pytest.fixture
    def mcp_client(self, examples_url):
        """Create an MCP client for testing."""
        return SimpleMCPClient(examples_url)

    def test_mcp_initialize(self, mcp_client):
        """Verify MCP session can be initialized."""
        result = mcp_client.initialize()

        assert result is not None
        assert "error" not in result or result.get("error") is None
        assert mcp_client.session_id is not None

    def test_mcp_tools_list(self, mcp_client):
        """Verify MCP tools are discovered."""
        mcp_client.initialize()
        result, response = mcp_client.list_tools()

        assert response.status_code == 200
        assert "result" in result
        assert "tools" in result["result"]

        tools = result["result"]["tools"]
        tool_names = [t["name"] for t in tools]

        # Should have customer_lookup tool from examples
        assert "customer_lookup" in tool_names, f"Expected customer_lookup in {tool_names}"

    def test_mcp_tool_customer_lookup_exists(self, mcp_client):
        """Verify customer_lookup tool has correct schema."""
        mcp_client.initialize()
        result, _ = mcp_client.list_tools()

        tools = result["result"]["tools"]
        customer_tool = next((t for t in tools if t["name"] == "customer_lookup"), None)

        assert customer_tool is not None
        assert "description" in customer_tool
        assert "Retrieve customer information" in customer_tool["description"]

    def test_mcp_tool_call_customer_lookup(self, mcp_client):
        """Verify customer_lookup tool can be called."""
        mcp_client.initialize()
        result, response = mcp_client.call_tool(
            "customer_lookup",
            {"segment": "AUTOMOBILE"}
        )

        assert response.status_code == 200
        assert "result" in result
        # Should return customer data
        assert "content" in result["result"]


class TestExamplesMCPResources:
    """Test MCP resources defined in examples.

    Note: Tests are limited to basic discovery only to avoid server hang issues.
    """

    @pytest.fixture(scope="class", autouse=True)
    def setup_class(self, examples_server, wait_for_examples):
        """Ensure examples server is ready before this test class."""
        pass

    @pytest.fixture
    def mcp_client(self, examples_url):
        """Create an MCP client for testing."""
        return SimpleMCPClient(examples_url)

    def test_mcp_resources_list(self, mcp_client):
        """Verify MCP resources are discovered."""
        mcp_client.initialize()
        result, response = mcp_client.list_resources()

        assert response.status_code == 200
        assert "result" in result
        assert "resources" in result["result"]

        resources = result["result"]["resources"]
        resource_names = [r["name"] for r in resources]

        # Should have customer_schema resource from examples
        assert "customer_schema" in resource_names, f"Expected customer_schema in {resource_names}"
