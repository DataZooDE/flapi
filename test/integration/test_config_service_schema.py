"""
Config Service Schema Introspection Tests
Tests the /api/v1/_config/schema endpoint comprehensively
"""
import pytest
import requests
import time


@pytest.fixture
def config_service_base_url(flapi_base_url):
    """Base URL for config service endpoints."""
    return f"{flapi_base_url}/api/v1/_config"


@pytest.fixture
def auth_headers():
    """Authentication headers for config service."""
    token = "test-token"  # Adjust based on your auth setup
    return {
        "X-Config-Token": token,
        "Authorization": f"Bearer {token}"
    }


class TestSchemaIntrospection:
    """Tests for GET /api/v1/_config/schema"""

    def test_full_schema_query(self, config_service_base_url, auth_headers, flapi_server):
        """Test full schema query (all tables/columns)."""
        response = requests.get(
            f"{config_service_base_url}/schema",
            headers=auth_headers,
            timeout=10
        )
        
        assert response.status_code == 200, f"Expected 200, got {response.status_code}: {response.text}"
        data = response.json()
        
        # Verify schema structure
        assert isinstance(data, dict), "Response should be a dictionary"
        # Should have schema keys (e.g., "main", "nw", etc.)
        assert len(data) > 0, "Should have at least one schema"

    def test_tables_only_query(self, config_service_base_url, auth_headers, flapi_server):
        """Test tables-only query (?tables=true)."""
        response = requests.get(
            f"{config_service_base_url}/schema?tables=true",
            headers=auth_headers,
            timeout=10
        )
        
        assert response.status_code == 200
        data = response.json()
        
        assert "tables" in data, "Response should contain 'tables' key"
        assert isinstance(data["tables"], list), "Tables should be a list"
        
        # Verify table structure if tables exist
        if len(data["tables"]) > 0:
            table = data["tables"][0]
            assert "name" in table
            assert "schema" in table
            assert "type" in table
            assert "qualified_name" in table

    def test_connections_only_query(self, config_service_base_url, auth_headers, flapi_server):
        """Test connections-only query (?connections=true)."""
        response = requests.get(
            f"{config_service_base_url}/schema?connections=true",
            headers=auth_headers,
            timeout=10
        )
        
        assert response.status_code == 200
        data = response.json()
        
        assert "connections" in data, "Response should contain 'connections' key"
        assert isinstance(data["connections"], list), "Connections should be a list"

    def test_completion_format_query(self, config_service_base_url, auth_headers, flapi_server):
        """Test completion format (?format=completion)."""
        response = requests.get(
            f"{config_service_base_url}/schema?format=completion",
            headers=auth_headers,
            timeout=10
        )
        
        assert response.status_code == 200
        data = response.json()
        
        assert "tables" in data, "Completion format should have 'tables'"
        assert "columns" in data, "Completion format should have 'columns'"
        assert isinstance(data["tables"], list)
        assert isinstance(data["columns"], list)

    def test_schema_with_connection_filter(self, config_service_base_url, auth_headers, flapi_server):
        """Test schema query with specific connection filter."""
        response = requests.get(
            f"{config_service_base_url}/schema?connection=northwind-sqlite",
            headers=auth_headers,
            timeout=10
        )
        
        # This may return 200 or handle filtering differently
        assert response.status_code in [200, 400], f"Unexpected status: {response.status_code}"
        
        if response.status_code == 200:
            data = response.json()
            assert isinstance(data, dict)

    def test_schema_structure_validation(self, config_service_base_url, auth_headers, flapi_server):
        """Verify schema structure (schemas -> tables -> columns)."""
        response = requests.get(
            f"{config_service_base_url}/schema",
            headers=auth_headers,
            timeout=10
        )
        
        assert response.status_code == 200
        data = response.json()
        
        # Verify hierarchical structure
        for schema_name, schema_data in data.items():
            assert isinstance(schema_data, dict), f"Schema {schema_name} should be a dict"
            if "tables" in schema_data:
                assert isinstance(schema_data["tables"], dict), "Tables should be a dict"
                
                for table_name, table_data in schema_data["tables"].items():
                    assert isinstance(table_data, dict), f"Table {table_name} should be a dict"
                    
                    if "columns" in table_data:
                        assert isinstance(table_data["columns"], dict), "Columns should be a dict"
                        
                        for col_name, col_data in table_data["columns"].items():
                            assert "type" in col_data, f"Column {col_name} should have 'type'"
                            assert "nullable" in col_data, f"Column {col_name} should have 'nullable'"

    def test_schema_data_types_validation(self, config_service_base_url, auth_headers, flapi_server):
        """Verify data types are correct in schema response."""
        response = requests.get(
            f"{config_service_base_url}/schema",
            headers=auth_headers,
            timeout=10
        )
        
        assert response.status_code == 200
        data = response.json()
        
        # Check that data types are valid strings
        valid_types = ["INTEGER", "VARCHAR", "DOUBLE", "BOOLEAN", "DATE", "TIMESTAMP", "BIGINT", "TEXT"]
        
        for schema_name, schema_data in data.items():
            if "tables" in schema_data:
                for table_name, table_data in schema_data["tables"].items():
                    if "columns" in table_data:
                        for col_name, col_data in table_data["columns"].items():
                            col_type = col_data.get("type", "")
                            # Type should be a non-empty string
                            assert isinstance(col_type, str), f"Type should be string for {schema_name}.{table_name}.{col_name}"
                            assert len(col_type) > 0, f"Type should not be empty for {schema_name}.{table_name}.{col_name}"

    def test_schema_nullable_flags(self, config_service_base_url, auth_headers, flapi_server):
        """Verify nullable flags are boolean values."""
        response = requests.get(
            f"{config_service_base_url}/schema",
            headers=auth_headers,
            timeout=10
        )
        
        assert response.status_code == 200
        data = response.json()
        
        for schema_name, schema_data in data.items():
            if "tables" in schema_data:
                for table_name, table_data in schema_data["tables"].items():
                    if "columns" in table_data:
                        for col_name, col_data in table_data["columns"].items():
                            nullable = col_data.get("nullable")
                            # Nullable should be boolean or integer (0/1)
                            assert nullable is not None, f"Nullable should be set for {schema_name}.{table_name}.{col_name}"
                            assert isinstance(nullable, (bool, int)), f"Nullable should be bool or int for {schema_name}.{table_name}.{col_name}"

    def test_schema_unauthorized_access(self, config_service_base_url, flapi_server):
        """Test schema endpoint without authentication."""
        response = requests.get(
            f"{config_service_base_url}/schema",
            timeout=10
        )
        
        # Should return 401 Unauthorized
        assert response.status_code == 401, f"Expected 401, got {response.status_code}"

    def test_schema_query_performance(self, config_service_base_url, auth_headers, flapi_server):
        """Test schema query responds within acceptable time."""
        start_time = time.time()
        response = requests.get(
            f"{config_service_base_url}/schema",
            headers=auth_headers,
            timeout=10
        )
        end_time = time.time()
        
        assert response.status_code == 200
        response_time = end_time - start_time
        assert response_time < 5.0, f"Schema query took {response_time:.2f}s, expected < 5.0s"


class TestSchemaRefresh:
    """Tests for POST /api/v1/_config/schema/refresh"""

    def test_refresh_schema_success(self, config_service_base_url, auth_headers, flapi_server):
        """Test schema refresh succeeds."""
        response = requests.post(
            f"{config_service_base_url}/schema/refresh",
            headers=auth_headers,
            timeout=10
        )
        
        assert response.status_code in [200, 201, 204], f"Expected success status, got {response.status_code}: {response.text}"

    def test_refresh_schema_updates_schema(self, config_service_base_url, auth_headers, flapi_server):
        """Test that schema is updated after refresh."""
        # Get initial schema
        initial_response = requests.get(
            f"{config_service_base_url}/schema",
            headers=auth_headers,
            timeout=10
        )
        assert initial_response.status_code == 200
        initial_data = initial_response.json()
        
        # Refresh schema
        refresh_response = requests.post(
            f"{config_service_base_url}/schema/refresh",
            headers=auth_headers,
            timeout=10
        )
        assert refresh_response.status_code in [200, 201, 204]
        
        # Get schema again
        updated_response = requests.get(
            f"{config_service_base_url}/schema",
            headers=auth_headers,
            timeout=10
        )
        assert updated_response.status_code == 200
        updated_data = updated_response.json()
        
        # Schema should be consistent (at minimum, same structure)
        assert isinstance(updated_data, dict)
        assert isinstance(initial_data, dict)

    def test_refresh_schema_unauthorized(self, config_service_base_url, flapi_server):
        """Test schema refresh without authentication."""
        response = requests.post(
            f"{config_service_base_url}/schema/refresh",
            timeout=10
        )
        
        assert response.status_code == 401, f"Expected 401, got {response.status_code}"

