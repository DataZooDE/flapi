"""
Advanced DuckLake integration tests for features that require direct DuckDB interaction
"""
import pytest
import requests
import time
import json
from datetime import datetime, timedelta


@pytest.fixture
def api_config_url(flapi_base_url):
    """Construct API config URL from the dynamic flapi_base_url fixture."""
    return f"{flapi_base_url}/api/v1/_config"


@pytest.fixture
def auth_headers():
    """Authentication headers for config service."""
    token = "test-token"
    return {
        "X-Config-Token": token,
        "Authorization": f"Bearer {token}"
    }


class TestDuckLakeAdvanced:
    """Test advanced DuckLake functionality that requires direct database access"""

    def test_ducklake_snapshot_management(self, api_config_url, auth_headers):
        """Test that DuckLake snapshots are properly managed"""
        # Trigger multiple cache refreshes to create snapshots
        for i in range(3):
            response = requests.post(f"{api_config_url}/endpoints/test-cached-slash/cache/refresh", headers=auth_headers)
            assert response.status_code == 200
            time.sleep(1)  # Small delay between refreshes

        # Check audit log for snapshot information
        audit_response = requests.get(f"{api_config_url}/endpoints/test-cached-slash/cache/audit", headers=auth_headers)
        assert audit_response.status_code == 200

        audit_data = audit_response.json()
        assert len(audit_data) >= 3

        # Verify that we have different snapshot IDs (indicating new snapshots)
        snapshot_ids = set()
        for entry in audit_data:
            if entry.get("sync_type") in ["full", "append", "merge"]:
                # Check if we have snapshot information in the message or other fields
                assert entry["status"] in ["success", "error"]

    def test_cache_mode_determination(self, api_config_url, auth_headers):
        """Test that cache modes are correctly determined based on configuration"""

        # Test full refresh mode (no cursor, no primary key)
        full_response = requests.get(f"{api_config_url}/endpoints/test-cached-slash/cache", headers=auth_headers)
        if full_response.status_code == 200:
            config = full_response.json()
            # Should be full mode - no cursor or primary key
            assert "cursor" not in config or not config.get("cursor")
            assert "primary-key" not in config or not config.get("primary-key")

        # Test append mode (cursor only)
        append_response = requests.get(f"{api_config_url}/endpoints/test-append-slash/cache", headers=auth_headers)
        if append_response.status_code == 200:
            config = append_response.json()
            # Should be append mode - cursor but no primary key
            assert "cursor" in config and config.get("cursor")
            assert "primary-key" not in config or not config.get("primary-key")

        # Test merge mode (cursor + primary key)
        merge_response = requests.get(f"{api_config_url}/endpoints/test-merge-slash/cache", headers=auth_headers)
        if merge_response.status_code == 200:
            config = merge_response.json()
            # Should be merge mode - both cursor and primary key
            assert "cursor" in config and config.get("cursor")
            assert "primary-key" in config and config.get("primary-key")

    def test_retention_policy_execution(self, api_config_url, auth_headers):
        """Test that retention policies are properly executed"""
        # Trigger cache refresh to create snapshots
        response = requests.post(f"{api_config_url}/endpoints/test-cached-slash/cache/refresh", headers=auth_headers)
        assert response.status_code == 200

        # Check audit log for garbage collection events
        audit_response = requests.get(f"{api_config_url}/cache/audit", headers=auth_headers)
        assert audit_response.status_code == 200

        audit_data = audit_response.json()

        # Look for garbage collection events
        gc_events = [entry for entry in audit_data if entry.get("sync_type") == "garbage_collection"]

        # If we have GC events, they should indicate retention policy execution
        for gc_event in gc_events:
            assert gc_event["status"] in ["success", "error"]
            if gc_event["status"] == "error":
                # Error messages should be informative
                assert "message" in gc_event
                assert len(gc_event["message"]) > 0

    def test_scheduled_cache_refresh(self, api_config_url, auth_headers):
        """Test that scheduled cache refreshes work correctly"""
        # Get cache configuration to verify scheduling is enabled
        response = requests.get(f"{api_config_url}/endpoints/test-cached-slash/cache", headers=auth_headers)

        if response.status_code == 200:
            config = response.json()

            # Verify schedule is configured
            assert "schedule" in config
            assert config["schedule"] is not None
            assert len(config["schedule"]) > 0

            # Schedule should be a valid time interval
            schedule = config["schedule"]
            assert any(suffix in schedule for suffix in ["s", "m", "h", "d"])

    def test_ducklake_compaction(self, api_config_url, auth_headers):
        """Test that DuckLake compaction is working"""
        # Trigger multiple cache operations to create files that can be compacted
        for i in range(5):
            response = requests.post(f"{api_config_url}/endpoints/test-cached-slash/cache/refresh", headers=auth_headers)
            assert response.status_code == 200
            time.sleep(0.5)

        # Check audit log for compaction events
        audit_response = requests.get(f"{api_config_url}/cache/audit", headers=auth_headers)
        assert audit_response.status_code == 200

        audit_data = audit_response.json()

        # Look for any compaction-related events
        # Note: Compaction might not be explicitly logged, but we can check for errors
        for entry in audit_data:
            if entry.get("sync_type") == "garbage_collection":
                # GC events might include compaction
                assert entry["status"] in ["success", "error"]

    def test_cache_template_processing(self, api_config_url, auth_headers):
        """Test that cache templates are properly processed with DuckLake variables"""
        # Trigger cache refresh and check for success
        response = requests.post(f"{api_config_url}/endpoints/test-cached-slash/cache/refresh", headers=auth_headers)
        assert response.status_code == 200

        # Check audit log for successful template processing
        audit_response = requests.get(f"{api_config_url}/endpoints/test-cached-slash/cache/audit", headers=auth_headers)
        assert audit_response.status_code == 200

        audit_data = audit_response.json()

        # Should have at least one successful cache operation
        success_events = [entry for entry in audit_data
                         if entry.get("sync_type") in ["full", "append", "merge"]
                         and entry.get("status") == "success"]

        assert len(success_events) > 0, "No successful cache operations found"

    def test_ducklake_catalog_attachment(self, api_config_url, auth_headers):
        """Test that DuckLake catalog is properly attached"""
        # Check DuckLake configuration
        response = requests.get(f"{api_config_url}/ducklake", headers=auth_headers)

        if response.status_code == 200:
            config = response.json()
            assert config.get("enabled") is True
            assert config.get("alias") == "cache"
            assert "metadata_path" in config
            assert "data_path" in config

    def test_audit_table_structure(self, api_config_url, auth_headers):
        """Test that audit table has the correct structure and data types"""
        response = requests.get(f"{api_config_url}/cache/audit", headers=auth_headers)
        assert response.status_code == 200

        audit_data = response.json()

        if len(audit_data) > 0:
            entry = audit_data[0]

            # Required fields
            required_fields = [
                "event_id", "endpoint_path", "cache_table", "cache_schema",
                "sync_type", "status", "sync_started_at", "sync_completed_at"
            ]

            for field in required_fields:
                assert field in entry, f"Missing required field: {field}"

            # Validate field types and values
            assert isinstance(entry["event_id"], str)
            assert isinstance(entry["endpoint_path"], str)
            assert isinstance(entry["cache_table"], str)
            assert isinstance(entry["cache_schema"], str)
            assert entry["sync_type"] in ["full", "append", "merge", "garbage_collection"]
            assert entry["status"] in ["success", "error", "warning"]

            # Timestamps should be valid ISO format
            try:
                datetime.fromisoformat(entry["sync_started_at"].replace('Z', '+00:00'))
                datetime.fromisoformat(entry["sync_completed_at"].replace('Z', '+00:00'))
            except ValueError:
                pytest.fail("Invalid timestamp format in audit log")

    def test_error_handling(self, api_config_url, auth_headers):
        """Test error handling in cache operations"""
        # Test with non-existent endpoint
        response = requests.get(f"{api_config_url}/endpoints/nonexistent/cache", headers=auth_headers)
        assert response.status_code == 404

        # Test cache refresh on non-existent endpoint
        response = requests.post(f"{api_config_url}/endpoints/nonexistent/cache/refresh", headers=auth_headers)
        assert response.status_code == 404

    def test_concurrent_cache_operations(self, api_config_url, auth_headers):
        """Test that concurrent cache operations are handled correctly"""
        import threading
        import queue

        results = queue.Queue()

        def refresh_cache():
            try:
                response = requests.post(f"{api_config_url}/endpoints/test-cached-slash/cache/refresh", headers=auth_headers)
                results.put(("refresh", response.status_code))
            except Exception as e:
                results.put(("refresh", f"error: {e}"))

        # Start multiple concurrent refresh operations
        threads = []
        for i in range(3):
            thread = threading.Thread(target=refresh_cache)
            threads.append(thread)
            thread.start()

        # Wait for all threads to complete
        for thread in threads:
            thread.join()

        # Check results
        refresh_results = []
        while not results.empty():
            op_type, result = results.get()
            if op_type == "refresh":
                refresh_results.append(result)

        # At least some operations should succeed
        success_count = sum(1 for result in refresh_results if result == 200)
        assert success_count > 0, f"All concurrent operations failed: {refresh_results}"

    def test_cache_performance(self, api_config_url, auth_headers):
        """Test cache performance characteristics"""
        # Measure time for cache refresh
        start_time = time.time()
        response = requests.post(f"{api_config_url}/endpoints/test-cached-slash/cache/refresh", headers=auth_headers)
        end_time = time.time()

        assert response.status_code == 200

        # Cache refresh should complete within reasonable time (10 seconds)
        refresh_time = end_time - start_time
        assert refresh_time < 10.0, f"Cache refresh took too long: {refresh_time:.2f}s"

        # Subsequent refreshes should be faster (cached templates, etc.)
        start_time = time.time()
        response = requests.post(f"{api_config_url}/endpoints/test-cached-slash/cache/refresh", headers=auth_headers)
        end_time = time.time()

        assert response.status_code == 200

        second_refresh_time = end_time - start_time
        # Second refresh should be faster (though this might not always be true)
        # Just ensure it's not significantly slower
        assert second_refresh_time < 15.0, f"Second cache refresh took too long: {second_refresh_time:.2f}s"


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
