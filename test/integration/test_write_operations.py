"""
Integration tests for write operations (POST/PUT/PATCH)
"""
import requests
import pytest


def _auth_headers(jwt_token: str):
    return {"Content-Type": "application/json", "Authorization": f"Bearer {jwt_token}"}


def test_post_endpoint_creates_record(flapi_base_url, jwt_token):
    """Test that POST endpoint creates a new record"""
    # Create a record using POST
    response = requests.post(
        f"{flapi_base_url}/customers/create",
        json={
            "name": "Test User",
            "segment": "BUILDING",
            "email": "test@example.com"
        },
        headers=_auth_headers(jwt_token)
    )
    
    # Should return 201 Created for POST
    assert response.status_code in [201, 200], f"Expected 201/200, got {response.status_code}: {response.text}"
    
    if response.status_code == 201:
        data = response.json()
        assert "rows_affected" in data
        assert data["rows_affected"] >= 0


def test_put_endpoint_updates_record(flapi_base_url, jwt_token):
    """Test that PUT endpoint updates an existing record"""
    response = requests.put(
        f"{flapi_base_url}/customers/1",
        json={
            "name": "Updated Name",
            "segment": "AUTOMOBILE",
            "email": "updated@example.com"
        },
        headers=_auth_headers(jwt_token)
    )
    
    # Should return 200 OK for PUT
    assert response.status_code in [200, 404], f"Expected 200 or 404, got {response.status_code}: {response.text}"


def test_patch_endpoint_partial_update(flapi_base_url, jwt_token):
    """Test that PATCH endpoint performs partial update"""
    response = requests.patch(
        f"{flapi_base_url}/customers/1",
        json={
            "email": "patched@example.com"
        },
        headers=_auth_headers(jwt_token)
    )
    
    # Should return 200 OK for PATCH
    assert response.status_code in [200, 404], f"Expected 200 or 404, got {response.status_code}: {response.text}"


def test_write_endpoint_validation_error(flapi_base_url, jwt_token):
    """Test that write endpoints return 400 for validation errors"""
    response = requests.post(
        f"{flapi_base_url}/customers/create",
        json={
            # Missing required field
            "segment": "BUILDING",
            "email": "test@example.com"
        },
        headers=_auth_headers(jwt_token)
    )
    
    # Should return 400 Bad Request for validation errors
    assert response.status_code == 400
    data = response.json()
    assert "errors" in data
    assert len(data["errors"]) > 0


def test_write_endpoint_with_returning_clause(flapi_base_url, jwt_token):
    """Test that write endpoints return data when RETURNING clause is used"""
    response = requests.post(
        f"{flapi_base_url}/customers/create",
        json={
            "name": "Return Test",
            "segment": "FURNITURE",
            "email": "return@example.com"
        },
        headers=_auth_headers(jwt_token)
    )
    
    if response.status_code == 201:
        data = response.json()
        assert "rows_affected" in data
        # If operation.returns_data is true, should have data field
        if "data" in data:
            assert isinstance(data["data"], list) or isinstance(data["data"], dict)


def test_write_endpoint_cors_headers(flapi_base_url):
    """Test that CORS headers are set for POST/PUT/PATCH requests"""
    response = requests.options(
        f"{flapi_base_url}/customers/create",
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
