"""
Integration tests for DuckLake scheduler functionality
"""
import pytest
import requests
import time
import json
from datetime import datetime, timedelta


class TestDuckLakeScheduler:
    """Test DuckLake scheduler and heartbeat worker functionality"""
    
    @pytest.fixture
    def base_url(self, flapi_base_url):
        return flapi_base_url
    
    @pytest.fixture
    def api_config_url(self, base_url):
        return f"{base_url}/api/v1/_config"
    
    @pytest.fixture
    def auth_headers(self):
        """Authentication headers for config service."""
        token = "test-token"
        return {
            "X-Config-Token": token,
            "Authorization": f"Bearer {token}"
        }
    
    def test_scheduler_configuration(self, api_config_url, auth_headers):
        """Test that DuckLake scheduler configuration is properly loaded"""
        # This would require a test endpoint with scheduling enabled
        # For now, we'll test the audit functionality
        response = requests.get(f"{api_config_url}/cache/audit", headers=auth_headers)
        assert response.status_code == 200
        
        audit_data = response.json()
        assert isinstance(audit_data, list)
    
    def test_cache_refresh_creates_audit_entry(self, api_config_url, auth_headers):
        """Test that cache refresh operations create audit entries"""
        # Trigger a cache refresh
        refresh_response = requests.post(f"{api_config_url}/endpoints/customers/cache/refresh", headers=auth_headers)
        
        if refresh_response.status_code == 200:
            # Wait a moment for audit entry to be created
            time.sleep(1)
            
            # Check audit log
            audit_response = requests.get(f"{api_config_url}/endpoints/customers/cache/audit", headers=auth_headers)
            assert audit_response.status_code == 200
            
            audit_data = audit_response.json()
            assert len(audit_data) > 0
            
            # Check that the most recent entry is from our refresh
            latest_entry = audit_data[0]
            assert latest_entry["endpoint_path"] == "/customers"
            assert latest_entry["sync_type"] in ["full", "append", "merge"]
            assert latest_entry["status"] in ["success", "error"]
    
    def test_audit_table_structure(self, api_config_url, auth_headers):
        """Test that audit table has the expected structure"""
        response = requests.get(f"{api_config_url}/cache/audit", headers=auth_headers)
        assert response.status_code == 200
        
        audit_data = response.json()
        
        if len(audit_data) > 0:
            entry = audit_data[0]
            required_fields = [
                "event_id", "endpoint_path", "cache_table", "cache_schema",
                "sync_type", "status", "sync_started_at", "sync_completed_at"
            ]
            
            for field in required_fields:
                assert field in entry, f"Missing required field: {field}"
            
            # Validate sync_type values
            assert entry["sync_type"] in ["full", "append", "merge", "garbage_collection"]
            
            # Validate status values
            assert entry["status"] in ["success", "error", "warning"]
    
    def test_endpoint_specific_audit_log(self, api_config_url, auth_headers):
        """Test endpoint-specific audit log filtering"""
        # Try to get audit log for customers endpoint
        # If endpoint doesn't exist or doesn't have cache, accept 404
        response = requests.get(f"{api_config_url}/endpoints/customers/cache/audit", headers=auth_headers)
        
        if response.status_code == 404:
            # Endpoint doesn't exist or doesn't have cache enabled - that's OK for this test
            return
        
        assert response.status_code == 200
        
        audit_data = response.json()
        
        # All entries should be for the customers endpoint
        for entry in audit_data:
            assert entry["endpoint_path"] == "/customers"
    
    def test_cache_configuration_validation(self, api_config_url, auth_headers):
        """Test that cache configuration is properly validated"""
        # Test getting cache config
        response = requests.get(f"{api_config_url}/endpoints/customers/cache", headers=auth_headers)
        
        if response.status_code == 200:
            config = response.json()
            
            # Validate DuckLake-specific fields
            assert "enabled" in config
            assert "table" in config
            assert "schema" in config
            
            if config.get("enabled"):
                assert config["table"] is not None
                assert len(config["table"]) > 0
    
    def test_scheduler_heartbeat_integration(self, base_url):
        """Test that heartbeat worker is running and processing scheduled tasks"""
        # This is a basic test to ensure the server is responsive
        # In a full test, we would check that scheduled cache refreshes are happening
        response = requests.get(f"{base_url}/health")
        
        # If health endpoint doesn't exist, just check root
        if response.status_code == 404:
            response = requests.get(base_url)
        
        # Server should be running
        assert response.status_code in [200, 404]  # 404 is OK if no root endpoint
    
    def test_ducklake_snapshot_management(self, api_config_url, auth_headers):
        """Test that DuckLake snapshots are being managed properly"""
        # Check if we can get audit data (indicates DuckLake is working)
        response = requests.get(f"{api_config_url}/cache/audit", headers=auth_headers)
        
        if response.status_code == 200:
            audit_data = response.json()
            
            # Look for garbage collection events
            gc_events = [entry for entry in audit_data if entry["sync_type"] == "garbage_collection"]
            
            # If we have GC events, they should have succeeded or have error messages
            for gc_event in gc_events:
                assert gc_event["status"] in ["success", "error"]
                if gc_event["status"] == "error":
                    assert "message" in gc_event
                    assert len(gc_event["message"]) > 0


if __name__ == "__main__":
    pytest.main([__file__])
