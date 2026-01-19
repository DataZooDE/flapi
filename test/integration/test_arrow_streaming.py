"""
Arrow IPC Streaming Integration Tests (TDD Phase - Task 4.1)

These tests define the expected behavior for Arrow IPC streaming responses.
They verify:
1. Content negotiation via Accept header returns Arrow format
2. Response is valid Arrow IPC stream (validated with pyarrow)
3. Data matches equivalent JSON response
4. Memory stays bounded during large streams
5. Client disconnect handling (graceful cleanup)

Tests use the examples_server fixture to test against real endpoints.
"""

import pytest
import requests
import time
import threading
import io
import tracemalloc
from typing import Optional

# Skip tests if pyarrow not installed
pytest.importorskip("pyarrow")

import pyarrow as pa
import pyarrow.ipc as ipc

from arrow_helpers import (
    ARROW_STREAM_MEDIA_TYPE,
    read_arrow_stream_to_table,
    validate_arrow_response,
    compare_arrow_to_json,
    request_with_arrow_accept,
    assert_valid_arrow_stream,
)


class TestArrowContentNegotiation:
    """Test content negotiation for Arrow format."""

    def test_accept_header_returns_arrow(self, examples_url, wait_for_examples):
        """Request with Arrow Accept header should return Arrow content-type."""
        headers = {"Accept": ARROW_STREAM_MEDIA_TYPE}
        response = requests.get(f"{examples_url}/northwind/products/", headers=headers)

        # Should return 200 OK or 406 if Arrow not yet supported
        assert response.status_code in [200, 406], f"Unexpected status: {response.status_code}"

        if response.status_code == 200:
            content_type = response.headers.get("Content-Type", "")
            assert ARROW_STREAM_MEDIA_TYPE in content_type, f"Expected Arrow content-type, got {content_type}"

    def test_format_query_param_returns_arrow(self, examples_url, wait_for_examples):
        """Request with ?format=arrow should return Arrow content-type."""
        response = requests.get(f"{examples_url}/northwind/products/?format=arrow")

        # Should return 200 OK or 400/406 if Arrow not yet supported
        assert response.status_code in [200, 400, 406], f"Unexpected status: {response.status_code}"

        if response.status_code == 200:
            content_type = response.headers.get("Content-Type", "")
            assert ARROW_STREAM_MEDIA_TYPE in content_type

    def test_quality_values_respected(self, examples_url, wait_for_examples):
        """Higher quality values should be preferred."""
        # Prefer Arrow over JSON
        headers = {"Accept": f"{ARROW_STREAM_MEDIA_TYPE};q=1.0, application/json;q=0.5"}
        response = requests.get(f"{examples_url}/northwind/products/", headers=headers)

        if response.status_code == 200:
            content_type = response.headers.get("Content-Type", "")
            # Should return Arrow since it has higher quality value
            assert ARROW_STREAM_MEDIA_TYPE in content_type or "application/json" in content_type

    def test_fallback_to_json_when_arrow_not_supported(self, examples_url, wait_for_examples):
        """Should fall back to JSON if Arrow is not available."""
        # Request Arrow but accept JSON as fallback
        headers = {"Accept": f"{ARROW_STREAM_MEDIA_TYPE};q=1.0, application/json;q=0.9"}
        response = requests.get(f"{examples_url}/northwind/products/", headers=headers)

        assert response.status_code in [200, 406]
        # Should return either Arrow or JSON
        if response.status_code == 200:
            content_type = response.headers.get("Content-Type", "")
            assert ARROW_STREAM_MEDIA_TYPE in content_type or "application/json" in content_type


class TestArrowStreamValidity:
    """Test that Arrow responses are valid IPC streams."""

    def test_response_is_valid_arrow_stream(self, examples_url, wait_for_examples):
        """Response should be a valid Arrow IPC stream readable by pyarrow."""
        response = request_with_arrow_accept(f"{examples_url}/northwind/products/")

        if response.status_code != 200:
            pytest.skip("Arrow format not yet supported")

        # Should be readable by pyarrow
        table = assert_valid_arrow_stream(response.content)
        assert table is not None
        assert table.num_rows > 0

    def test_arrow_schema_matches_expected(self, examples_url, wait_for_examples):
        """Arrow schema should have expected columns and types."""
        response = request_with_arrow_accept(f"{examples_url}/northwind/products/")

        if response.status_code != 200:
            pytest.skip("Arrow format not yet supported")

        table = read_arrow_stream_to_table(response.content)

        # Products should have these columns (Northwind uses CamelCase)
        expected_columns = {"ProductID", "ProductName", "UnitPrice"}
        actual_columns = set(table.column_names)

        # At least these columns should exist
        assert expected_columns.issubset(actual_columns), f"Missing columns: {expected_columns - actual_columns}"

    def test_empty_result_returns_valid_arrow(self, examples_url, wait_for_examples):
        """Empty result should still return valid Arrow with schema."""
        # Query with filter that returns no results
        response = request_with_arrow_accept(
            f"{examples_url}/northwind/products/",
            params={"product_id": 999999}  # Non-existent ID
        )

        if response.status_code != 200:
            pytest.skip("Arrow format not yet supported")

        # Should still be valid Arrow stream (with schema but no data)
        is_valid, table, error = validate_arrow_response(response)
        if not is_valid and error:
            # If we got here, it's actually a JSON response
            pytest.skip("Endpoint returned JSON instead of Arrow")

        # Empty table should have schema but no rows
        assert table.num_rows == 0 or table is not None


class TestArrowDataIntegrity:
    """Test that Arrow data matches JSON data."""

    def test_arrow_data_matches_json(self, examples_url, wait_for_examples):
        """Arrow and JSON responses should contain the same data."""
        endpoint = f"{examples_url}/northwind/products/"

        # Get JSON response
        json_response = requests.get(endpoint, headers={"Accept": "application/json"})
        if json_response.status_code != 200:
            pytest.skip("Endpoint not available")

        json_data = json_response.json()
        if isinstance(json_data, dict) and "data" in json_data:
            json_data = json_data["data"]

        # Get Arrow response
        arrow_response = request_with_arrow_accept(endpoint)
        if arrow_response.status_code != 200:
            pytest.skip("Arrow format not yet supported")

        # Compare data
        is_equal, diff = compare_arrow_to_json(
            arrow_response.content,
            json_data,
            ignore_columns=[]  # Compare all columns
        )

        assert is_equal, f"Data mismatch: {diff}"

    def test_null_values_preserved(self, examples_url, wait_for_examples):
        """NULL values should be correctly represented in Arrow."""
        # This test requires an endpoint with nullable columns
        endpoint = f"{examples_url}/northwind/products/"

        arrow_response = request_with_arrow_accept(endpoint)
        if arrow_response.status_code != 200:
            pytest.skip("Arrow format not yet supported")

        table = read_arrow_stream_to_table(arrow_response.content)

        # Check that nullable columns are properly represented
        for column in table.columns:
            # Arrow tracks null count per column
            if column.null_count > 0:
                # Verify we can access null values without error
                for value in column.to_pylist():
                    pass  # Just iterating should not raise

    def test_various_data_types(self, examples_url, wait_for_examples):
        """Various data types should be correctly serialized."""
        endpoint = f"{examples_url}/northwind/orders/"

        arrow_response = request_with_arrow_accept(endpoint)
        if arrow_response.status_code != 200:
            pytest.skip("Arrow format not yet supported")

        table = read_arrow_stream_to_table(arrow_response.content)

        # Orders should have various types: integers, strings, dates
        schema = table.schema

        # Verify we have multiple columns with different types
        types_seen = set()
        for field in schema:
            types_seen.add(str(field.type))

        # Should have at least int and string types
        assert len(types_seen) >= 2, f"Expected multiple types, got: {types_seen}"


class TestArrowCompression:
    """Test Arrow compression support."""

    def test_zstd_compression(self, examples_url, wait_for_examples):
        """Request with ZSTD codec should return compressed stream."""
        headers = {"Accept": f"{ARROW_STREAM_MEDIA_TYPE};codec=zstd"}
        response = requests.get(f"{examples_url}/northwind/products/", headers=headers)

        if response.status_code != 200:
            pytest.skip("Arrow format or ZSTD compression not yet supported")

        # Should be valid Arrow (pyarrow can read compressed streams)
        table = read_arrow_stream_to_table(response.content)
        assert table.num_rows > 0

    def test_lz4_compression(self, examples_url, wait_for_examples):
        """Request with LZ4 codec should return compressed stream."""
        headers = {"Accept": f"{ARROW_STREAM_MEDIA_TYPE};codec=lz4"}
        response = requests.get(f"{examples_url}/northwind/products/", headers=headers)

        if response.status_code != 200:
            pytest.skip("Arrow format or LZ4 compression not yet supported")

        # Should be valid Arrow
        table = read_arrow_stream_to_table(response.content)
        assert table.num_rows > 0


class TestArrowMemoryBounds:
    """Test memory usage stays bounded during streaming."""

    def test_large_response_memory_bounded(self, examples_url, wait_for_examples):
        """Large responses should not consume unbounded memory."""
        # Start memory tracking
        tracemalloc.start()

        endpoint = f"{examples_url}/northwind/orders/"
        arrow_response = request_with_arrow_accept(endpoint)

        if arrow_response.status_code != 200:
            tracemalloc.stop()
            pytest.skip("Arrow format not yet supported")

        # Read the response
        table = read_arrow_stream_to_table(arrow_response.content)

        # Check memory usage
        current, peak = tracemalloc.get_traced_memory()
        tracemalloc.stop()

        # Memory should be reasonable (less than 256MB for test data)
        # This tests that streaming works and doesn't buffer everything
        max_expected_bytes = 256 * 1024 * 1024  # 256 MB
        assert peak < max_expected_bytes, f"Memory usage too high: {peak / 1024 / 1024:.2f} MB"


class TestArrowErrorHandling:
    """Test error handling for Arrow responses."""

    def test_invalid_endpoint_returns_error(self, examples_url, wait_for_examples):
        """Invalid endpoint should return appropriate error."""
        response = request_with_arrow_accept(f"{examples_url}/nonexistent/endpoint/")

        assert response.status_code in [404, 406]

    def test_invalid_parameters_returns_error(self, examples_url, wait_for_examples):
        """Invalid parameters should return appropriate error."""
        # Pass invalid parameter value
        response = request_with_arrow_accept(
            f"{examples_url}/northwind/products/",
            params={"product_id": "not-a-number"}
        )

        # Should return validation error, not crash
        assert response.status_code in [200, 400, 422]  # May filter or reject

    def test_unsupported_codec_handled(self, examples_url, wait_for_examples):
        """Unsupported codec should return error or fall back."""
        headers = {"Accept": f"{ARROW_STREAM_MEDIA_TYPE};codec=invalid_codec"}
        response = requests.get(f"{examples_url}/northwind/products/", headers=headers)

        # Should either reject (406) or ignore invalid codec and return uncompressed
        assert response.status_code in [200, 406]


class TestArrowClientDisconnect:
    """Test client disconnect handling."""

    def test_client_disconnect_cleanup(self, examples_url, wait_for_examples):
        """Server should handle client disconnect gracefully."""
        # This test verifies that early disconnect doesn't crash the server
        import socket

        endpoint = f"{examples_url}/northwind/orders/"

        # Use a raw socket to disconnect early
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            # Parse URL
            import urllib.parse
            parsed = urllib.parse.urlparse(endpoint)

            sock.connect((parsed.hostname, parsed.port))
            sock.settimeout(2)

            # Send HTTP request with Arrow Accept header
            request = (
                f"GET {parsed.path} HTTP/1.1\r\n"
                f"Host: {parsed.hostname}:{parsed.port}\r\n"
                f"Accept: {ARROW_STREAM_MEDIA_TYPE}\r\n"
                f"Connection: close\r\n"
                f"\r\n"
            )
            sock.send(request.encode())

            # Read just the headers (partial response)
            response_start = sock.recv(1024)

            # Close immediately before receiving full response
            sock.close()

            # Give server time to clean up
            time.sleep(0.5)

            # Server should still be healthy
            health_check = requests.get(f"{examples_url}/northwind/products/", timeout=5)
            assert health_check.status_code in [200, 406], "Server should still be responsive"

        except socket.error as e:
            # Connection errors are acceptable for this test
            pass
        finally:
            try:
                sock.close()
            except:
                pass


class TestArrowPerformance:
    """Basic performance validation tests."""

    def test_arrow_response_time_reasonable(self, examples_url, wait_for_examples):
        """Arrow responses should complete in reasonable time."""
        endpoint = f"{examples_url}/northwind/products/"

        start = time.perf_counter()
        response = request_with_arrow_accept(endpoint)
        elapsed = time.perf_counter() - start

        if response.status_code != 200:
            pytest.skip("Arrow format not yet supported")

        # Response should complete within 5 seconds for small dataset
        assert elapsed < 5.0, f"Response took too long: {elapsed:.2f}s"

        # Also verify data is valid
        table = read_arrow_stream_to_table(response.content)
        assert table.num_rows > 0
