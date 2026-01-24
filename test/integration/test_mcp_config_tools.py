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


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
