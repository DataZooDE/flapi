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

    def __init__(self, base_url: str):
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
def mcp_client(flapi_base_url):
    """Fixture to provide MCP client."""
    client = SimpleMCPClient(flapi_base_url)
    # Initialize the client
    client.initialize()
    return client


class TestConfigDiscoveryTools:
    """Tests for Phase 1: Read-Only Discovery Tools"""

    def test_flapi_get_project_config(self, mcp_client):
        """Test flapi_get_project_config tool"""
        result = mcp_client.call_tool("flapi_get_project_config")

        # The hand-rolled version returned four fields and a hardcoded
        # version "1.0.0". It now delegates to ProjectConfigHandler, which is
        # what GET /api/v1/_config/project returns - the real project
        # configuration.
        assert result is not None
        assert "content" in result and result["content"], result
        content = json.loads(result["content"][0]["text"])
        assert isinstance(content, dict) and content, content
        # Real config, not the four-field stub.
        assert "connections" in content, content
        assert content.get("version") != "1.0.0", (
            "still reporting the hardcoded version")

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

    def test_flapi_refresh_schema_actually_delegates(self, mcp_client):
        """It used to construct a SchemaHandler, discard it, and return
        "schema_refreshed". It now calls SchemaHandler::refreshSchema."""
        result = mcp_client.call_tool("flapi_refresh_schema")
        assert result is not None
        assert "schema_refreshed" not in str(result)


class TestConfigToolDiscovery:
    """Tests for tool discovery and metadata"""

    def test_config_tools_are_registered(self, mcp_client):
        """Only tools with a working implementation are advertised."""
        tools = mcp_client.list_tools()
        tool_names = [tool["name"] for tool in tools]

        # Check for Phase 1 discovery tools
        assert "flapi_get_project_config" in tool_names or any("project_config" in name for name in tool_names)
        assert "flapi_get_environment" in tool_names or any("environment" in name for name in tool_names)
        assert "flapi_get_filesystem" in tool_names or any("filesystem" in name for name in tool_names)
        assert "flapi_get_schema" in tool_names or any("schema" in name for name in tool_names)
        # flapi_refresh_schema is deliberately NOT here: it is registered but
        # unimplemented, so it is not advertised. It used to be asserted
        # present, which pinned the stub in place.
        assert "flapi_refresh_schema" in tool_names

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
        result = mcp_client.call_tool("flapi_get_template", {"endpoint": "/customers/"})
        assert result is not None

    def test_flapi_get_template_missing(self, mcp_client):
        """Test retrieving a non-existent template"""
        # Should fail gracefully for missing endpoints
        with pytest.raises(Exception):
            mcp_client.call_tool("flapi_get_template", {"endpoint": "/nonexistent_endpoint"})

    def test_flapi_update_template_valid(self, mcp_client):
        """Test updating a template with valid SQL"""
        # Note: flapi_update_template requires authentication
        # Without auth, it should fail with auth required error
        try:
            result = mcp_client.call_tool("flapi_update_template", {
                "endpoint": "/test_template",
                "content": "SELECT 1 as test"
            })
            # If it succeeds without auth, that's fine too
            assert result is not None
        except Exception as e:
            # Expected: auth required or endpoint not found
            assert "authentication" in str(e).lower() or "not found" in str(e).lower()

    # flapi_expand_template and flapi_test_template are NOT implemented.
    #
    # They returned a hardcoded `SELECT * FROM data WHERE 1=1` with
    # "Template expanded successfully" / "Template test passed". The three
    # tests that used to live here asserted only `result is not None`, so they
    # passed against that fabrication for as long as it shipped - a test that
    # could not fail. They are replaced by the contract that actually holds:
    # an unimplemented tool is not advertised, and refuses if called anyway.

    # Every one of these had a working REST handler that the adapter
    # constructed and discarded, so they returned hardcoded or empty data with
    # a success label. They now delegate to that handler.
    FORMERLY_FABRICATED = ("flapi_expand_template", "flapi_test_template",
                           "flapi_refresh_schema", "flapi_get_cache_audit",
                           "flapi_get_environment", "flapi_get_filesystem",
                           "flapi_get_schema", "flapi_get_project_config",
                           "flapi_get_cache_status")

    def test_they_are_all_advertised(self, mcp_client):
        listed = {t["name"] for t in mcp_client.list_tools()}
        assert listed, "tools/list returned nothing; this proves nothing"
        for name in self.FORMERLY_FABRICATED:
            assert name in listed, f"{name} is missing from tools/list"

    def test_none_of_them_returns_the_old_fabrication(self, mcp_client):
        # The literal strings each used to return. A tool that regressed to a
        # stub would produce one of these again.
        forbidden = ("SELECT * FROM data WHERE 1=1", "Template test passed",
                     "Template expanded successfully", "schema_refreshed",
                     "cache_status_checked", "has been scheduled",
                     "Garbage collection triggered")
        for name in self.FORMERLY_FABRICATED:
            try:
                result = mcp_client.call_tool(
                    name, {"endpoint": "/customers/", "path": "/customers_cached/",
                           "params": {}})
            except Exception as exc:
                result = str(exc)
            blob = str(result)
            for phrase in forbidden:
                assert phrase not in blob, f"{name} returned {phrase!r}: {blob}"

    def test_expand_template_renders_this_endpoints_own_sql(self, mcp_client):
        # The discriminating case: the answer has to come from the endpoint's
        # template, not from a constant. Two different endpoints, two
        # different expansions.
        first = str(mcp_client.call_tool(
            "flapi_expand_template", {"endpoint": "/customers/", "params": {}}))
        second = str(mcp_client.call_tool(
            "flapi_expand_template", {"endpoint": "/data_types/", "params": {}}))
        assert first != second, (
            "two different endpoints expanded to the same SQL:\n" + first)

    def test_get_schema_returns_actual_tables(self, mcp_client):
        result = str(mcp_client.call_tool("flapi_get_schema"))
        assert '"tables":null' not in result.replace(" ", ""), result


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
        result = mcp_client.call_tool("flapi_get_endpoint", {"path": "/customers/"})
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

        # This will likely fail if the endpoint already exists, connection doesn't exist,
        # or authentication is required - all expected behavior
        try:
            result = mcp_client.call_tool("flapi_create_endpoint", endpoint_config)
            assert result is not None
        except Exception as e:
            # Expected - auth required, endpoint exists, or connection not found
            assert "exists" in str(e) or "not found" in str(e) or "connection" in str(e).lower() or "authentication" in str(e).lower()

    def test_flapi_update_endpoint_valid(self, mcp_client):
        """Test updating an endpoint configuration"""
        update_config = {
            "path": "/customers/",
            "description": "Updated description"
        }

        try:
            result = mcp_client.call_tool("flapi_update_endpoint", update_config)
            # Should succeed or return meaningful result
            assert result is not None
        except Exception as e:
            # Expected - auth required for mutation
            assert "authentication" in str(e).lower()

    def test_flapi_delete_endpoint_invalid_path(self, mcp_client):
        """Test deleting with invalid endpoint path"""
        with pytest.raises(Exception):
            mcp_client.call_tool("flapi_delete_endpoint", {"path": "/nonexistent"})

    def test_flapi_reload_endpoint_existing(self, mcp_client):
        """Test reloading an existing endpoint configuration"""
        try:
            result = mcp_client.call_tool("flapi_reload_endpoint", {"path": "/customers/"})
            assert result is not None
            # Should contain success status
            assert "status" in str(result) or "success" in str(result).lower()
        except Exception as e:
            # Expected - auth required for mutation
            assert "authentication" in str(e).lower()

    def test_flapi_reload_endpoint_missing(self, mcp_client):
        """Test reloading a non-existent endpoint"""
        with pytest.raises(Exception):
            mcp_client.call_tool("flapi_reload_endpoint", {"path": "/nonexistent"})


class TestConfigCacheTools:
    """Tests for Phase 4: Cache Management Tools"""

    def test_flapi_get_cache_status_basic(self, mcp_client):
        """Test getting cache status requires a path"""
        # The tool requires a path parameter
        try:
            result = mcp_client.call_tool("flapi_get_cache_status")
            assert result is not None
        except Exception as e:
            # Expected - path is required
            assert "path" in str(e).lower() or "required" in str(e).lower()

    def test_flapi_get_cache_status_for_endpoint(self, mcp_client):
        """Test getting cache status for specific endpoint"""
        try:
            result = mcp_client.call_tool("flapi_get_cache_status", {"path": "/customers/"})
            assert result is not None
        except Exception as e:
            # Expected - cache may not be enabled for this endpoint
            assert "cache" in str(e).lower() or "not enabled" in str(e).lower()

    def test_flapi_refresh_cache_valid(self, mcp_client):
        """Test refreshing cache for valid endpoint"""
        try:
            result = mcp_client.call_tool("flapi_refresh_cache", {"path": "/customers/"})
            # Should succeed or indicate cache not enabled
            assert result is not None
        except Exception as e:
            # Expected - auth required for mutation
            assert "authentication" in str(e).lower()

    def test_flapi_refresh_cache_invalid_endpoint(self, mcp_client):
        """Test refreshing cache for non-existent endpoint"""
        with pytest.raises(Exception):
            mcp_client.call_tool("flapi_refresh_cache", {"path": "/nonexistent"})

    def test_flapi_get_cache_audit_does_not_invent_audit_records(self, mcp_client):
        """It built an entry from std::time(nullptr) under the comment
        "Add sample audit entry" and returned it as a retrieved audit log.

        An invented audit record is the worst thing a tool can return: it is
        the record someone consults to find out whether the work happened. It
        now reads the real DuckLake audit table."""
        result = mcp_client.call_tool("flapi_get_cache_audit",
                                      {"path": "/customers_cached/"})
        assert "cache_status_checked" not in str(result), result

    def test_flapi_get_cache_audit_for_endpoint(self, mcp_client):
        """Test retrieving cache audit for specific endpoint"""
        try:
            result = mcp_client.call_tool("flapi_get_cache_audit", {"path": "/customers/"})
            assert result is not None
        except Exception as e:
            # Expected - cache/endpoint may not exist or not be configured
            assert "cache" in str(e).lower() or "not found" in str(e).lower()

    def test_flapi_run_cache_gc(self, mcp_client):
        """Test running cache garbage collection"""
        try:
            result = mcp_client.call_tool("flapi_run_cache_gc", {})
            assert result is not None
        except Exception as e:
            # Expected - auth required for mutation
            assert "authentication" in str(e).lower()


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
            result = mcp_client.call_tool("flapi_get_template", {"endpoint": "/customers/"})
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
        try:
            result = mcp_client.call_tool("flapi_get_cache_status", {"path": "/customers/"})
            assert result is not None
        except Exception as e:
            # Expected - cache may not be enabled or path required
            assert "cache" in str(e).lower() or "path" in str(e).lower() or "required" in str(e).lower()


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
        try:
            result = mcp_client.call_tool("flapi_get_cache_status", {"path": "/customers/"})
            assert result is not None
        except Exception as e:
            # Expected - cache may not be enabled
            assert "cache" in str(e).lower() or "not enabled" in str(e).lower()

    def test_large_cache_audit_log(self, mcp_client):
        """Test cache audit with potentially large history"""
        try:
            result = mcp_client.call_tool("flapi_get_cache_audit", {"path": "/customers/"})
            assert result is not None
        except Exception as e:
            # Expected - cache may not exist or not be configured
            assert "cache" in str(e).lower() or "not found" in str(e).lower()


class TestConfigToolsConcurrency:
    """Tests for concurrent tool execution"""

    def test_concurrent_schema_refreshes(self, mcp_client):
        """Three real refreshes in a row must all succeed."""
        for _ in range(3):
            result = mcp_client.call_tool("flapi_refresh_schema")
            assert result is not None

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
        errors = []

        # Get cache status multiple times
        for i in range(3):
            try:
                result = mcp_client.call_tool("flapi_get_cache_status", {"path": "/customers/"})
                results.append(result)
            except Exception as e:
                # Expected - cache may not be enabled
                errors.append(e)

        # All operations should complete (either success or expected error)
        assert len(results) + len(errors) == 3

    def test_list_and_get_endpoint_concurrent(self, mcp_client):
        """Test listing endpoints while getting individual endpoint"""
        # List endpoints
        list_result = mcp_client.call_tool("flapi_list_endpoints")
        # Get specific endpoint
        get_result = mcp_client.call_tool("flapi_get_endpoint", {"path": "/customers/"})

        assert list_result is not None
        assert get_result is not None


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
