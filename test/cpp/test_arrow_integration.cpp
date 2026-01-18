#include <catch2/catch_test_macros.hpp>
#include <nanoarrow/nanoarrow.h>
#include <nanoarrow/nanoarrow_ipc.h>
#include <cstring>

// Test that nanoarrow headers are usable and basic types work
TEST_CASE("Arrow Integration - nanoarrow headers", "[arrow]") {
    SECTION("Can create and release ArrowSchema") {
        ArrowSchema schema;
        ArrowSchemaInit(&schema);

        // Set schema to int64 type
        REQUIRE(ArrowSchemaSetType(&schema, NANOARROW_TYPE_INT64) == NANOARROW_OK);
        REQUIRE(schema.format != nullptr);
        REQUIRE(std::string(schema.format) == "l"); // 'l' is Arrow format for int64

        ArrowSchemaRelease(&schema);
    }

    SECTION("Can create and release ArrowArray") {
        ArrowArray array;
        REQUIRE(ArrowArrayInitFromType(&array, NANOARROW_TYPE_INT64) == NANOARROW_OK);

        // Append some values
        REQUIRE(ArrowArrayStartAppending(&array) == NANOARROW_OK);
        REQUIRE(ArrowArrayAppendInt(&array, 42) == NANOARROW_OK);
        REQUIRE(ArrowArrayAppendInt(&array, 100) == NANOARROW_OK);
        REQUIRE(ArrowArrayFinishBuildingDefault(&array, nullptr) == NANOARROW_OK);

        REQUIRE(array.length == 2);

        ArrowArrayRelease(&array);
    }

    SECTION("Can create ArrowBuffer for IPC") {
        ArrowBuffer buffer;
        ArrowBufferInit(&buffer);

        // Append some bytes
        const char* data = "test data";
        REQUIRE(ArrowBufferAppend(&buffer, data, std::strlen(data)) == NANOARROW_OK);
        REQUIRE(static_cast<size_t>(buffer.size_bytes) == std::strlen(data));

        ArrowBufferReset(&buffer);
    }
}

TEST_CASE("Arrow Integration - IPC encoder availability", "[arrow][ipc]") {
    SECTION("IPC output stream can be created") {
        ArrowBuffer output;
        ArrowBufferInit(&output);

        // Create an output stream that writes to a buffer
        ArrowIpcOutputStream stream;
        REQUIRE(ArrowIpcOutputStreamInitBuffer(&stream, &output) == NANOARROW_OK);

        // Clean up
        stream.release(&stream);
        ArrowBufferReset(&output);
    }

    SECTION("IPC writer can be initialized with output stream") {
        ArrowBuffer output;
        ArrowBufferInit(&output);

        // Create an output stream
        ArrowIpcOutputStream stream;
        REQUIRE(ArrowIpcOutputStreamInitBuffer(&stream, &output) == NANOARROW_OK);

        // Initialize the writer (takes ownership of stream)
        ArrowIpcWriter writer;
        ArrowErrorCode result = ArrowIpcWriterInit(&writer, &stream);
        REQUIRE(result == NANOARROW_OK);

        // Clean up
        ArrowIpcWriterReset(&writer);
        ArrowBufferReset(&output);
    }
}
