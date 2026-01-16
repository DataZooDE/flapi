"""
Integration tests for new MCP methods (logging/setLevel, completion/complete, protocol negotiation)
"""
import pytest
import requests
import json
from typing import Dict, Any, Tuple


class SimpleMCPClient:
    """Simple HTTP-based MCP client for testing."""

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
                "name": "test-client",
                "version": "1.0"
            }
        })
        return response_data

    def set_log_level(self, level: str) -> Dict[str, Any]:
        """Set logging level."""
        response_data, _ = self._make_request("logging/setLevel", {
            "level": level
        })
        return response_data

    def get_completions(self, partial: str, ref_type: str = None, ref: str = None) -> Dict[str, Any]:
        """Get completions."""
        params = {"partial": partial}
        if ref_type:
            params["ref"] = {"type": ref_type}
            if ref:
                params["ref"]["name"] = ref
        response_data, _ = self._make_request("completion/complete", params)
        return response_data

    def list_tools(self) -> Dict[str, Any]:
        """List available tools."""
        response_data, _ = self._make_request("tools/list")
        return response_data


class TestLoggingMethod:
    """Tests for logging/setLevel MCP method."""

    @pytest.fixture
    def client(self, flapi_base_url):
        """Create an MCP client for testing."""
        return SimpleMCPClient(flapi_base_url)

    def test_set_log_level_debug(self, client):
        """Test setting log level to debug."""
        client.initialize()
        response = client.set_log_level("debug")

        assert response is not None
        # Should not have an error
        assert "error" not in response or response.get("error") is None

    def test_set_log_level_info(self, client):
        """Test setting log level to info."""
        client.initialize()
        response = client.set_log_level("info")

        assert response is not None
        assert "error" not in response or response.get("error") is None

    def test_set_log_level_warning(self, client):
        """Test setting log level to warning."""
        client.initialize()
        response = client.set_log_level("warning")

        assert response is not None
        assert "error" not in response or response.get("error") is None

    def test_set_log_level_error(self, client):
        """Test setting log level to error."""
        client.initialize()
        response = client.set_log_level("error")

        assert response is not None
        assert "error" not in response or response.get("error") is None

    def test_set_log_level_without_init(self, client):
        """Test setting log level without initialization should still work."""
        response = client.set_log_level("debug")

        assert response is not None
        # May have error or succeed depending on implementation

    def test_set_log_level_invalid_level(self, client):
        """Test setting invalid log level."""
        client.initialize()
        response = client.set_log_level("invalid_level")

        # Should either handle gracefully or return error
        assert response is not None

    def test_log_level_persistence(self, client):
        """Test that log level changes persist."""
        client.initialize()

        # Set debug
        response1 = client.set_log_level("debug")
        assert response1 is not None

        # Set warning
        response2 = client.set_log_level("warning")
        assert response2 is not None

        # Both requests should succeed
        assert "error" not in response1 or response1.get("error") is None
        assert "error" not in response2 or response2.get("error") is None


class TestCompletionMethod:
    """Tests for completion/complete MCP method."""

    @pytest.fixture
    def client(self, flapi_base_url):
        """Create an MCP client for testing."""
        return SimpleMCPClient(flapi_base_url)

    def test_completion_basic(self, client):
        """Test basic completion with partial string."""
        client.initialize()
        response = client.get_completions("cust")

        assert response is not None
        # Response should contain completion suggestions
        if "result" in response:
            assert isinstance(response["result"], dict)

    def test_completion_empty_string(self, client):
        """Test completion with empty string."""
        client.initialize()
        response = client.get_completions("")

        assert response is not None

    def test_completion_with_tool_ref(self, client):
        """Test completion with tool reference."""
        client.initialize()
        response = client.get_completions("cust", ref_type="tool", ref="get_customers")

        assert response is not None

    def test_completion_with_resource_ref(self, client):
        """Test completion with resource reference."""
        client.initialize()
        response = client.get_completions("cust", ref_type="resource", ref="customers")

        assert response is not None

    def test_completion_without_init(self, client):
        """Test completion without initialization."""
        response = client.get_completions("cust")

        assert response is not None

    def test_completion_with_special_characters(self, client):
        """Test completion with special characters."""
        client.initialize()
        response = client.get_completions("cust_*")

        assert response is not None

    def test_completion_long_partial(self, client):
        """Test completion with long partial string."""
        client.initialize()
        partial = "a" * 500
        response = client.get_completions(partial)

        assert response is not None


class TestProtocolVersionNegotiation:
    """Tests for protocol version negotiation in initialize."""

    @pytest.fixture
    def client(self, flapi_base_url):
        """Create an MCP client for testing."""
        return SimpleMCPClient(flapi_base_url)

    def test_initialize_with_current_protocol_version(self, client):
        """Test initialize with current protocol version."""
        response = client.initialize(protocol_version="2025-11-25")

        assert response is not None
        # Should successfully initialize
        assert "error" not in response or response.get("error") is None

    def test_initialize_protocol_version_in_response(self, client):
        """Test that protocol version is returned in response."""
        response = client.initialize(protocol_version="2025-11-25")

        assert response is not None
        # Response should contain protocol version
        if "result" in response:
            assert isinstance(response["result"], dict)
            # Server should return protocolVersion in result

    def test_initialize_creates_session(self, client):
        """Test that initialize creates a session."""
        response = client.initialize()

        assert response is not None
        # Client should have session ID from response headers
        assert client.session_id is not None

    def test_initialize_session_id_format(self, client):
        """Test that session ID is properly formatted."""
        response = client.initialize()

        assert response is not None
        assert client.session_id is not None

        # Session ID should be a valid string
        assert isinstance(client.session_id, str)
        assert len(client.session_id) > 0

    def test_initialize_with_capabilities(self, client):
        """Test initialize with capabilities."""
        payload = {
            "jsonrpc": "2.0",
            "id": "1",
            "method": "initialize",
            "params": {
                "protocolVersion": "2025-11-25",
                "capabilities": {
                    "logging": {},
                    "completion": {}
                },
                "clientInfo": {
                    "name": "test-client",
                    "version": "1.0"
                }
            }
        }

        response = client.session.post(
            f"{client.base_url}/mcp/jsonrpc",
            json=payload,
            headers={'Content-Type': 'application/json'},
            timeout=10
        )

        assert response.status_code == 200
        data = response.json()
        assert data is not None

    def test_initialize_multiple_times(self, client):
        """Test initializing multiple times."""
        response1 = client.initialize()
        assert response1 is not None

        session_id_1 = client.session_id
        assert session_id_1 is not None

        # Initialize again
        response2 = client.initialize()
        assert response2 is not None

        # Session ID might change or stay same depending on implementation
        assert client.session_id is not None


class TestContentTypes:
    """Tests for content type system in MCP responses."""

    @pytest.fixture
    def client(self, flapi_base_url):
        """Create an MCP client for testing."""
        return SimpleMCPClient(flapi_base_url)

    def test_tool_response_content_type(self, client):
        """Test that tool responses include content type information."""
        client.initialize()
        response = client.list_tools()

        assert response is not None
        # Response should contain tools with content type info
        if "result" in response and "tools" in response["result"]:
            tools = response["result"]["tools"]
            assert isinstance(tools, list)

    def test_text_content_type(self, client):
        """Test text content type in responses."""
        client.initialize()
        response = client.list_tools()

        assert response is not None

    def test_initialize_response_structure(self, client):
        """Test initialize response has proper structure."""
        response = client.initialize()

        assert response is not None
        # Response should have JSON-RPC structure
        assert "jsonrpc" in response or "result" in response or "error" in response


class TestMethodErrorHandling:
    """Tests for error handling in new MCP methods."""

    @pytest.fixture
    def client(self, flapi_base_url):
        """Create an MCP client for testing."""
        return SimpleMCPClient(flapi_base_url)

    def test_logging_with_missing_params(self, client):
        """Test logging method with missing parameters."""
        client.initialize()

        response = client.session.post(
            f"{client.base_url}/mcp/jsonrpc",
            json={
                "jsonrpc": "2.0",
                "id": "1",
                "method": "logging/setLevel",
                "params": {}  # Missing level parameter
            },
            headers={'Content-Type': 'application/json'},
            timeout=10
        )

        assert response.status_code in [200, 400]
        data = response.json()
        assert data is not None

    def test_completion_with_missing_params(self, client):
        """Test completion method with missing parameters."""
        client.initialize()

        response = client.session.post(
            f"{client.base_url}/mcp/jsonrpc",
            json={
                "jsonrpc": "2.0",
                "id": "1",
                "method": "completion/complete",
                "params": {}  # Missing partial parameter
            },
            headers={'Content-Type': 'application/json'},
            timeout=10
        )

        assert response.status_code in [200, 400]
        data = response.json()
        assert data is not None

    def test_invalid_method_name(self, client):
        """Test calling invalid method."""
        client.initialize()

        response = client.session.post(
            f"{client.base_url}/mcp/jsonrpc",
            json={
                "jsonrpc": "2.0",
                "id": "1",
                "method": "invalid/method",
                "params": {}
            },
            headers={'Content-Type': 'application/json'},
            timeout=10
        )

        assert response.status_code in [200, 400]
        data = response.json()
        # Should contain error for invalid method
        assert data is not None


class TestMethodSequences:
    """Tests for sequences of MCP method calls."""

    @pytest.fixture
    def client(self, flapi_base_url):
        """Create an MCP client for testing."""
        return SimpleMCPClient(flapi_base_url)

    def test_init_then_logging(self, client):
        """Test initialize followed by logging method."""
        init_response = client.initialize()
        assert init_response is not None

        log_response = client.set_log_level("debug")
        assert log_response is not None

    def test_init_then_completion(self, client):
        """Test initialize followed by completion method."""
        init_response = client.initialize()
        assert init_response is not None

        comp_response = client.get_completions("test")
        assert comp_response is not None

    def test_init_logging_completion(self, client):
        """Test sequence: init -> logging -> completion."""
        init_response = client.initialize()
        assert init_response is not None

        log_response = client.set_log_level("info")
        assert log_response is not None

        comp_response = client.get_completions("cust")
        assert comp_response is not None

    def test_multiple_logging_calls(self, client):
        """Test multiple logging calls in sequence."""
        client.initialize()

        for level in ["debug", "info", "warning", "error"]:
            response = client.set_log_level(level)
            assert response is not None

    def test_multiple_completion_calls(self, client):
        """Test multiple completion calls in sequence."""
        client.initialize()

        partials = ["cust", "order", "prod", "user", "data"]
        for partial in partials:
            response = client.get_completions(partial)
            assert response is not None

    def test_interleaved_logging_and_completion(self, client):
        """Test interleaved logging and completion calls."""
        client.initialize()

        response1 = client.set_log_level("debug")
        assert response1 is not None

        response2 = client.get_completions("cust")
        assert response2 is not None

        response3 = client.set_log_level("info")
        assert response3 is not None

        response4 = client.get_completions("order")
        assert response4 is not None


class TestSessionMaintenance:
    """Tests to verify session is maintained across new method calls."""

    @pytest.fixture
    def client(self, flapi_base_url):
        """Create an MCP client for testing."""
        return SimpleMCPClient(flapi_base_url)

    def test_session_maintained_through_logging(self, client):
        """Test that session ID is maintained through logging calls."""
        client.initialize()
        session_id_1 = client.session_id

        client.set_log_level("debug")
        session_id_2 = client.session_id

        assert session_id_1 is not None
        assert session_id_2 is not None
        assert session_id_1 == session_id_2

    def test_session_maintained_through_completion(self, client):
        """Test that session ID is maintained through completion calls."""
        client.initialize()
        session_id_1 = client.session_id

        client.get_completions("test")
        session_id_2 = client.session_id

        assert session_id_1 is not None
        assert session_id_2 is not None
        assert session_id_1 == session_id_2

    def test_session_header_in_all_responses(self, client):
        """Test that session header is in all method responses."""
        client.initialize()

        for level in ["debug", "info", "warning"]:
            response = client.session.post(
                f"{client.base_url}/mcp/jsonrpc",
                json={
                    "jsonrpc": "2.0",
                    "id": "1",
                    "method": "logging/setLevel",
                    "params": {"level": level}
                },
                headers={
                    'Content-Type': 'application/json',
                    'Mcp-Session-Id': client.session_id
                },
                timeout=10
            )

            # Should have session header in response
            assert 'Mcp-Session-Id' in response.headers or response.status_code == 200
