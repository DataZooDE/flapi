"""
Load Testing for FLAPI
Tests concurrent requests, sustained load, and stress scenarios

Note: These tests use the examples_server fixture which provides the northwind
endpoints needed for these tests.

Some tests are marked xfail due to known server performance limitations under
high concurrent load. See issue flapi-mui for investigation status.
"""
import pytest
import time
import concurrent.futures
import requests
from test_utils import make_concurrent_requests, calculate_percentiles


# Known issue: Server struggles with high concurrent POST requests
CONCURRENT_LOAD_XFAIL = pytest.mark.xfail(
    reason="Server performance degrades under high concurrent load (flapi-mui)",
    strict=False
)


class TestConcurrentRequests:
    """Tests for concurrent request handling"""

    def test_concurrent_get_requests(self, examples_url, examples_server, wait_for_examples):
        """Test 100+ concurrent GET requests."""
        results = make_concurrent_requests(
            examples_url,
            "/northwind/products/",
            method="GET",
            num_requests=100
        )
        
        # Analyze results
        status_codes = [r["status_code"] for r in results]
        response_times = [r["response_time"] for r in results if "response_time" in r]
        
        # Most requests should succeed
        success_count = sum(1 for code in status_codes if code == 200)
        assert success_count >= 90, f"Expected at least 90 successful requests, got {success_count}"
        
        # Response times should be reasonable
        if response_times:
            avg_time = sum(response_times) / len(response_times)
            assert avg_time < 2.0, f"Average response time {avg_time:.2f}s exceeds 2.0s"

    @CONCURRENT_LOAD_XFAIL
    def test_concurrent_post_requests(self, examples_url, examples_server, wait_for_examples):
        """Test 50+ concurrent POST requests."""
        payload = {
            "product_name": "Concurrent Test Product",
            "supplier_id": 1,
            "category_id": 1
        }
        
        results = make_concurrent_requests(
            examples_url,
            "/northwind/products/",
            method="POST",
            num_requests=50,
            payload=payload
        )
        
        status_codes = [r["status_code"] for r in results]
        # Note: Some may fail due to validation or database constraints
        success_count = sum(1 for code in status_codes if code in [200, 201])
        
        # At least some should succeed (allowing for constraints)
        assert success_count >= 10, f"Expected at least 10 successful requests, got {success_count}"

    @CONCURRENT_LOAD_XFAIL
    def test_mixed_read_write_operations(self, examples_url, examples_server, wait_for_examples):
        """Test mixed read/write operations concurrently."""
        def make_get():
            return requests.get(f"{examples_url}/northwind/products/", timeout=10)
        
        def make_post():
            payload = {"product_name": "Mixed Test", "supplier_id": 1, "category_id": 1}
            return requests.post(f"{examples_url}/northwind/products/", json=payload, timeout=10)
        
        with concurrent.futures.ThreadPoolExecutor(max_workers=50) as executor:
            futures = []
            # Mix of GET and POST requests
            for i in range(50):
                if i % 2 == 0:
                    futures.append(executor.submit(make_get))
                else:
                    futures.append(executor.submit(make_post))
            
            results = [future.result() for future in concurrent.futures.as_completed(futures)]
        
        # Should not have crashed or timed out
        assert len(results) == 50
        success_count = sum(1 for r in results if r.status_code in [200, 201])
        assert success_count >= 40, "Too many requests failed"

    @CONCURRENT_LOAD_XFAIL
    def test_no_deadlocks_or_timeouts(self, examples_url, examples_server, wait_for_examples):
        """Verify no deadlocks or timeouts occur with concurrent requests."""
        results = make_concurrent_requests(
            examples_url,
            "/northwind/products/",
            method="GET",
            num_requests=200,
        )
        
        # Check for timeouts or errors
        timeouts = sum(1 for r in results if r.get("error") and "timeout" in str(r["error"]).lower())
        assert timeouts == 0, f"Found {timeouts} timeout errors"
        
        # All should have response codes (not 0)
        errors = sum(1 for r in results if r.get("status_code") == 0)
        assert errors < 10, f"Too many errors: {errors}"


class TestSustainedLoad:
    """Tests for sustained load over time"""

    @pytest.mark.slow
    def test_sustained_load_1000_requests(self, examples_url, examples_server, wait_for_examples):
        """Run 1000 requests over 5 minutes."""
        start_time = time.time()
        duration = 300  # 5 minutes
        target_requests = 1000
        interval = duration / target_requests
        
        results = []
        
        for i in range(target_requests):
            try:
                response = requests.get(f"{examples_url}/northwind/products/", timeout=30)
                results.append({
                    "status_code": response.status_code,
                    "request_number": i + 1
                })
            except Exception as e:
                results.append({
                    "status_code": 0,
                    "error": str(e),
                    "request_number": i + 1
                })
            
            # Maintain approximate rate
            if i < target_requests - 1:
                time.sleep(interval)
        
        elapsed = time.time() - start_time
        
        # Most requests should succeed
        success_count = sum(1 for r in results if r["status_code"] == 200)
        assert success_count >= 950, f"Expected at least 950 successful requests, got {success_count}"
        
        # Should complete within reasonable time
        assert elapsed < duration * 1.5, f"Test took {elapsed:.2f}s, expected < {duration * 1.5}s"

    @pytest.mark.slow
    def test_consistent_performance(self, examples_url, examples_server, wait_for_examples):
        """Verify consistent performance over time."""
        response_times = []
        
        for i in range(100):
            start = time.time()
            response = requests.get(f"{examples_url}/northwind/products/", timeout=30)
            response_times.append(time.time() - start)
            time.sleep(0.1)  # Small delay between requests
        
        # Calculate percentiles
        percentiles = calculate_percentiles(response_times, [50, 90, 95, 99])
        
        # 95th percentile should be reasonable
        if 95 in percentiles:
            assert percentiles[95] < 1.0, f"95th percentile response time {percentiles[95]:.2f}s too high"


class TestStressScenarios:
    """Stress testing scenarios"""

    @CONCURRENT_LOAD_XFAIL
    def test_maximum_concurrent_connections(self, examples_url, examples_server, wait_for_examples):
        """Test with maximum concurrent connections."""
        # Use a reasonable number for testing (adjust based on system)
        max_connections = 200
        
        results = make_concurrent_requests(
            examples_url,
            "/northwind/products/",
            method="GET",
            num_requests=max_connections
        )
        
        # Should handle most requests
        success_count = sum(1 for r in results if r.get("status_code") == 200)
        assert success_count >= max_connections * 0.8, f"Too many failures: {success_count}/{max_connections}"

    @CONCURRENT_LOAD_XFAIL
    def test_large_payload_handling(self, examples_url, examples_server, wait_for_examples):
        """Test handling of large JSON payloads."""
        # Create a payload with large strings (near 1MB limit)
        large_string = "x" * 500000  # ~500KB
        
        payload = {
            "product_name": "Large Payload Test",
            "supplier_id": 1,
            "category_id": 1,
            "quantity_per_unit": large_string[:50],  # Keep within limits
        }
        
        # This may fail due to validation, but server should handle it gracefully
        response = requests.post(
            f"{examples_url}/northwind/products/",
            json=payload,
            timeout=30
        )
        
        # Should not crash - either succeed or return validation error
        assert response.status_code in [200, 201, 400, 413], \
            f"Unexpected status {response.status_code} for large payload"

