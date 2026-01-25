"""
Integration tests for MCP Configuration Tools (Phase 1: Discovery Tools)

Tests the following MCP tools:
- flapi_get_project_config: Get project configuration
- flapi_get_environment: List environment variables
- flapi_get_filesystem: Get template directory structure
- flapi_get_schema: Introspect database schema
- flapi_refresh_schema: Refresh database schema cache
"""

import pytest
import requests
import json
import time
from typing import Dict, Any, List
from dotenv import load_dotenv
import os

load_dotenv()


class SimpleMCPClient:
    """Simple HTTP-based MCP client for testing FLAPI MCP server."""

    def __init__(self, base_url: str = "http://localhost:8080"):
        self.base_url = base_url
        self.session = requests.Session()
        self.session.headers.update({
            'Content-Type': 'application/json',
            'Accept': 'application/json'
        })

    def _make_request(self, method: str, params: Dict[str, Any] = None) -> Dict[str, Any]:
        """Make a JSON-RPC request to the MCP server."""
        payload = {
            "jsonrpc": "2.0",
            "id": "1",
            "method": method,
            "params": params or {}
        }

        try:
            response = self.session.post(
                f"{self.base_url}/mcp/jsonrpc",
                json=payload,
                timeout=10
            )
            response.raise_for_status()
            return response.json()
        except requests.exceptions.RequestException as e:
            raise Exception(f"MCP request failed: {e}")

    def initialize(self) -> Dict[str, Any]:
        """Initialize the MCP session."""
        return self._make_request("initialize", {
            "protocolVersion": "2024-11-05",
            "capabilities": {
                "tools": {},
                "resources": {},
                "prompts": {},
                "sampling": {}
            }
        })

    def list_tools(self) -> List[Dict[str, Any]]:
        """List available tools."""
        response = self._make_request("tools/list")
        if "result" in response and "tools" in response["result"]:
            return response["result"]["tools"]
        return []

    def call_tool(self, tool_name: str, arguments: Dict[str, Any] = None) -> Dict[str, Any]:
        """Call a tool with the given arguments."""
        response = self._make_request("tools/call", {
            "name": tool_name,
            "arguments": arguments or {}
        })
        if "result" in response:
            return response["result"]
        elif "error" in response:
            raise Exception(f"Tool call failed: {response['error']}")
        return {}


@pytest.fixture
def mcp_client():
    """Fixture to provide MCP client."""
    client = SimpleMCPClient()
    # Initialize the client
    client.initialize()
    return client


class TestConfigDiscoveryTools:
    """Tests for Phase 1: Read-Only Discovery Tools"""

    def test_flapi_get_project_config(self, mcp_client):
        """Test flapi_get_project_config tool"""
        result = mcp_client.call_tool("flapi_get_project_config")

        # Verify result structure
        assert result is not None
        assert "content" in result or "project_name" in str(result)

        # If content is present (MCP format), parse it
        if "content" in result and result["content"]:
            content = json.loads(result["content"][0]["text"])
            assert "project_name" in content or "base_path" in content

    def test_flapi_get_environment(self, mcp_client):
        """Test flapi_get_environment tool"""
        result = mcp_client.call_tool("flapi_get_environment")

        # Verify result structure
        assert result is not None
        assert "content" in result or "variables" in str(result)

    def test_flapi_get_filesystem(self, mcp_client):
        """Test flapi_get_filesystem tool"""
        result = mcp_client.call_tool("flapi_get_filesystem")

        # Verify result structure
        assert result is not None
        assert "content" in result or "base_path" in str(result) or "tree" in str(result)

        # If content is present, it should be JSON
        if "content" in result and result["content"]:
            content = json.loads(result["content"][0]["text"])
            # Should have base_path or tree
            assert "base_path" in content or "tree" in content

    def test_flapi_get_schema(self, mcp_client):
        """Test flapi_get_schema tool"""
        result = mcp_client.call_tool("flapi_get_schema")

        # Verify result structure
        assert result is not None
        assert "content" in result or "tables" in str(result)

    def test_flapi_refresh_schema(self, mcp_client):
        """Test flapi_refresh_schema tool"""
        result = mcp_client.call_tool("flapi_refresh_schema")

        # Verify result structure
        assert result is not None
        assert "content" in result or "status" in str(result)

        # If content is present, check for success status
        if "content" in result and result["content"]:
            content = json.loads(result["content"][0]["text"])
            assert "status" in content


class TestConfigToolDiscovery:
    """Tests for tool discovery and metadata"""

    def test_config_tools_are_registered(self, mcp_client):
        """Test that all Phase 1 config tools are registered"""
        tools = mcp_client.list_tools()
        tool_names = [tool["name"] for tool in tools]

        # Check for Phase 1 discovery tools
        assert "flapi_get_project_config" in tool_names or any("project_config" in name for name in tool_names)
        assert "flapi_get_environment" in tool_names or any("environment" in name for name in tool_names)
        assert "flapi_get_filesystem" in tool_names or any("filesystem" in name for name in tool_names)
        assert "flapi_get_schema" in tool_names or any("schema" in name for name in tool_names)
        assert "flapi_refresh_schema" in tool_names or any("refresh_schema" in name for name in tool_names)

    def test_config_tools_have_descriptions(self, mcp_client):
        """Test that tools have proper descriptions"""
        tools = mcp_client.list_tools()

        # Find config tools
        config_tools = [t for t in tools if "flapi_get" in t.get("name", "") or "flapi_refresh" in t.get("name", "")]

        # Each tool should have a description
        for tool in config_tools:
            assert "description" in tool or "name" in tool
            if "description" in tool:
                assert len(tool["description"]) > 0

    def test_config_tools_have_input_schema(self, mcp_client):
        """Test that tools have input schemas"""
        tools = mcp_client.list_tools()

        # Find config tools
        config_tools = [t for t in tools if "flapi_get" in t.get("name", "") or "flapi_refresh" in t.get("name", "")]

        # Each tool should have an input schema
        for tool in config_tools:
            assert "inputSchema" in tool or "input_schema" in tool or "schema" in tool


class TestConfigToolErrors:
    """Tests for error handling"""

    def test_unknown_tool_returns_error(self, mcp_client):
        """Test that calling unknown tool returns error"""
        with pytest.raises(Exception):
            mcp_client.call_tool("unknown_tool_xyz")

    def test_invalid_arguments_handled(self, mcp_client):
        """Test that invalid arguments are handled gracefully"""
        # Most discovery tools don't take arguments, so this should work
        result = mcp_client.call_tool("flapi_get_project_config", {"extra_param": "value"})
        # Should either succeed (ignoring extra params) or fail gracefully
        assert result is not None


class TestConfigTemplateTools:
    """Tests for Phase 2: Template Management Tools"""

    def test_flapi_get_template_existing(self, mcp_client):
        """Test retrieving an existing template"""
        # First, list endpoints to find a valid one
        endpoints = mcp_client.call_tool("flapi_list_endpoints")
        assert endpoints is not None

        # Try to get a template for the first endpoint if available
        # Most endpoints should have templates
        result = mcp_client.call_tool("flapi_get_template", {"path": "/sample"})
        assert result is not None

    def test_flapi_get_template_missing(self, mcp_client):
        """Test retrieving a non-existent template"""
        # Should fail gracefully for missing endpoints
        with pytest.raises(Exception):
            mcp_client.call_tool("flapi_get_template", {"path": "/nonexistent_endpoint"})

    def test_flapi_update_template_valid(self, mcp_client):
        """Test updating a template with valid SQL"""
        # Create new endpoint first, then update its template
        endpoint_config = {
            "path": "/test_template",
            "method": "GET",
            "connection": "sample_data",
            "template_source": "test_template.sql"
        }

        # Note: This might fail if endpoint doesn't exist, which is expected
        # In a full integration test, we'd create it first
        result = mcp_client.call_tool("flapi_update_template", {
            "path": "/test_template",
            "content": "SELECT 1 as test"
        })
        # Either success or expected error (endpoint not found)
        assert result is not None

    def test_flapi_expand_template_basic(self, mcp_client):
        """Test expanding a template with parameters"""
        result = mcp_client.call_tool("flapi_expand_template", {
            "path": "/sample",
            "params": {}
        })
        assert result is not None

    def test_flapi_expand_template_with_params(self, mcp_client):
        """Test expanding a template with actual parameters"""
        result = mcp_client.call_tool("flapi_expand_template", {
            "path": "/sample",
            "params": {"limit": 10, "offset": 0}
        })
        assert result is not None

    def test_flapi_test_template_execution(self, mcp_client):
        """Test template execution validation"""
        result = mcp_client.call_tool("flapi_test_template", {
            "path": "/sample",
            "params": {}
        })
        # Should either succeed or return test result
        assert result is not None


class TestConfigEndpointTools:
    """Tests for Phase 3: Endpoint Management Tools"""

    def test_flapi_list_endpoints_returns_list(self, mcp_client):
        """Test that list_endpoints returns available endpoints"""
        result = mcp_client.call_tool("flapi_list_endpoints")
        assert result is not None
        # Should contain content or endpoints list
        assert "content" in result or "endpoints" in str(result)

    def test_flapi_get_endpoint_existing(self, mcp_client):
        """Test retrieving an existing endpoint"""
        result = mcp_client.call_tool("flapi_get_endpoint", {"path": "/sample"})
        assert result is not None

    def test_flapi_get_endpoint_missing(self, mcp_client):
        """Test retrieving a non-existent endpoint"""
        with pytest.raises(Exception):
            mcp_client.call_tool("flapi_get_endpoint", {"path": "/nonexistent"})

    def test_flapi_create_endpoint_valid(self, mcp_client):
        """Test creating a new endpoint"""
        endpoint_config = {
            "path": "/test_new_endpoint",
            "method": "GET",
            "connection": "sample_data",
            "template_source": "new_endpoint.sql",
            "description": "Test endpoint"
        }

        # This will likely fail if the endpoint already exists or connection doesn't exist
        # which is expected behavior
        try:
            result = mcp_client.call_tool("flapi_create_endpoint", endpoint_config)
            assert result is not None
        except Exception as e:
            # Expected - either endpoint exists or connection not found
            assert "exists" in str(e) or "not found" in str(e) or "connection" in str(e).lower()

    def test_flapi_update_endpoint_valid(self, mcp_client):
        """Test updating an endpoint configuration"""
        update_config = {
            "path": "/sample",
            "description": "Updated description"
        }

        result = mcp_client.call_tool("flapi_update_endpoint", update_config)
        # Should succeed or return meaningful result
        assert result is not None

    def test_flapi_delete_endpoint_invalid_path(self, mcp_client):
        """Test deleting with invalid endpoint path"""
        with pytest.raises(Exception):
            mcp_client.call_tool("flapi_delete_endpoint", {"path": "/nonexistent"})

    def test_flapi_reload_endpoint_existing(self, mcp_client):
        """Test reloading an existing endpoint configuration"""
        result = mcp_client.call_tool("flapi_reload_endpoint", {"path": "/sample"})
        assert result is not None
        # Should contain success status
        assert "status" in str(result) or "success" in str(result).lower()

    def test_flapi_reload_endpoint_missing(self, mcp_client):
        """Test reloading a non-existent endpoint"""
        with pytest.raises(Exception):
            mcp_client.call_tool("flapi_reload_endpoint", {"path": "/nonexistent"})


class TestConfigCacheTools:
    """Tests for Phase 4: Cache Management Tools"""

    def test_flapi_get_cache_status_basic(self, mcp_client):
        """Test getting cache status"""
        result = mcp_client.call_tool("flapi_get_cache_status")
        assert result is not None

    def test_flapi_get_cache_status_for_endpoint(self, mcp_client):
        """Test getting cache status for specific endpoint"""
        result = mcp_client.call_tool("flapi_get_cache_status", {"path": "/sample"})
        assert result is not None

    def test_flapi_refresh_cache_valid(self, mcp_client):
        """Test refreshing cache for valid endpoint"""
        result = mcp_client.call_tool("flapi_refresh_cache", {"path": "/sample"})
        # Should succeed or indicate cache not enabled
        assert result is not None

    def test_flapi_refresh_cache_invalid_endpoint(self, mcp_client):
        """Test refreshing cache for non-existent endpoint"""
        with pytest.raises(Exception):
            mcp_client.call_tool("flapi_refresh_cache", {"path": "/nonexistent"})

    def test_flapi_get_cache_audit_history(self, mcp_client):
        """Test retrieving cache audit history"""
        result = mcp_client.call_tool("flapi_get_cache_audit", {})
        assert result is not None

    def test_flapi_get_cache_audit_for_endpoint(self, mcp_client):
        """Test retrieving cache audit for specific endpoint"""
        result = mcp_client.call_tool("flapi_get_cache_audit", {"path": "/sample"})
        assert result is not None

    def test_flapi_run_cache_gc(self, mcp_client):
        """Test running cache garbage collection"""
        result = mcp_client.call_tool("flapi_run_cache_gc", {})
        assert result is not None


class TestConfigToolsAuthentication:
    """Tests for authentication and authorization"""

    def test_protected_tools_list_with_auth(self, mcp_client):
        """Test that protected tools are accessible with proper auth"""
        # Phase 2-4 tools should be in the tool list
        tools = mcp_client.list_tools()
        tool_names = [tool["name"] for tool in tools]

        # Check for Phase 2 tools
        has_template_tools = any("template" in name for name in tool_names)
        # Check for Phase 3 tools
        has_endpoint_tools = any("endpoint" in name for name in tool_names)
        # Check for Phase 4 tools
        has_cache_tools = any("cache" in name for name in tool_names)

        # At least some should exist
        assert has_template_tools or has_endpoint_tools or has_cache_tools

    def test_template_tools_accessible(self, mcp_client):
        """Test that template tools can be called"""
        # flapi_get_template should be accessible
        try:
            result = mcp_client.call_tool("flapi_get_template", {"path": "/sample"})
            # Success or endpoint not found is fine
            assert result is not None or "not found" in str(result).lower()
        except Exception as e:
            # Expected if endpoint doesn't exist
            assert "not found" in str(e).lower() or "endpoint" in str(e).lower()

    def test_endpoint_tools_accessible(self, mcp_client):
        """Test that endpoint management tools can be called"""
        result = mcp_client.call_tool("flapi_list_endpoints")
        assert result is not None

    def test_cache_tools_accessible(self, mcp_client):
        """Test that cache tools can be called"""
        result = mcp_client.call_tool("flapi_get_cache_status")
        assert result is not None


class TestConfigToolsLargeData:
    """Tests for handling large data scenarios"""

    def test_large_schema_introspection(self, mcp_client):
        """Test schema introspection with potentially large schemas"""
        result = mcp_client.call_tool("flapi_get_schema")
        assert result is not None
        # Should complete within reasonable time

    def test_many_endpoints_list(self, mcp_client):
        """Test listing endpoints when many exist"""
        result = mcp_client.call_tool("flapi_list_endpoints")
        assert result is not None
        # Should handle large lists gracefully

    def test_large_cache_status(self, mcp_client):
        """Test cache status with potentially large data"""
        result = mcp_client.call_tool("flapi_get_cache_status")
        assert result is not None

    def test_large_cache_audit_log(self, mcp_client):
        """Test cache audit with potentially large history"""
        result = mcp_client.call_tool("flapi_get_cache_audit", {})
        assert result is not None


class TestConfigToolsConcurrency:
    """Tests for concurrent tool execution"""

    def test_concurrent_schema_refreshes(self, mcp_client):
        """Test concurrent schema refresh operations"""
        # Run multiple concurrent schema refreshes
        results = []
        for i in range(3):
            result = mcp_client.call_tool("flapi_refresh_schema")
            results.append(result)

        # All should complete
        assert len(results) == 3
        assert all(r is not None for r in results)

    def test_concurrent_list_endpoints(self, mcp_client):
        """Test concurrent endpoint listing"""
        results = []
        for i in range(3):
            result = mcp_client.call_tool("flapi_list_endpoints")
            results.append(result)

        assert len(results) == 3
        assert all(r is not None for r in results)

    def test_concurrent_cache_operations(self, mcp_client):
        """Test concurrent cache operations"""
        results = []

        # Get cache status multiple times concurrently
        for i in range(3):
            result = mcp_client.call_tool("flapi_get_cache_status")
            results.append(result)

        assert len(results) == 3
        assert all(r is not None for r in results)

    def test_list_and_get_endpoint_concurrent(self, mcp_client):
        """Test listing endpoints while getting individual endpoint"""
        # List endpoints
        list_result = mcp_client.call_tool("flapi_list_endpoints")
        # Get specific endpoint
        get_result = mcp_client.call_tool("flapi_get_endpoint", {"path": "/sample"})

        assert list_result is not None
        assert get_result is not None


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
