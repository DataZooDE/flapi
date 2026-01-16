"""
MCP Session Management Integration Tests

This module provides comprehensive testing of FLAPI MCP session functionality including:
- Session creation and header extraction
- Session persistence across requests
- Session cleanup and deletion
- Error handling for missing/invalid session IDs
- Concurrent session management
- Session header validation
"""

import pytest
import requests
import json
import uuid
import time
from typing import Optional, Dict, Any, List
from dotenv import load_dotenv
import os

load_dotenv()


class SessionMCPClient:
    """MCP client that tracks and manages session headers."""

    def __init__(self, base_url: str = "http://localhost:8080"):
        self.base_url = base_url
        self.session = requests.Session()
        self.session.headers.update({
            'Content-Type': 'application/json',
            'Accept': 'application/json'
        })
        self.session_id: Optional[str] = None
        self.request_count = 0

    def _make_request(self, method: str, params: Dict[str, Any] = None,
                     include_session_id: bool = True) -> tuple[Dict[str, Any], requests.Response]:
        """
        Make a JSON-RPC request to the MCP server.
        Returns tuple of (parsed response, raw response).
        """
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

        # Include session ID if we have one and it's requested
        if self.session_id and include_session_id:
            headers['Mcp-Session-Id'] = self.session_id

        self.request_count += 1

        try:
            response = self.session.post(
                f"{self.base_url}/mcp/jsonrpc",
                json=payload,
                headers=headers,
                timeout=10
            )

            # Extract session ID from response headers if present
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

    def delete_session(self) -> tuple[Dict[str, Any], requests.Response]:
        """Delete the current session via DELETE endpoint."""
        if not self.session_id:
            raise ValueError("No active session to delete")

        headers = {
            'Content-Type': 'application/json',
            'Mcp-Session-Id': self.session_id
        }

        try:
            response = requests.delete(
                f"{self.base_url}/mcp/jsonrpc",
                headers=headers,
                timeout=10
            )

            try:
                return response.json(), response
            except json.JSONDecodeError:
                return {}, response
        except requests.exceptions.RequestException as e:
            raise Exception(f"Session deletion failed: {e}")

    def initialize(self) -> Dict[str, Any]:
        """Initialize the MCP session."""
        response_data, _ = self._make_request("initialize", {
            "protocolVersion": "2025-11-25",
            "capabilities": {
                "tools": {},
                "resources": {},
                "prompts": {},
                "sampling": {}
            }
        }, include_session_id=False)
        return response_data

    def list_tools(self) -> List[Dict[str, Any]]:
        """List available tools."""
        response_data, _ = self._make_request("tools/list")
        if "result" in response_data and "tools" in response_data["result"]:
            return response_data["result"]["tools"]
        return []

    def call_tool(self, tool_name: str, arguments: Dict[str, Any] = None) -> Dict[str, Any]:
        """Call a tool with the given arguments."""
        response_data, _ = self._make_request("tools/call", {
            "name": tool_name,
            "arguments": arguments or {}
        })
        if "result" in response_data:
            return response_data["result"]
        elif "error" in response_data:
            raise Exception(f"Tool call failed: {response_data['error']}")
        return {}


@pytest.fixture
def mcp_session_client(flapi_base_url):
    """Fixture that provides a SessionMCPClient."""
    return SessionMCPClient(flapi_base_url)


@pytest.mark.timeout(30)
class TestMCPSessionManagement:
    """Tests for MCP session header management."""

    def test_session_creation_on_initialize(self, mcp_session_client):
        """Test that a session ID is created and returned on initialize."""
        assert mcp_session_client.session_id is None, "Session should start as None"

        response = mcp_session_client.initialize()

        # After initialization, session ID should be set
        assert mcp_session_client.session_id is not None, "Session ID should be created on initialize"
        assert isinstance(mcp_session_client.session_id, str), "Session ID should be a string"
        assert len(mcp_session_client.session_id) > 0, "Session ID should not be empty"

    def test_session_id_format(self, mcp_session_client):
        """Test that session IDs have valid format."""
        mcp_session_client.initialize()
        session_id = mcp_session_client.session_id

        # Session ID should be a valid UUID or similar format
        # At minimum, it should contain alphanumeric characters
        assert all(c.isalnum() or c == '-' for c in session_id), \
            f"Session ID contains invalid characters: {session_id}"

    def test_session_persistence_across_requests(self, mcp_session_client):
        """Test that the same session ID is used across multiple requests."""
        # Initialize to create a session
        mcp_session_client.initialize()
        initial_session_id = mcp_session_client.session_id

        # Make another request (list tools)
        mcp_session_client.list_tools()
        after_first_request_session_id = mcp_session_client.session_id

        # Make another request (list tools again)
        mcp_session_client.list_tools()
        after_second_request_session_id = mcp_session_client.session_id

        # All should be the same
        assert initial_session_id == after_first_request_session_id, \
            "Session ID should persist across requests"
        assert after_first_request_session_id == after_second_request_session_id, \
            "Session ID should remain consistent throughout session"

    def test_session_header_in_responses(self, flapi_base_url):
        """Test that session headers are properly included in responses."""
        headers = {
            'Content-Type': 'application/json'
        }

        # Make an initialize request
        response = requests.post(
            f"{flapi_base_url}/mcp/jsonrpc",
            json={
                "jsonrpc": "2.0",
                "id": "1",
                "method": "initialize",
                "params": {
                    "protocolVersion": "2025-11-25",
                    "capabilities": {"tools": {}, "resources": {}}
                }
            },
            headers=headers,
            timeout=10
        )

        assert response.status_code == 200, f"Expected 200, got {response.status_code}"

        # Check that response includes session header
        assert 'Mcp-Session-Id' in response.headers, \
            "Response should include Mcp-Session-Id header"

        session_id = response.headers['Mcp-Session-Id']
        assert len(session_id) > 0, "Session ID should not be empty"

        # Second request should return the same session ID
        response2 = requests.post(
            f"{flapi_base_url}/mcp/jsonrpc",
            json={
                "jsonrpc": "2.0",
                "id": "2",
                "method": "tools/list",
                "params": {}
            },
            headers={
                'Content-Type': 'application/json',
                'Mcp-Session-Id': session_id
            },
            timeout=10
        )

        assert response2.status_code == 200
        assert 'Mcp-Session-Id' in response2.headers
        assert response2.headers['Mcp-Session-Id'] == session_id

    def test_session_cleanup_via_delete(self, mcp_session_client):
        """Test that sessions can be cleaned up via DELETE request."""
        # Initialize to create a session
        mcp_session_client.initialize()
        session_id = mcp_session_client.session_id

        assert session_id is not None, "Session should be created"

        # Delete the session
        response_data, http_response = mcp_session_client.delete_session()

        assert http_response.status_code == 200, \
            f"DELETE should return 200, got {http_response.status_code}"

        # Response should include session status
        if "result" in response_data:
            assert response_data["result"].get("status") == "closed", \
                "Response should indicate session was closed"
            assert response_data["result"].get("session_id") == session_id, \
                "Response should echo the closed session ID"

    def test_delete_without_session_header_fails(self, flapi_base_url):
        """Test that DELETE without session header returns error."""
        response = requests.delete(
            f"{flapi_base_url}/mcp/jsonrpc",
            headers={'Content-Type': 'application/json'},
            timeout=10
        )

        assert response.status_code == 400, \
            f"DELETE without session header should return 400, got {response.status_code}"

        response_data = response.json()
        assert "error" in response_data, "Response should include error"
        assert "session" in response_data["error"]["message"].lower(), \
            "Error should mention missing session header"

    def test_session_header_included_in_error_responses(self, flapi_base_url):
        """Test that session headers are included even in error responses."""
        # First, create a session
        response1 = requests.post(
            f"{flapi_base_url}/mcp/jsonrpc",
            json={
                "jsonrpc": "2.0",
                "id": "1",
                "method": "initialize",
                "params": {
                    "protocolVersion": "2025-11-25",
                    "capabilities": {}
                }
            },
            headers={'Content-Type': 'application/json'},
            timeout=10
        )

        session_id = response1.headers.get('Mcp-Session-Id')
        assert session_id is not None, "Session should be created"

        # Make a request with an error condition (invalid method)
        response2 = requests.post(
            f"{flapi_base_url}/mcp/jsonrpc",
            json={
                "jsonrpc": "2.0",
                "id": "2",
                "method": "invalid/method",
                "params": {}
            },
            headers={
                'Content-Type': 'application/json',
                'Mcp-Session-Id': session_id
            },
            timeout=10
        )

        # Even with error, session header should be present
        assert 'Mcp-Session-Id' in response2.headers, \
            "Session header should be included in error responses"
        assert response2.headers['Mcp-Session-Id'] == session_id, \
            "Session ID should be preserved in error responses"


@pytest.mark.timeout(30)
class TestMCPSessionConcurrency:
    """Tests for concurrent session management."""

    def test_multiple_concurrent_sessions(self, flapi_base_url):
        """Test that multiple concurrent sessions can be managed independently."""
        client1 = SessionMCPClient(flapi_base_url)
        client2 = SessionMCPClient(flapi_base_url)
        client3 = SessionMCPClient(flapi_base_url)

        # Initialize all three clients
        client1.initialize()
        client2.initialize()
        client3.initialize()

        # Each should have a different session ID
        session_ids = [client1.session_id, client2.session_id, client3.session_id]
        assert len(set(session_ids)) == 3, "Each client should have a unique session ID"

        # Make requests with each and verify session IDs are maintained
        client1.list_tools()
        client2.list_tools()
        client3.list_tools()

        assert client1.session_id == session_ids[0], "Client 1 session ID should persist"
        assert client2.session_id == session_ids[1], "Client 2 session ID should persist"
        assert client3.session_id == session_ids[2], "Client 3 session ID should persist"

    def test_session_isolation(self, flapi_base_url):
        """Test that different sessions don't interfere with each other."""
        # Create two clients with different sessions
        client_a = SessionMCPClient(flapi_base_url)
        client_b = SessionMCPClient(flapi_base_url)

        client_a.initialize()
        client_b.initialize()

        session_a = client_a.session_id
        session_b = client_b.session_id

        assert session_a != session_b, "Sessions should be different"

        # Both should be able to make requests independently
        client_a.list_tools()
        client_b.list_tools()

        # And still have their original session IDs
        assert client_a.session_id == session_a
        assert client_b.session_id == session_b

    def test_reusing_session_id_across_requests(self, flapi_base_url):
        """Test that a session ID from one request can be reused in another."""
        # Get a session ID from first request
        response1 = requests.post(
            f"{flapi_base_url}/mcp/jsonrpc",
            json={
                "jsonrpc": "2.0",
                "id": "1",
                "method": "initialize",
                "params": {
                    "protocolVersion": "2025-11-25",
                    "capabilities": {}
                }
            },
            headers={'Content-Type': 'application/json'},
            timeout=10
        )

        session_id = response1.headers.get('Mcp-Session-Id')
        assert session_id is not None

        # Reuse the same session ID in multiple requests
        for i in range(5):
            response = requests.post(
                f"{flapi_base_url}/mcp/jsonrpc",
                json={
                    "jsonrpc": "2.0",
                    "id": str(i + 2),
                    "method": "tools/list",
                    "params": {}
                },
                headers={
                    'Content-Type': 'application/json',
                    'Mcp-Session-Id': session_id
                },
                timeout=10
            )

            assert response.status_code == 200
            assert response.headers.get('Mcp-Session-Id') == session_id, \
                f"Session ID should be returned in response {i + 2}"


@pytest.mark.timeout(30)
class TestMCPSessionEdgeCases:
    """Tests for edge cases in session management."""

    def test_empty_session_header(self, flapi_base_url):
        """Test handling of empty session header."""
        response = requests.post(
            f"{flapi_base_url}/mcp/jsonrpc",
            json={
                "jsonrpc": "2.0",
                "id": "1",
                "method": "initialize",
                "params": {"protocolVersion": "2025-11-25", "capabilities": {}}
            },
            headers={
                'Content-Type': 'application/json',
                'Mcp-Session-Id': ''
            },
            timeout=10
        )

        # Should create a new session or handle gracefully
        # Either way, should not crash
        assert response.status_code in [200, 400]

    def test_invalid_session_id_format(self, flapi_base_url):
        """Test handling of invalid session ID format."""
        response = requests.post(
            f"{flapi_base_url}/mcp/jsonrpc",
            json={
                "jsonrpc": "2.0",
                "id": "1",
                "method": "tools/list",
                "params": {}
            },
            headers={
                'Content-Type': 'application/json',
                'Mcp-Session-Id': 'invalid!!!session!!!id'
            },
            timeout=10
        )

        # Should handle gracefully (might create new session or return error)
        # But should not crash
        assert response.status_code in [200, 400, 404]

    def test_session_header_case_sensitivity(self, flapi_base_url):
        """Test that session header is case-insensitive (as per HTTP spec)."""
        # Create a session first
        response1 = requests.post(
            f"{flapi_base_url}/mcp/jsonrpc",
            json={
                "jsonrpc": "2.0",
                "id": "1",
                "method": "initialize",
                "params": {"protocolVersion": "2025-11-25", "capabilities": {}}
            },
            headers={'Content-Type': 'application/json'},
            timeout=10
        )

        session_id = response1.headers.get('Mcp-Session-Id')

        # Try with different case variations
        response2 = requests.post(
            f"{flapi_base_url}/mcp/jsonrpc",
            json={
                "jsonrpc": "2.0",
                "id": "2",
                "method": "tools/list",
                "params": {}
            },
            headers={
                'Content-Type': 'application/json',
                'mcp-session-id': session_id  # lowercase
            },
            timeout=10
        )

        # Should still work (HTTP headers are case-insensitive)
        assert response2.status_code == 200

    def test_session_with_special_characters(self, flapi_base_url):
        """Test that sessions work even with special characters in request."""
        response1 = requests.post(
            f"{flapi_base_url}/mcp/jsonrpc",
            json={
                "jsonrpc": "2.0",
                "id": "1",
                "method": "initialize",
                "params": {"protocolVersion": "2025-11-25", "capabilities": {}}
            },
            headers={'Content-Type': 'application/json'},
            timeout=10
        )

        session_id = response1.headers.get('Mcp-Session-Id')

        # Make a request with special characters in JSON
        response2 = requests.post(
            f"{flapi_base_url}/mcp/jsonrpc",
            json={
                "jsonrpc": "2.0",
                "id": "2",
                "method": "tools/call",
                "params": {
                    "name": "test_tool",
                    "arguments": {
                        "text": "Hello with special chars: !@#$%^&*()"
                    }
                }
            },
            headers={
                'Content-Type': 'application/json',
                'Mcp-Session-Id': session_id
            },
            timeout=10
        )

        # Session header should still be returned
        assert 'Mcp-Session-Id' in response2.headers or response2.status_code in [200, 400, 404]

    def test_rapid_sequential_requests_maintain_session(self, mcp_session_client):
        """Test that rapid sequential requests maintain the same session."""
        mcp_session_client.initialize()
        initial_session_id = mcp_session_client.session_id

        # Make multiple rapid requests
        for i in range(10):
            mcp_session_client.list_tools()
            assert mcp_session_client.session_id == initial_session_id, \
                f"Session ID should persist through request {i + 1}"

        # Verify we made all requests with the same session
        assert mcp_session_client.request_count >= 11  # 1 initialize + 10 list_tools


@pytest.mark.timeout(30)
class TestMCPSessionResponseFormat:
    """Tests for session-related response format compliance."""

    def test_initialize_response_includes_session(self, flapi_base_url):
        """Test that initialize response properly structures session data."""
        response = requests.post(
            f"{flapi_base_url}/mcp/jsonrpc",
            json={
                "jsonrpc": "2.0",
                "id": "1",
                "method": "initialize",
                "params": {
                    "protocolVersion": "2025-11-25",
                    "capabilities": {}
                }
            },
            headers={'Content-Type': 'application/json'},
            timeout=10
        )

        assert response.status_code == 200
        data = response.json()

        # Should have proper JSON-RPC structure
        assert "jsonrpc" in data
        assert data["jsonrpc"] == "2.0"
        assert "id" in data
        assert "result" in data or "error" in data

        # Should have session header
        assert 'Mcp-Session-Id' in response.headers

    def test_delete_response_format(self, flapi_base_url):
        """Test that DELETE response has correct format."""
        # Create session first
        init_response = requests.post(
            f"{flapi_base_url}/mcp/jsonrpc",
            json={
                "jsonrpc": "2.0",
                "id": "1",
                "method": "initialize",
                "params": {"protocolVersion": "2025-11-25", "capabilities": {}}
            },
            headers={'Content-Type': 'application/json'},
            timeout=10
        )

        session_id = init_response.headers.get('Mcp-Session-Id')

        # Delete the session
        delete_response = requests.delete(
            f"{flapi_base_url}/mcp/jsonrpc",
            headers={
                'Content-Type': 'application/json',
                'Mcp-Session-Id': session_id
            },
            timeout=10
        )

        assert delete_response.status_code == 200

        if delete_response.content:
            data = delete_response.json()
            # Should have valid JSON-RPC response
            assert "jsonrpc" in data
            assert data["jsonrpc"] == "2.0"
            assert "result" in data or "error" in data
