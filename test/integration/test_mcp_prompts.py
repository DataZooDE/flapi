"""
Integration tests for MCP Prompts functionality (prompts/list, prompts/get)

Tests the MCP prompts implementation:
- Listing available prompts
- Getting prompts with template substitution
- Error handling for invalid prompts
- Response structure validation
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

    def list_prompts(self) -> Dict[str, Any]:
        """List available prompts."""
        response_data, _ = self._make_request("prompts/list")
        return response_data

    def get_prompt(self, name: str, arguments: Dict[str, Any] = None) -> Dict[str, Any]:
        """Get a specific prompt by name with optional arguments."""
        params = {"name": name}
        if arguments:
            params["arguments"] = arguments
        response_data, _ = self._make_request("prompts/get", params)
        return response_data


class TestPromptsListMethod:
    """Tests for prompts/list MCP method."""

    @pytest.fixture
    def client(self, flapi_base_url):
        """Create an MCP client for testing."""
        return SimpleMCPClient(flapi_base_url)

    def test_prompts_list_returns_prompts(self, client):
        """Test that prompts/list returns available prompts."""
        client.initialize()
        response = client.list_prompts()

        assert response is not None
        assert "error" not in response or response.get("error") is None

        # Should have result with prompts array
        if "result" in response:
            result = response["result"]
            assert "prompts" in result
            assert isinstance(result["prompts"], list)

    def test_prompts_list_includes_simple_greeting(self, client):
        """Test that simple_greeting prompt is available."""
        client.initialize()
        response = client.list_prompts()

        assert response is not None
        assert "result" in response

        prompts = response["result"].get("prompts", [])
        prompt_names = [p.get("name") for p in prompts]

        assert "simple_greeting" in prompt_names, \
            f"Expected 'simple_greeting' in prompts, got: {prompt_names}"

    def test_prompts_list_prompt_structure(self, client):
        """Test that each prompt has required fields."""
        client.initialize()
        response = client.list_prompts()

        assert response is not None
        assert "result" in response

        prompts = response["result"].get("prompts", [])

        for prompt in prompts:
            assert "name" in prompt, f"Prompt missing 'name': {prompt}"
            assert "description" in prompt, f"Prompt missing 'description': {prompt}"
            # Arguments may be optional but should exist if present
            if "arguments" in prompt:
                assert isinstance(prompt["arguments"], list)

    def test_prompts_list_without_init(self, client):
        """Test prompts/list without initialization (should still work or return error)."""
        response = client.list_prompts()

        # Should return a valid response (either success or error)
        assert response is not None

    def test_prompts_list_multiple_calls(self, client):
        """Test multiple prompts/list calls return consistent results."""
        client.initialize()

        response1 = client.list_prompts()
        response2 = client.list_prompts()

        assert response1 is not None
        assert response2 is not None

        # Results should be consistent
        if "result" in response1 and "result" in response2:
            prompts1 = set(p.get("name") for p in response1["result"].get("prompts", []))
            prompts2 = set(p.get("name") for p in response2["result"].get("prompts", []))
            assert prompts1 == prompts2


class TestPromptsGetMethod:
    """Tests for prompts/get MCP method."""

    @pytest.fixture
    def client(self, flapi_base_url):
        """Create an MCP client for testing."""
        return SimpleMCPClient(flapi_base_url)

    def test_prompts_get_valid_prompt(self, client):
        """Test getting a valid prompt by name."""
        client.initialize()
        response = client.get_prompt("simple_greeting")

        assert response is not None
        assert "error" not in response or response.get("error") is None

        # Should have result with messages
        if "result" in response:
            result = response["result"]
            assert "messages" in result
            assert isinstance(result["messages"], list)
            assert len(result["messages"]) >= 1

    def test_prompts_get_with_arguments(self, client):
        """Test getting a prompt with arguments for template substitution."""
        client.initialize()
        response = client.get_prompt("simple_greeting", {"current_time": "2025-01-10T12:00:00Z"})

        assert response is not None
        assert "error" not in response or response.get("error") is None

        if "result" in response:
            result = response["result"]
            assert "messages" in result

            # The message content should contain the substituted value
            if len(result["messages"]) > 0:
                message = result["messages"][0]
                if "content" in message:
                    content = message["content"]
                    if isinstance(content, dict) and "text" in content:
                        assert "2025-01-10T12:00:00Z" in content["text"]

    def test_prompts_get_missing_name_returns_error(self, client):
        """Test that prompts/get without name returns error."""
        client.initialize()

        # Make request without name parameter
        response_data, _ = client._make_request("prompts/get", {})

        assert response_data is not None
        assert "error" in response_data
        error = response_data["error"]
        assert error["code"] == -32602  # Invalid params
        assert "name" in error["message"].lower() or "required" in error["message"].lower()

    def test_prompts_get_invalid_name_returns_error(self, client):
        """Test that prompts/get with non-existent name returns error."""
        client.initialize()
        response = client.get_prompt("non_existent_prompt_xyz")

        assert response is not None
        assert "error" in response
        error = response["error"]
        assert error["code"] == -32602  # Invalid params
        assert "not found" in error["message"].lower()

    def test_prompts_get_response_has_description(self, client):
        """Test that prompts/get response includes description."""
        client.initialize()
        response = client.get_prompt("simple_greeting")

        assert response is not None
        if "result" in response:
            result = response["result"]
            assert "description" in result
            assert isinstance(result["description"], str)

    def test_prompts_get_message_structure(self, client):
        """Test that prompts/get messages have correct structure."""
        client.initialize()
        response = client.get_prompt("simple_greeting")

        assert response is not None
        if "result" in response:
            messages = response["result"].get("messages", [])
            assert len(messages) >= 1

            for message in messages:
                assert "role" in message
                assert message["role"] in ["user", "assistant", "system"]
                assert "content" in message

    def test_prompts_get_message_content_type(self, client):
        """Test that message content has type and text fields."""
        client.initialize()
        response = client.get_prompt("simple_greeting")

        assert response is not None
        if "result" in response:
            messages = response["result"].get("messages", [])
            if len(messages) > 0:
                content = messages[0].get("content")
                if isinstance(content, dict):
                    assert "type" in content
                    assert "text" in content
                    assert content["type"] == "text"


class TestPromptsTemplateSubstitution:
    """Tests for template substitution in prompts."""

    @pytest.fixture
    def client(self, flapi_base_url):
        """Create an MCP client for testing."""
        return SimpleMCPClient(flapi_base_url)

    def test_template_substitution_replaces_placeholders(self, client):
        """Test that template substitution replaces {{placeholder}} with values."""
        client.initialize()

        # Get the prompt with an argument
        response = client.get_prompt("simple_greeting", {"current_time": "TEST_TIME_VALUE"})

        assert response is not None
        if "result" in response:
            messages = response["result"].get("messages", [])
            if len(messages) > 0:
                content = messages[0].get("content", {})
                text = content.get("text", "") if isinstance(content, dict) else str(content)

                # The placeholder should be replaced
                assert "{{current_time}}" not in text
                assert "TEST_TIME_VALUE" in text

    def test_template_substitution_missing_arg_becomes_empty(self, client):
        """Test that missing arguments result in empty replacement."""
        client.initialize()

        # Get the prompt without providing the argument
        response = client.get_prompt("simple_greeting")

        assert response is not None
        if "result" in response:
            messages = response["result"].get("messages", [])
            if len(messages) > 0:
                content = messages[0].get("content", {})
                text = content.get("text", "") if isinstance(content, dict) else str(content)

                # The placeholder should not remain as {{current_time}}
                # It should either be empty or removed
                assert "{{current_time}}" not in text

    def test_template_substitution_special_characters(self, client):
        """Test template substitution with special characters in argument."""
        client.initialize()

        # Try with special characters
        response = client.get_prompt("simple_greeting", {
            "current_time": "Test with <special> & \"characters\""
        })

        assert response is not None
        # Should not error, even with special characters
        assert "error" not in response or response.get("error") is None


class TestPromptsErrorHandling:
    """Tests for error handling in prompts methods."""

    @pytest.fixture
    def client(self, flapi_base_url):
        """Create an MCP client for testing."""
        return SimpleMCPClient(flapi_base_url)

    def test_prompts_get_null_name(self, client):
        """Test prompts/get with null name parameter."""
        client.initialize()

        response_data, _ = client._make_request("prompts/get", {"name": None})

        assert response_data is not None
        assert "error" in response_data

    def test_prompts_get_empty_name(self, client):
        """Test prompts/get with empty string name."""
        client.initialize()

        response = client.get_prompt("")

        assert response is not None
        # Should return error for empty name
        assert "error" in response

    def test_prompts_list_with_invalid_params(self, client):
        """Test prompts/list ignores invalid parameters."""
        client.initialize()

        response_data, _ = client._make_request("prompts/list", {
            "invalid_param": "value",
            "another_invalid": 123
        })

        assert response_data is not None
        # Should succeed or gracefully handle extra params
        # The actual behavior may vary, but it shouldn't crash


class TestPromptsSessionIntegration:
    """Tests for prompts methods with session management."""

    @pytest.fixture
    def client(self, flapi_base_url):
        """Create an MCP client for testing."""
        return SimpleMCPClient(flapi_base_url)

    def test_prompts_list_maintains_session(self, client):
        """Test that prompts/list maintains session ID."""
        client.initialize()
        session_id = client.session_id

        client.list_prompts()

        assert client.session_id is not None
        assert client.session_id == session_id

    def test_prompts_get_maintains_session(self, client):
        """Test that prompts/get maintains session ID."""
        client.initialize()
        session_id = client.session_id

        client.get_prompt("simple_greeting")

        assert client.session_id is not None
        assert client.session_id == session_id

    def test_prompts_sequence_list_then_get(self, client):
        """Test sequence: list prompts then get specific prompt."""
        client.initialize()

        list_response = client.list_prompts()
        assert list_response is not None

        get_response = client.get_prompt("simple_greeting")
        assert get_response is not None

    def test_prompts_interleaved_with_other_methods(self, client):
        """Test prompts methods interleaved with other MCP methods."""
        client.initialize()

        # List prompts
        list_response = client.list_prompts()
        assert list_response is not None

        # List tools (another method)
        tools_response, _ = client._make_request("tools/list")
        assert tools_response is not None

        # Get prompt
        get_response = client.get_prompt("simple_greeting")
        assert get_response is not None

        # All should succeed and maintain session
        assert client.session_id is not None
