"""
Arrow IPC testing utilities for flAPI integration tests.

This module provides helpers for:
- Validating Arrow IPC stream responses
- Comparing Arrow data with JSON responses
- Benchmarking Arrow vs JSON performance
"""

import io
import time
from typing import Dict, Any, Optional, List, Tuple
import pyarrow as pa
import pyarrow.ipc as ipc
import requests


# Arrow IPC media type for content negotiation
ARROW_STREAM_MEDIA_TYPE = "application/vnd.apache.arrow.stream"
ARROW_FILE_MEDIA_TYPE = "application/vnd.apache.arrow.file"


def read_arrow_stream(data: bytes) -> pa.RecordBatchReader:
    """
    Read Arrow IPC stream format from bytes.

    Args:
        data: Raw bytes containing Arrow IPC stream

    Returns:
        RecordBatchReader for iterating over batches

    Raises:
        pa.ArrowInvalid: If data is not valid Arrow IPC stream
    """
    reader = ipc.open_stream(io.BytesIO(data))
    return reader


def read_arrow_stream_to_table(data: bytes) -> pa.Table:
    """
    Read Arrow IPC stream and convert to a Table.

    Args:
        data: Raw bytes containing Arrow IPC stream

    Returns:
        Arrow Table containing all data from the stream
    """
    reader = read_arrow_stream(data)
    return reader.read_all()


def validate_arrow_response(
    response: requests.Response,
    expected_columns: Optional[List[str]] = None,
    expected_row_count: Optional[int] = None,
) -> Tuple[bool, pa.Table, Optional[str]]:
    """
    Validate an HTTP response contains valid Arrow IPC stream.

    Args:
        response: HTTP response object
        expected_columns: Optional list of expected column names
        expected_row_count: Optional expected number of rows

    Returns:
        Tuple of (is_valid, table, error_message)
    """
    # Check content type
    content_type = response.headers.get("Content-Type", "")
    if ARROW_STREAM_MEDIA_TYPE not in content_type:
        return False, None, f"Expected content-type {ARROW_STREAM_MEDIA_TYPE}, got {content_type}"

    # Try to parse Arrow data
    try:
        table = read_arrow_stream_to_table(response.content)
    except pa.ArrowInvalid as e:
        return False, None, f"Invalid Arrow IPC stream: {e}"
    except Exception as e:
        return False, None, f"Error reading Arrow data: {e}"

    # Validate columns if specified
    if expected_columns is not None:
        actual_columns = table.column_names
        if set(actual_columns) != set(expected_columns):
            return False, table, f"Column mismatch. Expected {expected_columns}, got {actual_columns}"

    # Validate row count if specified
    if expected_row_count is not None:
        if table.num_rows != expected_row_count:
            return False, table, f"Row count mismatch. Expected {expected_row_count}, got {table.num_rows}"

    return True, table, None


def compare_arrow_to_json(
    arrow_data: bytes,
    json_data: List[Dict[str, Any]],
    ignore_columns: Optional[List[str]] = None,
) -> Tuple[bool, Optional[str]]:
    """
    Compare Arrow IPC stream data with equivalent JSON data.

    Args:
        arrow_data: Raw bytes containing Arrow IPC stream
        json_data: List of dictionaries from JSON response
        ignore_columns: Optional list of columns to ignore in comparison

    Returns:
        Tuple of (is_equal, difference_description)
    """
    try:
        table = read_arrow_stream_to_table(arrow_data)
    except Exception as e:
        return False, f"Failed to read Arrow data: {e}"

    # Convert Arrow table to list of dicts for comparison
    arrow_records = table.to_pylist()

    # Filter columns if needed
    if ignore_columns:
        arrow_records = [
            {k: v for k, v in record.items() if k not in ignore_columns}
            for record in arrow_records
        ]
        json_data = [
            {k: v for k, v in record.items() if k not in ignore_columns}
            for record in json_data
        ]

    # Check row counts
    if len(arrow_records) != len(json_data):
        return False, f"Row count differs: Arrow has {len(arrow_records)}, JSON has {len(json_data)}"

    # Compare records
    for i, (arrow_rec, json_rec) in enumerate(zip(arrow_records, json_data)):
        # Normalize None values (JSON uses null, Arrow uses None)
        arrow_normalized = {k: v for k, v in arrow_rec.items()}
        json_normalized = {k: v for k, v in json_rec.items()}

        if arrow_normalized != json_normalized:
            return False, f"Record {i} differs:\n  Arrow: {arrow_normalized}\n  JSON: {json_normalized}"

    return True, None


class ArrowBenchmark:
    """
    Benchmark utility for comparing Arrow vs JSON performance.
    """

    def __init__(self, base_url: str, endpoint: str):
        """
        Initialize benchmark for a specific endpoint.

        Args:
            base_url: Base URL of the flAPI server
            endpoint: Endpoint path to benchmark
        """
        self.base_url = base_url
        self.endpoint = endpoint
        self.url = f"{base_url}{endpoint}"

    def _fetch_json(self, params: Optional[Dict[str, Any]] = None) -> Tuple[float, int, Any]:
        """Fetch JSON and measure time. Returns (time_seconds, bytes, data)."""
        headers = {"Accept": "application/json"}
        start = time.perf_counter()
        response = requests.get(self.url, headers=headers, params=params)
        elapsed = time.perf_counter() - start
        return elapsed, len(response.content), response.json()

    def _fetch_arrow(self, params: Optional[Dict[str, Any]] = None) -> Tuple[float, int, pa.Table]:
        """Fetch Arrow and measure time. Returns (time_seconds, bytes, table)."""
        headers = {"Accept": ARROW_STREAM_MEDIA_TYPE}
        start = time.perf_counter()
        response = requests.get(self.url, headers=headers, params=params)
        elapsed = time.perf_counter() - start

        table = read_arrow_stream_to_table(response.content)
        return elapsed, len(response.content), table

    def run(
        self,
        params: Optional[Dict[str, Any]] = None,
        iterations: int = 10,
        warmup: int = 2,
    ) -> Dict[str, Any]:
        """
        Run benchmark comparing Arrow vs JSON.

        Args:
            params: Query parameters to pass to the endpoint
            iterations: Number of test iterations
            warmup: Number of warmup iterations (not counted)

        Returns:
            Dict containing benchmark results
        """
        # Warmup
        for _ in range(warmup):
            try:
                self._fetch_json(params)
                self._fetch_arrow(params)
            except Exception:
                pass  # Ignore warmup errors

        json_times = []
        json_sizes = []
        arrow_times = []
        arrow_sizes = []

        for _ in range(iterations):
            # Fetch JSON
            try:
                t, size, _ = self._fetch_json(params)
                json_times.append(t)
                json_sizes.append(size)
            except Exception as e:
                json_times.append(float('inf'))
                json_sizes.append(0)

            # Fetch Arrow
            try:
                t, size, _ = self._fetch_arrow(params)
                arrow_times.append(t)
                arrow_sizes.append(size)
            except Exception as e:
                arrow_times.append(float('inf'))
                arrow_sizes.append(0)

        # Calculate statistics
        def stats(values):
            valid = [v for v in values if v != float('inf')]
            if not valid:
                return {"min": None, "max": None, "avg": None}
            return {
                "min": min(valid),
                "max": max(valid),
                "avg": sum(valid) / len(valid),
            }

        json_time_stats = stats(json_times)
        arrow_time_stats = stats(arrow_times)

        # Calculate speedup (if both have valid averages)
        speedup = None
        if json_time_stats["avg"] and arrow_time_stats["avg"]:
            speedup = json_time_stats["avg"] / arrow_time_stats["avg"]

        return {
            "endpoint": self.endpoint,
            "iterations": iterations,
            "json": {
                "time": json_time_stats,
                "avg_size_bytes": sum(json_sizes) / len(json_sizes) if json_sizes else 0,
            },
            "arrow": {
                "time": arrow_time_stats,
                "avg_size_bytes": sum(arrow_sizes) / len(arrow_sizes) if arrow_sizes else 0,
            },
            "speedup": speedup,  # Arrow speedup vs JSON (higher is better for Arrow)
        }


def request_with_arrow_accept(
    url: str,
    method: str = "GET",
    params: Optional[Dict[str, Any]] = None,
    auth: Optional[Tuple[str, str]] = None,
    **kwargs
) -> requests.Response:
    """
    Make an HTTP request with Arrow Accept header.

    Args:
        url: Full URL to request
        method: HTTP method
        params: Query parameters
        **kwargs: Additional arguments passed to requests

    Returns:
        HTTP Response object
    """
    headers = kwargs.pop("headers", {})
    headers["Accept"] = ARROW_STREAM_MEDIA_TYPE
    return requests.request(
        method,
        url,
        headers=headers,
        params=params,
        auth=auth,
        **kwargs,
    )


# Pytest fixtures (can be imported in conftest.py)
def arrow_response_fixture():
    """Factory fixture for creating Arrow test responses."""
    def _make_response(columns: Dict[str, List], metadata: Optional[Dict] = None) -> bytes:
        """
        Create an Arrow IPC stream from column data.

        Args:
            columns: Dict mapping column names to lists of values
            metadata: Optional schema metadata

        Returns:
            Bytes containing Arrow IPC stream
        """
        arrays = {name: pa.array(values) for name, values in columns.items()}
        table = pa.Table.from_pydict(arrays)

        if metadata:
            table = table.replace_schema_metadata(metadata)

        sink = io.BytesIO()
        with ipc.new_stream(sink, table.schema) as writer:
            writer.write_table(table)

        return sink.getvalue()

    return _make_response


def assert_valid_arrow_stream(data: bytes) -> pa.Table:
    """
    Assert that data is a valid Arrow IPC stream.

    Args:
        data: Bytes to validate

    Returns:
        Arrow Table if valid

    Raises:
        AssertionError: If data is not valid Arrow IPC stream
    """
    try:
        return read_arrow_stream_to_table(data)
    except Exception as e:
        raise AssertionError(f"Invalid Arrow IPC stream: {e}")
