"""
Tests for Arrow helper utilities.

These tests verify that the arrow_helpers module works correctly
with pyarrow for creating, reading, and validating Arrow IPC streams.
"""

import pytest
import io
import pyarrow as pa
import pyarrow.ipc as ipc

from arrow_helpers import (
    ARROW_STREAM_MEDIA_TYPE,
    read_arrow_stream,
    read_arrow_stream_to_table,
    compare_arrow_to_json,
    arrow_response_fixture,
    assert_valid_arrow_stream,
)


class TestArrowStreamReading:
    """Tests for reading Arrow IPC streams."""

    def test_read_simple_stream(self):
        """Can read a simple Arrow IPC stream with primitive types."""
        # Create a test table
        table = pa.Table.from_pydict({
            "id": [1, 2, 3],
            "name": ["Alice", "Bob", "Charlie"],
            "value": [1.5, 2.5, 3.5],
        })

        # Serialize to IPC stream
        sink = io.BytesIO()
        with ipc.new_stream(sink, table.schema) as writer:
            writer.write_table(table)

        # Read it back
        result = read_arrow_stream_to_table(sink.getvalue())

        assert result.num_rows == 3
        assert result.column_names == ["id", "name", "value"]
        assert result["id"].to_pylist() == [1, 2, 3]
        assert result["name"].to_pylist() == ["Alice", "Bob", "Charlie"]

    def test_read_stream_with_nulls(self):
        """Can read Arrow IPC stream containing null values."""
        table = pa.Table.from_pydict({
            "id": [1, 2, 3],
            "nullable_value": [10, None, 30],
        })

        sink = io.BytesIO()
        with ipc.new_stream(sink, table.schema) as writer:
            writer.write_table(table)

        result = read_arrow_stream_to_table(sink.getvalue())

        assert result.num_rows == 3
        assert result["nullable_value"].to_pylist() == [10, None, 30]

    def test_read_empty_stream(self):
        """Can read an empty Arrow IPC stream (schema only)."""
        schema = pa.schema([
            ("id", pa.int64()),
            ("name", pa.string()),
        ])
        table = pa.Table.from_pydict({"id": [], "name": []}, schema=schema)

        sink = io.BytesIO()
        with ipc.new_stream(sink, table.schema) as writer:
            writer.write_table(table)

        result = read_arrow_stream_to_table(sink.getvalue())

        assert result.num_rows == 0
        assert result.column_names == ["id", "name"]

    def test_read_invalid_stream_raises(self):
        """Reading invalid data raises ArrowInvalid."""
        invalid_data = b"this is not valid arrow data"

        with pytest.raises(pa.ArrowInvalid):
            read_arrow_stream_to_table(invalid_data)


class TestArrowJsonComparison:
    """Tests for comparing Arrow data with JSON."""

    def test_compare_matching_data(self):
        """Identical data should compare equal."""
        table = pa.Table.from_pydict({
            "id": [1, 2],
            "name": ["Alice", "Bob"],
        })

        sink = io.BytesIO()
        with ipc.new_stream(sink, table.schema) as writer:
            writer.write_table(table)

        json_data = [
            {"id": 1, "name": "Alice"},
            {"id": 2, "name": "Bob"},
        ]

        is_equal, diff = compare_arrow_to_json(sink.getvalue(), json_data)
        assert is_equal, f"Data should match: {diff}"

    def test_compare_different_row_count(self):
        """Different row counts should not compare equal."""
        table = pa.Table.from_pydict({
            "id": [1, 2, 3],
        })

        sink = io.BytesIO()
        with ipc.new_stream(sink, table.schema) as writer:
            writer.write_table(table)

        json_data = [{"id": 1}, {"id": 2}]

        is_equal, diff = compare_arrow_to_json(sink.getvalue(), json_data)
        assert not is_equal
        assert "Row count differs" in diff

    def test_compare_with_nulls(self):
        """Null values should be handled correctly in comparison."""
        table = pa.Table.from_pydict({
            "id": [1, 2],
            "value": [10, None],
        })

        sink = io.BytesIO()
        with ipc.new_stream(sink, table.schema) as writer:
            writer.write_table(table)

        json_data = [
            {"id": 1, "value": 10},
            {"id": 2, "value": None},
        ]

        is_equal, diff = compare_arrow_to_json(sink.getvalue(), json_data)
        assert is_equal, f"Data with nulls should match: {diff}"

    def test_compare_ignore_columns(self):
        """Can ignore specific columns in comparison."""
        table = pa.Table.from_pydict({
            "id": [1, 2],
            "timestamp": ["2024-01-01", "2024-01-02"],  # Different
        })

        sink = io.BytesIO()
        with ipc.new_stream(sink, table.schema) as writer:
            writer.write_table(table)

        json_data = [
            {"id": 1, "timestamp": "different"},
            {"id": 2, "timestamp": "values"},
        ]

        # Without ignoring, should differ
        is_equal, _ = compare_arrow_to_json(sink.getvalue(), json_data)
        assert not is_equal

        # With ignoring timestamp, should match
        is_equal, diff = compare_arrow_to_json(
            sink.getvalue(), json_data, ignore_columns=["timestamp"]
        )
        assert is_equal, f"Should match when ignoring timestamp: {diff}"


class TestArrowResponseFixture:
    """Tests for the arrow_response_fixture helper."""

    def test_create_simple_response(self):
        """Can create a simple Arrow IPC stream from columns."""
        make_response = arrow_response_fixture()

        data = make_response({
            "id": [1, 2, 3],
            "name": ["a", "b", "c"],
        })

        # Should be valid Arrow stream
        table = assert_valid_arrow_stream(data)
        assert table.num_rows == 3
        assert table.column_names == ["id", "name"]

    def test_create_response_with_metadata(self):
        """Can create Arrow IPC stream with schema metadata."""
        make_response = arrow_response_fixture()

        data = make_response(
            {"id": [1, 2]},
            metadata={"source": "test"}
        )

        table = assert_valid_arrow_stream(data)
        assert table.schema.metadata is not None
        assert b"source" in table.schema.metadata


class TestAssertValidArrowStream:
    """Tests for the assertion helper."""

    def test_valid_stream_passes(self):
        """Valid Arrow stream passes assertion."""
        table = pa.Table.from_pydict({"x": [1, 2, 3]})
        sink = io.BytesIO()
        with ipc.new_stream(sink, table.schema) as writer:
            writer.write_table(table)

        # Should not raise
        result = assert_valid_arrow_stream(sink.getvalue())
        assert result.num_rows == 3

    def test_invalid_stream_fails(self):
        """Invalid data fails assertion."""
        with pytest.raises(AssertionError, match="Invalid Arrow IPC stream"):
            assert_valid_arrow_stream(b"not arrow data")


class TestArrowMediaType:
    """Tests for media type constants."""

    def test_stream_media_type(self):
        """Stream media type is correct."""
        assert ARROW_STREAM_MEDIA_TYPE == "application/vnd.apache.arrow.stream"
