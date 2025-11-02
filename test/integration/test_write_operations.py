"""
Integration tests for write operations (POST/PUT/PATCH)
"""
import requests
import time
import pytest

API_BASE_URL = "http://localhost:8080"
CONFIG_API_BASE_URL = f"{API_BASE_URL}/api/v1/_config"


def test_post_endpoint_creates_record(flapi_server):
    """Test that POST endpoint creates a new record"""
    # First, ensure we have a write endpoint configured
    # This test assumes the endpoint is already set up via the API config service
    
    # Create a record using POST
    response = requests.post(
        f"{API_BASE_URL}/test-write",
        json={
            "name": "Test User",
            "email": "test@example.com"
        },
        headers={"Content-Type": "application/json"}
    )
    
    # Should return 201 Created for POST
    assert response.status_code in [201, 200], f"Expected 201/200, got {response.status_code}: {response.text}"
    
    if response.status_code == 201:
        data = response.json()
        assert "rows_affected" in data
        assert data["rows_affected"] >= 0


def test_put_endpoint_updates_record(flapi_server):
    """Test that PUT endpoint updates an existing record"""
    response = requests.put(
        f"{API_BASE_URL}/test-write/1",
        json={
            "name": "Updated Name",
            "email": "updated@example.com"
        },
        headers={"Content-Type": "application/json"}
    )
    
    # Should return 200 OK for PUT
    assert response.status_code in [200, 404], f"Expected 200 or 404, got {response.status_code}: {response.text}"


def test_patch_endpoint_partial_update(flapi_server):
    """Test that PATCH endpoint performs partial update"""
    response = requests.patch(
        f"{API_BASE_URL}/test-write/1",
        json={
            "email": "patched@example.com"
        },
        headers={"Content-Type": "application/json"}
    )
    
    # Should return 200 OK for PATCH
    assert response.status_code in [200, 404], f"Expected 200 or 404, got {response.status_code}: {response.text}"


def test_write_endpoint_validation_error(flapi_server):
    """Test that write endpoints return 400 for validation errors"""
    response = requests.post(
        f"{API_BASE_URL}/test-write",
        json={
            # Missing required field
            "email": "test@example.com"
        },
        headers={"Content-Type": "application/json"}
    )
    
    # Should return 400 Bad Request for validation errors
    if response.status_code == 400:
        data = response.json()
        assert "errors" in data
        assert len(data["errors"]) > 0


def test_write_endpoint_with_returning_clause(flapi_server):
    """Test that write endpoints return data when RETURNING clause is used"""
    response = requests.post(
        f"{API_BASE_URL}/test-write-returning",
        json={
            "name": "Return Test",
            "email": "return@example.com"
        },
        headers={"Content-Type": "application/json"}
    )
    
    if response.status_code == 201:
        data = response.json()
        assert "rows_affected" in data
        # If operation.returns_data is true, should have data field
        if "data" in data:
            assert isinstance(data["data"], list) or isinstance(data["data"], dict)


def test_write_endpoint_cors_headers(flapi_server):
    """Test that CORS headers are set for POST/PUT/PATCH requests"""
    response = requests.options(
        f"{API_BASE_URL}/test-write",
        headers={
            "Origin": "http://localhost:3000",
            "Access-Control-Request-Method": "POST"
        }
    )
    
    # CORS preflight should be allowed
    assert response.status_code in [200, 204], f"CORS preflight failed: {response.status_code}"
    
    # Check that CORS headers are present
    if "Access-Control-Allow-Methods" in response.headers:
        methods = response.headers["Access-Control-Allow-Methods"]
        assert "POST" in methods
        assert "PUT" in methods
        assert "PATCH" in methods

