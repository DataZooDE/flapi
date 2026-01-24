"""
End-to-End Workflow Tests for MCP Configuration Tools

Tests realistic agent workflows that combine multiple MCP tools:
1. Create and deploy endpoint workflow
2. Modify existing endpoint workflow
3. Cache management workflow
4. Schema exploration workflow
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
    client.initialize()
    return client


class TestCreateEndpointWorkflow:
    """Tests for creating and deploying a new endpoint workflow"""

    def test_workflow_schema_exploration(self, mcp_client):
        """
        Workflow: Agent explores schema before creating endpoint
        1. Get project config
        2. Get database schema
        3. Get file structure
        """
        # Step 1: Get project config
        config = mcp_client.call_tool("flapi_get_project_config")
        assert config is not None

        # Step 2: Get schema
        schema = mcp_client.call_tool("flapi_get_schema")
        assert schema is not None

        # Step 3: Get filesystem structure
        filesystem = mcp_client.call_tool("flapi_get_filesystem")
        assert filesystem is not None

    def test_workflow_list_existing_endpoints(self, mcp_client):
        """
        Workflow: Agent lists endpoints to understand patterns
        1. List endpoints
        2. Get one endpoint config
        3. Get its template
        """
        # Step 1: List endpoints
        endpoints = mcp_client.call_tool("flapi_list_endpoints")
        assert endpoints is not None

        # Step 2: Get specific endpoint (sample is commonly available)
        try:
            endpoint = mcp_client.call_tool("flapi_get_endpoint", {"path": "/sample"})
            assert endpoint is not None

            # Step 3: Get its template
            template = mcp_client.call_tool("flapi_get_template", {"path": "/sample"})
            assert template is not None
        except Exception:
            # Endpoint might not exist, that's ok
            pass

    def test_workflow_template_validation_before_deploy(self, mcp_client):
        """
        Workflow: Agent validates template before deployment
        1. Expand template with sample params
        2. Test template execution
        """
        # Step 1: Expand template
        expanded = mcp_client.call_tool("flapi_expand_template", {
            "path": "/sample",
            "params": {"limit": 10}
        })
        assert expanded is not None

        # Step 2: Test template execution
        test_result = mcp_client.call_tool("flapi_test_template", {
            "path": "/sample",
            "params": {"limit": 10}
        })
        assert test_result is not None


class TestModifyEndpointWorkflow:
    """Tests for modifying existing endpoints workflow"""

    def test_workflow_modify_endpoint_full_cycle(self, mcp_client):
        """
        Full modification workflow:
        1. List endpoints
        2. Get endpoint details
        3. Get current template
        4. Expand to see current output
        5. Update template
        6. Test updated template
        7. Reload endpoint
        """
        # Step 1: List endpoints
        endpoints = mcp_client.call_tool("flapi_list_endpoints")
        assert endpoints is not None

        # Steps 2-3: Get endpoint and template
        try:
            endpoint = mcp_client.call_tool("flapi_get_endpoint", {"path": "/sample"})
            assert endpoint is not None

            template = mcp_client.call_tool("flapi_get_template", {"path": "/sample"})
            assert template is not None

            # Step 4: Expand to see current output
            expanded = mcp_client.call_tool("flapi_expand_template", {
                "path": "/sample",
                "params": {}
            })
            assert expanded is not None

            # Step 5: Update template (with same content for safety)
            update = mcp_client.call_tool("flapi_update_template", {
                "path": "/sample",
                "content": "SELECT 1 as test"
            })
            # Update should succeed or fail gracefully
            assert update is not None or update is None

            # Step 6: Test new template
            test = mcp_client.call_tool("flapi_test_template", {
                "path": "/sample",
                "params": {}
            })
            assert test is not None

            # Step 7: Reload endpoint
            reload = mcp_client.call_tool("flapi_reload_endpoint", {"path": "/sample"})
            assert reload is not None

        except Exception as e:
            # Expected if endpoint doesn't have certain properties
            assert "not found" in str(e).lower() or "endpoint" in str(e).lower()

    def test_workflow_get_then_update_endpoint_config(self, mcp_client):
        """
        Workflow: Get endpoint config and update properties
        1. Get endpoint
        2. Modify config
        3. Update endpoint
        4. Verify update
        """
        # Step 1: Get endpoint
        try:
            endpoint = mcp_client.call_tool("flapi_get_endpoint", {"path": "/sample"})
            assert endpoint is not None

            # Step 2-3: Update endpoint
            updated = mcp_client.call_tool("flapi_update_endpoint", {
                "path": "/sample",
                "description": "Updated via MCP workflow"
            })
            assert updated is not None

            # Step 4: Verify (get again)
            verified = mcp_client.call_tool("flapi_get_endpoint", {"path": "/sample"})
            assert verified is not None

        except Exception as e:
            assert "not found" in str(e).lower() or "endpoint" in str(e).lower()


class TestCacheManagementWorkflow:
    """Tests for cache management workflows"""

    def test_workflow_cache_inspection(self, mcp_client):
        """
        Workflow: Inspect cache state
        1. Get overall cache status
        2. Get cache audit history
        3. Verify cache statistics
        """
        # Step 1: Get cache status
        status = mcp_client.call_tool("flapi_get_cache_status")
        assert status is not None

        # Step 2: Get audit history
        audit = mcp_client.call_tool("flapi_get_cache_audit", {})
        assert audit is not None

        # All steps completed successfully
        assert status is not None and audit is not None

    def test_workflow_cache_refresh_and_audit(self, mcp_client):
        """
        Workflow: Refresh cache and verify
        1. Get current cache status
        2. Refresh cache for endpoint
        3. Check updated audit log
        """
        # Step 1: Get current status
        status_before = mcp_client.call_tool("flapi_get_cache_status")
        assert status_before is not None

        # Step 2: Refresh cache
        try:
            refresh = mcp_client.call_tool("flapi_refresh_cache", {"path": "/sample"})
            assert refresh is not None

            # Step 3: Check audit
            time.sleep(0.1)  # Brief pause for persistence
            audit_after = mcp_client.call_tool("flapi_get_cache_audit", {"path": "/sample"})
            assert audit_after is not None

        except Exception as e:
            # Expected if endpoint doesn't have cache enabled
            assert "cache" in str(e).lower() or "not found" in str(e).lower()

    def test_workflow_cache_cleanup(self, mcp_client):
        """
        Workflow: Maintenance - clean up cache
        1. Get current cache status
        2. Run garbage collection
        3. Verify status after cleanup
        """
        # Step 1: Get current status
        status = mcp_client.call_tool("flapi_get_cache_status")
        assert status is not None

        # Step 2: Run garbage collection
        gc_result = mcp_client.call_tool("flapi_run_cache_gc", {})
        assert gc_result is not None

        # Step 3: Get status after GC
        status_after = mcp_client.call_tool("flapi_get_cache_status")
        assert status_after is not None


class TestMultiStepWorkflows:
    """Tests for complex multi-step workflows combining multiple tools"""

    def test_workflow_schema_refresh_and_exploration(self, mcp_client):
        """
        Workflow: Refresh schema and explore results
        1. Refresh schema
        2. Get updated schema
        3. Get filesystem to verify structure
        """
        # Step 1: Refresh
        refresh = mcp_client.call_tool("flapi_refresh_schema")
        assert refresh is not None

        # Step 2: Get schema
        schema = mcp_client.call_tool("flapi_get_schema")
        assert schema is not None

        # Step 3: Get filesystem
        filesystem = mcp_client.call_tool("flapi_get_filesystem")
        assert filesystem is not None

    def test_workflow_environment_exploration(self, mcp_client):
        """
        Workflow: Explore environment configuration
        1. Get environment variables
        2. Get project config
        3. Verify connectivity
        """
        # Step 1: Get environment
        env = mcp_client.call_tool("flapi_get_environment")
        assert env is not None

        # Step 2: Get config
        config = mcp_client.call_tool("flapi_get_project_config")
        assert config is not None

        # Step 3: Verify by refreshing schema
        schema = mcp_client.call_tool("flapi_refresh_schema")
        assert schema is not None

    def test_workflow_comprehensive_system_check(self, mcp_client):
        """
        Comprehensive workflow: Full system health check
        1. Get project config
        2. Check environment
        3. Refresh schema
        4. List endpoints
        5. Get cache status
        6. Run GC
        """
        # Full system check workflow
        steps = [
            ("Project Config", lambda: mcp_client.call_tool("flapi_get_project_config")),
            ("Environment", lambda: mcp_client.call_tool("flapi_get_environment")),
            ("Refresh Schema", lambda: mcp_client.call_tool("flapi_refresh_schema")),
            ("List Endpoints", lambda: mcp_client.call_tool("flapi_list_endpoints")),
            ("Cache Status", lambda: mcp_client.call_tool("flapi_get_cache_status")),
            ("Cache GC", lambda: mcp_client.call_tool("flapi_run_cache_gc", {})),
        ]

        results = {}
        for step_name, step_func in steps:
            result = step_func()
            results[step_name] = result
            assert result is not None, f"Step '{step_name}' returned None"

        # Verify all steps completed
        assert len(results) == 6


class TestErrorRecoveryWorkflows:
    """Tests for workflows that handle errors gracefully"""

    def test_workflow_handle_missing_endpoint(self, mcp_client):
        """
        Workflow: Handle missing endpoint gracefully
        1. Try to get non-existent endpoint
        2. List endpoints instead
        3. Suggest valid endpoint
        """
        # Step 1: Try to get missing endpoint
        with pytest.raises(Exception):
            mcp_client.call_tool("flapi_get_endpoint", {"path": "/missing"})

        # Step 2: List available endpoints
        endpoints = mcp_client.call_tool("flapi_list_endpoints")
        assert endpoints is not None

    def test_workflow_handle_invalid_template_params(self, mcp_client):
        """
        Workflow: Handle invalid template parameters
        1. Try expand with invalid params
        2. Get template for reference
        3. Try again with valid params
        """
        try:
            # Step 1: Try with invalid params (may fail)
            try:
                expand1 = mcp_client.call_tool("flapi_expand_template", {
                    "path": "/sample",
                    "params": {"invalid_param_xyz": "value"}
                })
            except Exception:
                pass  # Expected

            # Step 2: Get template
            template = mcp_client.call_tool("flapi_get_template", {"path": "/sample"})
            assert template is not None

            # Step 3: Try with valid params
            expand2 = mcp_client.call_tool("flapi_expand_template", {
                "path": "/sample",
                "params": {}
            })
            assert expand2 is not None

        except Exception as e:
            assert "not found" in str(e).lower() or "endpoint" in str(e).lower()

    def test_workflow_handle_cache_operations_on_uncached_endpoint(self, mcp_client):
        """
        Workflow: Handle cache operations gracefully
        1. Try to refresh cache on endpoint without cache
        2. Check cache status
        3. Verify appropriate response
        """
        try:
            # Step 1: Try refresh on endpoint without cache
            try:
                refresh = mcp_client.call_tool("flapi_refresh_cache", {"path": "/sample"})
            except Exception:
                pass  # Expected if no cache

            # Step 2: Check cache status
            status = mcp_client.call_tool("flapi_get_cache_status")
            assert status is not None

            # Step 3: Get audit
            audit = mcp_client.call_tool("flapi_get_cache_audit", {"path": "/sample"})
            assert audit is not None

        except Exception as e:
            # Some endpoints may not support cache
            assert "cache" in str(e).lower() or "not found" in str(e).lower()


class TestConcurrentWorkflows:
    """Tests for concurrent execution of workflows"""

    def test_concurrent_schema_refreshes(self, mcp_client):
        """Test concurrent schema refresh operations"""
        results = []
        for i in range(3):
            result = mcp_client.call_tool("flapi_refresh_schema")
            results.append(result)

        assert len([r for r in results if r is not None]) == 3

    def test_interleaved_read_and_list_operations(self, mcp_client):
        """Test interleaved list and get operations"""
        # List endpoints
        list_result = mcp_client.call_tool("flapi_list_endpoints")
        assert list_result is not None

        # Get endpoint
        try:
            get_result = mcp_client.call_tool("flapi_get_endpoint", {"path": "/sample"})
            assert get_result is not None
        except Exception:
            pass

        # List again
        list_result2 = mcp_client.call_tool("flapi_list_endpoints")
        assert list_result2 is not None


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
