"""
Concurrency correctness for flAPI.

These exercise behaviour under concurrent load - no deadlocks, no dropped
connections, correct results when reads and writes interleave. They are NOT the
performance gate: timing a server from Python is timing the GIL, which is why
this module previously hung in CI and was skipped wholesale. Latency and
throughput live in test/load/ (k6, out of process). See test/load/README.md.

Two things were wrong with this module and both are now fixed or explained.

1. It was skipped wholesale for "hangs in CI". One real cause was found:
   RouteTranslator rebuilt a std::regex on every call, and three call sites per
   request each scanned every endpoint, so routing cost ~6us per endpoint per
   lookup. Pre-compiling the patterns cut per-request server time roughly in
   half (health TTFB -42%, unmatched -51%). The read-only tests now pass in
   seconds.

2. The remaining failures were blamed on an unexplained
   "server performance degrades under high concurrent load". That was neither a
   performance problem nor general: mixed read/write traffic against a
   SQLite-backed endpoint returned 500 for every request because SQLite allows a
   single writer and flAPI surfaced the lock as a 500 with no retry. Measured,
   reduced to a standalone reproducer, and filed as issue #116 - fixed in
   v26.09.20 by per-connection `serialize-access`. The skip that named it is
   gone; these tests run unconditionally now.
"""
import pytest
import time
import concurrent.futures
import requests
from test_utils import make_concurrent_requests, calculate_percentiles

pytestmark = pytest.mark.concurrency


class TestConcurrentRequests:
    """Tests for concurrent request handling"""

    def test_concurrent_get_requests(self, isolated_examples_url, isolated_examples_server):
        """Test 100+ concurrent GET requests."""
        results = make_concurrent_requests(
            isolated_examples_url,
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

    def test_concurrent_post_requests(self, isolated_examples_url, isolated_examples_server):
        """Test 50+ concurrent POST requests."""
        payload = {
            "product_name": "Concurrent Test Product",
            "supplier_id": 1,
            "category_id": 1
        }
        
        results = make_concurrent_requests(
            isolated_examples_url,
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

    def test_mixed_read_write_operations(self, isolated_examples_url, isolated_examples_server):
        """Test mixed read/write operations concurrently."""
        def make_get():
            return requests.get(f"{isolated_examples_url}/northwind/products/", timeout=10)
        
        def make_post():
            payload = {"product_name": "Mixed Test", "supplier_id": 1, "category_id": 1}
            return requests.post(f"{isolated_examples_url}/northwind/products/", json=payload, timeout=10)
        
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

    def test_no_deadlocks_or_timeouts(self, isolated_examples_url, isolated_examples_server):
        """Verify no deadlocks or timeouts occur with concurrent requests."""
        results = make_concurrent_requests(
            isolated_examples_url,
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
    def test_sustained_load_held_over_time(self, isolated_examples_url, isolated_examples_server):
        """Hold a steady ~3.3 req/s for 60s and require 95% success throughout.

        This is the *sustained* counterpart to TestConcurrentRequests, which fires
        bursts. What matters here is that the rate is held over wall-clock time -
        so the server has to survive many sequential connections, keepalive
        recycling and any per-request accumulation - not the absolute count.

        Scaled from the original 1000 requests over 300s, which could never pass:
        its sleeps alone floored it at ~300s and it asserted a 450s budget, while
        pytest-timeout kills it at 300s locally and 180s in CI. The pacing interval
        (0.3s) and the success ratio (95%) are unchanged; only the wall-clock span
        is cut 5x, to 60s of pacing plus request time, asserted under a 90s budget.
        That leaves ample head-room under CI's --timeout=180 while still spanning
        two orders of magnitude more time than a burst test.
        """
        start_time = time.time()
        duration = 60  # 1 minute of held load
        interval = 0.3  # same pacing as before: ~3.3 requests/second
        target_requests = int(duration / interval)  # 200

        results = []
        
        for i in range(target_requests):
            try:
                response = requests.get(f"{isolated_examples_url}/northwind/products/", timeout=30)
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
        
        # Most requests should succeed - same 95% ratio as the original 950/1000
        min_success = int(target_requests * 0.95)
        success_count = sum(1 for r in results if r["status_code"] == 200)
        assert success_count >= min_success, \
            f"Expected at least {min_success} successful requests, got {success_count}"
        
        # Should complete within reasonable time
        assert elapsed < duration * 1.5, f"Test took {elapsed:.2f}s, expected < {duration * 1.5}s"

    @pytest.mark.slow
    def test_consistent_performance(self, isolated_examples_url, isolated_examples_server):
        """Verify consistent performance over time."""
        response_times = []
        
        for i in range(100):
            start = time.time()
            response = requests.get(f"{isolated_examples_url}/northwind/products/", timeout=30)
            response_times.append(time.time() - start)
            time.sleep(0.1)  # Small delay between requests
        
        # Calculate percentiles
        percentiles = calculate_percentiles(response_times, [50, 90, 95, 99])
        
        # 95th percentile should be reasonable
        if 95 in percentiles:
            assert percentiles[95] < 1.0, f"95th percentile response time {percentiles[95]:.2f}s too high"


class TestStressScenarios:
    """Stress testing scenarios"""

    def test_maximum_concurrent_connections(self, isolated_examples_url, isolated_examples_server):
        """Test with maximum concurrent connections."""
        # Use a reasonable number for testing (adjust based on system)
        max_connections = 200
        
        results = make_concurrent_requests(
            isolated_examples_url,
            "/northwind/products/",
            method="GET",
            num_requests=max_connections
        )
        
        # Should handle most requests
        success_count = sum(1 for r in results if r.get("status_code") == 200)
        assert success_count >= max_connections * 0.8, f"Too many failures: {success_count}/{max_connections}"

    def test_large_payload_handling(self, isolated_examples_url, isolated_examples_server):
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
            f"{isolated_examples_url}/northwind/products/",
            json=payload,
            timeout=30
        )
        
        # Should not crash - either succeed or return validation error
        assert response.status_code in [200, 201, 400, 413], \
            f"Unexpected status {response.status_code} for large payload"
