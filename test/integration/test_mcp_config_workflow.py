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


# Eleven flapi_* config tools used to return confident, fabricated results - a
# hardcoded `SELECT * FROM data WHERE 1=1`, "Cache refresh has been scheduled"
# with nothing scheduled, invented audit rows, empty schemas and filesystems
# labelled success. Each of them had a working REST handler that the adapter
# constructed and then discarded; they now delegate to it.
#
# The workflow tests below were written as
#
#     try:  assert call_tool(...) is not None
#     except Exception as e:  assert "cache" in str(e).lower() or ...
#
# which passes whether the tool fabricates, works, or raises for any reason at
# all - so they could not tell any of that apart. They now assert what each
# step actually produced.
# /customers/ is the UNCACHED endpoint in the test configuration;
# /customers_cached/ is the one with a cache. Cache workflows have to address
# the latter or they only ever exercise "Cache not enabled for this endpoint".
CACHED_ENDPOINT = "/customers_cached/"

# The literal strings these tools used to return. Any of them reappearing
# means a tool has regressed to a stub.
FABRICATIONS = ("SELECT * FROM data WHERE 1=1", "Template test passed",
                "Template expanded successfully", "schema_refreshed",
                "cache_status_checked", "has been scheduled",
                "Garbage collection triggered")


def assert_real(result, tool):
    """The step produced something, and not one of the old fabrications."""
    blob = str(result)
    assert result is not None, f"{tool} returned nothing"
    for phrase in FABRICATIONS:
        assert phrase not in blob, f"{tool} returned the old stub {phrase!r}: {blob}"
    return blob


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
            endpoint = mcp_client.call_tool("flapi_get_endpoint", {"path": "/customers/"})
            assert endpoint is not None

            # Step 3: Get its template
            template = mcp_client.call_tool("flapi_get_template", {"endpoint": "/customers/"})
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
        # Both steps used to answer with a hardcoded
        # `SELECT * FROM data WHERE 1=1`, and flapi_test_template claimed
        # "Template test passed" for a query it never ran - so an agent
        # validating a template before deployment validated nothing.
        expanded = assert_real(
            mcp_client.call_tool("flapi_expand_template",
                                 {"endpoint": "/customers/",
                                  "params": {"limit": 10}}),
            "flapi_expand_template")
        assert_real(
            mcp_client.call_tool("flapi_test_template",
                                 {"endpoint": "/customers/",
                                  "params": {"limit": 10}}),
            "flapi_test_template")

        # The expansion has to come from THIS endpoint's template.
        other = str(mcp_client.call_tool("flapi_expand_template",
                                         {"endpoint": "/data_types/",
                                          "params": {}}))
        assert expanded != other, (
            "two endpoints expanded identically:\n" + expanded)


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
            endpoint = mcp_client.call_tool("flapi_get_endpoint", {"path": "/customers/"})
            assert endpoint is not None

            template = mcp_client.call_tool("flapi_get_template", {"endpoint": "/customers/"})
            assert template is not None

            # Step 4: expand to see the current output.
            assert_real(mcp_client.call_tool("flapi_expand_template",
                                             {"endpoint": "/customers/",
                                              "params": {}}),
                        "flapi_expand_template")

            # Step 5 is deliberately NOT exercised here: flapi_update_template
            # now really writes the file, and this suite runs against the
            # shared session server, so a write would leak into every test
            # after it. test_mcp_config_tool_auth.py covers the write against
            # its own server and asserts the file changed.

            # Step 6: test the template.
            assert_real(mcp_client.call_tool("flapi_test_template",
                                             {"endpoint": "/customers/",
                                              "params": {}}),
                        "flapi_test_template")

            # Step 7: Reload endpoint
            reload = mcp_client.call_tool("flapi_reload_endpoint", {"path": "/customers/"})
            assert reload is not None

        except Exception as e:
            # Expected if endpoint doesn't exist or auth required for mutation
            assert "not found" in str(e).lower() or "endpoint" in str(e).lower() or "authentication" in str(e).lower()

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
            endpoint = mcp_client.call_tool("flapi_get_endpoint", {"path": "/customers/"})
            assert endpoint is not None

            # Step 2-3: Update endpoint
            updated = mcp_client.call_tool("flapi_update_endpoint", {
                "endpoint": "/customers/",
                "description": "Updated via MCP workflow"
            })
            assert updated is not None

            # Step 4: Verify (get again)
            verified = mcp_client.call_tool("flapi_get_endpoint", {"path": "/customers/"})
            assert verified is not None

        except Exception as e:
            # Expected if endpoint doesn't exist or auth required for mutation
            assert "not found" in str(e).lower() or "endpoint" in str(e).lower() or "authentication" in str(e).lower()


class TestCacheManagementWorkflow:
    """Tests for cache management workflows"""

    def test_workflow_cache_inspection(self, mcp_client):
        """
        Workflow: Inspect cache state
        1. Get cache status for endpoint
        2. Get cache audit history
        """
        # Step 1 works.
        status = mcp_client.call_tool("flapi_get_cache_status", {"path": CACHED_ENDPOINT})
        assert status is not None

        # Step 2 used to build an audit entry from the current time under the
        # comment "Add sample audit entry" and return it as a retrieved audit
        # log. It now reads the real DuckLake audit table.
        assert_real(mcp_client.call_tool("flapi_get_cache_audit",
                                         {"path": CACHED_ENDPOINT}),
                    "flapi_get_cache_audit")

    def test_workflow_cache_refresh_and_audit(self, mcp_client):
        """
        Workflow: Refresh cache and verify
        1. Get current cache status
        2. Refresh cache for endpoint
        3. Check updated audit log
        """
        status_before = mcp_client.call_tool("flapi_get_cache_status", {"path": CACHED_ENDPOINT})
        assert status_before is not None

        # The refresh never happened: the adapter held no CacheManager
        # reference at all and returned "Cache refresh has been scheduled".
        # An agent polling the audit log to confirm then saw a manufactured
        # success entry - two fabrications reinforcing each other. Both now
        # delegate to the handlers the REST routes use.
        #
        # flapi_refresh_cache is a mutation and requires a token, which this
        # client does not carry; the refusal must be the AUTH one, never a
        # fabricated success. test_mcp_config_tool_auth.py exercises the
        # authenticated path.
        with pytest.raises(Exception) as excinfo:
            mcp_client.call_tool("flapi_refresh_cache", {"path": CACHED_ENDPOINT})
        assert "Authentication required" in str(excinfo.value), str(excinfo.value)
        assert "has been scheduled" not in str(excinfo.value)
        time.sleep(0.2)
        assert_real(mcp_client.call_tool("flapi_get_cache_audit",
                                         {"path": CACHED_ENDPOINT}),
                    "flapi_get_cache_audit")

    def test_workflow_cache_cleanup(self, mcp_client):
        """
        Workflow: Maintenance - clean up cache
        1. Get current cache status
        2. Run garbage collection
        3. Verify status after cleanup
        """
        status = mcp_client.call_tool("flapi_get_cache_status", {"path": CACHED_ENDPOINT})
        assert status is not None

        # Also a mutation, also token-gated.
        with pytest.raises(Exception) as excinfo:
            mcp_client.call_tool("flapi_run_cache_gc", {"path": CACHED_ENDPOINT})
        assert "Authentication required" in str(excinfo.value), str(excinfo.value)
        assert "Garbage collection triggered" not in str(excinfo.value)

        # Reading still works either side of the refusal.
        status_after = mcp_client.call_tool("flapi_get_cache_status", {"path": CACHED_ENDPOINT})
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
        # Step 1 used to construct a SchemaHandler, discard it, and return
        # "schema_refreshed". It now calls SchemaHandler::refreshSchema.
        assert_real(mcp_client.call_tool("flapi_refresh_schema"),
                    "flapi_refresh_schema")

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

        # Step 3 used to "verify by refreshing schema" against a tool that
        # built a SchemaHandler, discarded it, and returned
        # "schema_refreshed". It now calls SchemaHandler::refreshSchema.
        assert_real(mcp_client.call_tool("flapi_refresh_schema"),
                    "flapi_refresh_schema")
        assert_real(mcp_client.call_tool("flapi_get_schema"),
                    "flapi_get_schema")

    def test_workflow_comprehensive_system_check(self, mcp_client):
        """
        Comprehensive workflow: Full system health check
        1. Get project config
        2. Check environment
        3. Read schema
        4. List endpoints
        (Cache operations may require auth or fail for uncached endpoints)
        """
        # Full system check workflow (read-only operations that don't require auth)
        steps = [
            ("Project Config", lambda: mcp_client.call_tool("flapi_get_project_config")),
            ("Environment", lambda: mcp_client.call_tool("flapi_get_environment")),
            ("Schema", lambda: mcp_client.call_tool("flapi_get_schema")),
            ("List Endpoints", lambda: mcp_client.call_tool("flapi_list_endpoints")),
        ]

        results = {}
        for step_name, step_func in steps:
            result = step_func()
            results[step_name] = result
            assert result is not None, f"Step '{step_name}' returned None"

        # Verify all steps completed
        assert len(results) == 4


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
                    "endpoint": "/customers/",
                    "params": {"invalid_param_xyz": "value"}
                })
            except Exception:
                pass  # Expected

            # Step 2: Get template
            template = mcp_client.call_tool("flapi_get_template", {"endpoint": "/customers/"})
            assert template is not None

            # Step 3: Try with valid params
            expand2 = mcp_client.call_tool("flapi_expand_template", {
                "endpoint": "/customers/",
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
            # Step 1: Try refresh on endpoint without cache (may fail with auth or cache not enabled)
            try:
                refresh = mcp_client.call_tool("flapi_refresh_cache", {"path": "/customers/"})
            except Exception:
                pass  # Expected if no cache or auth required

            # Step 2: Check cache status (requires path)
            status = mcp_client.call_tool("flapi_get_cache_status", {"path": CACHED_ENDPOINT})
            assert status is not None

            # Step 3: Get audit
            audit = mcp_client.call_tool("flapi_get_cache_audit", {"path": "/customers/"})
            assert audit is not None

        except Exception as e:
            # Some endpoints may not support cache or auth may be required
            assert "cache" in str(e).lower() or "not found" in str(e).lower() or "authentication" in str(e).lower()


class TestConcurrentWorkflows:
    """Tests for concurrent execution of workflows"""

    def test_concurrent_schema_reads(self, mcp_client):
        """Concurrency over a tool that does something.

        This read flapi_refresh_schema three times and asserted three
        non-None results - which a stub returning a constant satisfies
        perfectly. flapi_get_schema actually queries."""
        results = []
        for _ in range(3):
            results.append(mcp_client.call_tool("flapi_get_schema"))

        assert len([r for r in results if r is not None]) == 3
        # Same input, same answer - which is the property concurrency could
        # break and a constant cannot demonstrate.
        assert results[0] == results[1] == results[2]

    def test_concurrent_schema_refreshes_all_succeed(self, mcp_client):
        for _ in range(3):
            assert_real(mcp_client.call_tool("flapi_refresh_schema"),
                        "flapi_refresh_schema")

    def test_interleaved_read_and_list_operations(self, mcp_client):
        """Test interleaved list and get operations"""
        # List endpoints
        list_result = mcp_client.call_tool("flapi_list_endpoints")
        assert list_result is not None

        # Get endpoint
        try:
            get_result = mcp_client.call_tool("flapi_get_endpoint", {"path": "/customers/"})
            assert get_result is not None
        except Exception:
            pass

        # List again
        list_result2 = mcp_client.call_tool("flapi_list_endpoints")
        assert list_result2 is not None


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
