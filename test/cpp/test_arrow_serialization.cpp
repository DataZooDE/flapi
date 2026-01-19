#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include "arrow_serializer.hpp"

// =============================================================================
// TESTS - Define expected behavior for Arrow serialization
// =============================================================================

using namespace flapi;

// Helper to create a simple in-memory DuckDB database and run a query
class DuckDBTestFixture {
public:
    duckdb_database db = nullptr;
    duckdb_connection conn = nullptr;
    duckdb_result result;
    bool has_result = false;

    void SetUp() {
        REQUIRE(duckdb_open(nullptr, &db) == DuckDBSuccess);
        REQUIRE(duckdb_connect(db, &conn) == DuckDBSuccess);
    }

    void TearDown() {
        if (has_result) {
            duckdb_destroy_result(&result);
            has_result = false;
        }
        if (conn) {
            duckdb_disconnect(&conn);
            conn = nullptr;
        }
        if (db) {
            duckdb_close(&db);
            db = nullptr;
        }
    }

    void executeQuery(const char* sql) {
        if (has_result) {
            duckdb_destroy_result(&result);
        }
        REQUIRE(duckdb_query(conn, sql, &result) == DuckDBSuccess);
        has_result = true;
    }

public:
    DuckDBTestFixture() { SetUp(); }
    ~DuckDBTestFixture() { TearDown(); }
};

TEST_CASE("Arrow Schema Extraction - Basic Types", "[arrow-serialization][schema]") {
    DuckDBTestFixture fixture;

    SECTION("Extract schema with integer column") {
        fixture.executeQuery("SELECT 42 AS id");

        ArrowSchema schema;
        ArrowError error;
        ArrowErrorCode code = extractSchemaFromDuckDB(fixture.result, &schema, &error);

        REQUIRE(code == NANOARROW_OK);
        REQUIRE(schema.n_children == 1);
        CHECK(std::string(schema.children[0]->name) == "id");
        // Integer should map to int64 or int32
        CHECK((std::string(schema.children[0]->format) == "l" ||  // int64
               std::string(schema.children[0]->format) == "i"));  // int32

        ArrowSchemaRelease(&schema);
    }

    SECTION("Extract schema with string column") {
        fixture.executeQuery("SELECT 'hello' AS name");

        ArrowSchema schema;
        ArrowError error;
        ArrowErrorCode code = extractSchemaFromDuckDB(fixture.result, &schema, &error);

        REQUIRE(code == NANOARROW_OK);
        REQUIRE(schema.n_children == 1);
        CHECK(std::string(schema.children[0]->name) == "name");
        CHECK(std::string(schema.children[0]->format) == "u");  // utf8 string

        ArrowSchemaRelease(&schema);
    }

    SECTION("Extract schema with multiple columns") {
        fixture.executeQuery("SELECT 1 AS id, 'test' AS name, 3.14 AS value");

        ArrowSchema schema;
        ArrowError error;
        ArrowErrorCode code = extractSchemaFromDuckDB(fixture.result, &schema, &error);

        REQUIRE(code == NANOARROW_OK);
        REQUIRE(schema.n_children == 3);
        CHECK(std::string(schema.children[0]->name) == "id");
        CHECK(std::string(schema.children[1]->name) == "name");
        CHECK(std::string(schema.children[2]->name) == "value");

        ArrowSchemaRelease(&schema);
    }

    SECTION("Extract schema with boolean column") {
        fixture.executeQuery("SELECT true AS flag");

        ArrowSchema schema;
        ArrowError error;
        ArrowErrorCode code = extractSchemaFromDuckDB(fixture.result, &schema, &error);

        REQUIRE(code == NANOARROW_OK);
        CHECK(std::string(schema.children[0]->format) == "b");  // boolean

        ArrowSchemaRelease(&schema);
    }

    SECTION("Extract schema with date column") {
        fixture.executeQuery("SELECT DATE '2024-01-15' AS dt");

        ArrowSchema schema;
        ArrowError error;
        ArrowErrorCode code = extractSchemaFromDuckDB(fixture.result, &schema, &error);

        REQUIRE(code == NANOARROW_OK);
        // Date should be tdD (date32) or tdm (date64)
        std::string fmt = schema.children[0]->format;
        CHECK((fmt == "tdD" || fmt == "tdm"));

        ArrowSchemaRelease(&schema);
    }

    SECTION("Extract schema with timestamp column") {
        fixture.executeQuery("SELECT TIMESTAMP '2024-01-15 10:30:00' AS ts");

        ArrowSchema schema;
        ArrowError error;
        ArrowErrorCode code = extractSchemaFromDuckDB(fixture.result, &schema, &error);

        REQUIRE(code == NANOARROW_OK);
        // Timestamp format starts with 'ts'
        std::string fmt = schema.children[0]->format;
        CHECK(fmt.substr(0, 2) == "ts");

        ArrowSchemaRelease(&schema);
    }
}

TEST_CASE("Arrow Schema Extraction - Complex Types", "[arrow-serialization][schema]") {
    DuckDBTestFixture fixture;

    SECTION("Extract schema with decimal column") {
        fixture.executeQuery("SELECT CAST(123.45 AS DECIMAL(10,2)) AS amount");

        ArrowSchema schema;
        ArrowError error;
        ArrowErrorCode code = extractSchemaFromDuckDB(fixture.result, &schema, &error);

        REQUIRE(code == NANOARROW_OK);
        // Decimal maps to string for now (future: native decimal support)
        std::string fmt = schema.children[0]->format;
        CHECK((fmt[0] == 'd' || fmt == "u"));  // decimal or string fallback

        ArrowSchemaRelease(&schema);
    }

    SECTION("Extract schema with nullable column") {
        fixture.executeQuery("SELECT NULL AS nullable_col");

        ArrowSchema schema;
        ArrowError error;
        ArrowErrorCode code = extractSchemaFromDuckDB(fixture.result, &schema, &error);

        REQUIRE(code == NANOARROW_OK);
        // Should handle NULL values
        CHECK(schema.n_children == 1);

        ArrowSchemaRelease(&schema);
    }
}

TEST_CASE("Arrow Data Conversion - Record Batches", "[arrow-serialization][conversion]") {
    DuckDBTestFixture fixture;

    SECTION("Convert simple integer data") {
        fixture.executeQuery("SELECT * FROM (VALUES (1), (2), (3)) AS t(id)");

        ArrowSchema schema;
        ArrowError error;
        REQUIRE(extractSchemaFromDuckDB(fixture.result, &schema, &error) == NANOARROW_OK);

        duckdb_data_chunk chunk = duckdb_fetch_chunk(fixture.result);
        REQUIRE(chunk != nullptr);

        ArrowArray array;
        ArrowErrorCode code = convertChunkToArrow(chunk, &schema, &array, &error);

        REQUIRE(code == NANOARROW_OK);
        CHECK(array.length == 3);
        CHECK(array.n_children == 1);

        ArrowArrayRelease(&array);
        ArrowSchemaRelease(&schema);
        duckdb_destroy_data_chunk(&chunk);
    }

    SECTION("Convert data with NULL values") {
        fixture.executeQuery("SELECT * FROM (VALUES (1), (NULL), (3)) AS t(id)");

        ArrowSchema schema;
        ArrowError error;
        REQUIRE(extractSchemaFromDuckDB(fixture.result, &schema, &error) == NANOARROW_OK);

        duckdb_data_chunk chunk = duckdb_fetch_chunk(fixture.result);
        REQUIRE(chunk != nullptr);

        ArrowArray array;
        ArrowErrorCode code = convertChunkToArrow(chunk, &schema, &array, &error);

        REQUIRE(code == NANOARROW_OK);
        CHECK(array.length == 3);
        // NULL count is tracked per column (child), not on the struct
        REQUIRE(array.n_children == 1);
        CHECK(array.children[0]->null_count == 1);  // One NULL value in id column

        ArrowArrayRelease(&array);
        ArrowSchemaRelease(&schema);
        duckdb_destroy_data_chunk(&chunk);
    }

    SECTION("Convert string data") {
        fixture.executeQuery("SELECT * FROM (VALUES ('hello'), ('world')) AS t(name)");

        ArrowSchema schema;
        ArrowError error;
        REQUIRE(extractSchemaFromDuckDB(fixture.result, &schema, &error) == NANOARROW_OK);

        duckdb_data_chunk chunk = duckdb_fetch_chunk(fixture.result);
        REQUIRE(chunk != nullptr);

        ArrowArray array;
        ArrowErrorCode code = convertChunkToArrow(chunk, &schema, &array, &error);

        REQUIRE(code == NANOARROW_OK);
        CHECK(array.length == 2);

        ArrowArrayRelease(&array);
        ArrowSchemaRelease(&schema);
        duckdb_destroy_data_chunk(&chunk);
    }

    SECTION("Convert mixed type columns") {
        fixture.executeQuery("SELECT 1 AS id, 'test' AS name, 3.14 AS value");

        ArrowSchema schema;
        ArrowError error;
        REQUIRE(extractSchemaFromDuckDB(fixture.result, &schema, &error) == NANOARROW_OK);

        duckdb_data_chunk chunk = duckdb_fetch_chunk(fixture.result);
        REQUIRE(chunk != nullptr);

        ArrowArray array;
        ArrowErrorCode code = convertChunkToArrow(chunk, &schema, &array, &error);

        REQUIRE(code == NANOARROW_OK);
        CHECK(array.length == 1);
        CHECK(array.n_children == 3);

        ArrowArrayRelease(&array);
        ArrowSchemaRelease(&schema);
        duckdb_destroy_data_chunk(&chunk);
    }
}

TEST_CASE("Arrow IPC Serialization - Full Pipeline", "[arrow-serialization][ipc]") {
    DuckDBTestFixture fixture;
    ArrowSerializerConfig config;

    SECTION("Serialize simple query result") {
        fixture.executeQuery("SELECT * FROM (VALUES (1, 'a'), (2, 'b'), (3, 'c')) AS t(id, name)");

        auto result = serializeToArrowIPC(fixture.result, config);

        REQUIRE(result.success);
        CHECK(result.rowCount == 3);
        CHECK(result.batchCount >= 1);
        CHECK(result.bytesWritten > 0);
        CHECK(!result.data.empty());
    }

    SECTION("Serialize empty result") {
        fixture.executeQuery("SELECT 1 AS id WHERE false");

        auto result = serializeToArrowIPC(fixture.result, config);

        REQUIRE(result.success);
        CHECK(result.rowCount == 0);
        // Should still have schema message
        CHECK(!result.data.empty());
    }

    SECTION("Serialize with batch size") {
        // Create larger dataset
        fixture.executeQuery(
            "SELECT i AS id, 'name_' || i AS name "
            "FROM generate_series(1, 100) AS t(i)"
        );

        config.batchSize = 10;  // Batch size config (not yet implemented)
        auto result = serializeToArrowIPC(fixture.result, config);

        REQUIRE(result.success);
        CHECK(result.rowCount == 100);
        // DuckDB returns data in chunks (default 2048 rows), so batch count
        // depends on DuckDB's chunking, not our config.batchSize yet
        CHECK(result.batchCount >= 1);
    }

    SECTION("Serialize various data types") {
        fixture.executeQuery(
            "SELECT "
            "  1 AS int_col, "
            "  'hello' AS str_col, "
            "  3.14 AS float_col, "
            "  true AS bool_col, "
            "  DATE '2024-01-15' AS date_col"
        );

        auto result = serializeToArrowIPC(fixture.result, config);

        REQUIRE(result.success);
        CHECK(result.rowCount == 1);
        CHECK(!result.data.empty());
    }
}

TEST_CASE("Arrow IPC Serialization - Compression", "[arrow-serialization][compression]") {
    DuckDBTestFixture fixture;

    // Create compressible data (repeated patterns)
    fixture.executeQuery(
        "SELECT i % 10 AS category, 'repeated_string_value' AS name "
        "FROM generate_series(1, 1000) AS t(i)"
    );

    SECTION("Serialize without compression") {
        ArrowSerializerConfig config;
        config.codec = "";  // No compression

        auto result = serializeToArrowIPC(fixture.result, config);

        REQUIRE(result.success);
        CHECK(result.bytesWritten > 0);
    }

    SECTION("Serialize with LZ4 compression") {
        ArrowSerializerConfig config;
        config.codec = "lz4";

        auto result = serializeToArrowIPC(fixture.result, config);

        REQUIRE(result.success);
        CHECK(result.bytesWritten > 0);
        // Compressed should be smaller (though not guaranteed for small data)
    }

    SECTION("Serialize with ZSTD compression") {
        ArrowSerializerConfig config;
        config.codec = "zstd";

        auto result = serializeToArrowIPC(fixture.result, config);

        REQUIRE(result.success);
        CHECK(result.bytesWritten > 0);
    }
}

TEST_CASE("Arrow Serialization - Memory Limits", "[arrow-serialization][memory]") {
    DuckDBTestFixture fixture;

    SECTION("Respect memory limit") {
        // Create dataset that might exceed memory limit
        fixture.executeQuery(
            "SELECT i AS id, repeat('x', 1000) AS data "
            "FROM generate_series(1, 1000) AS t(i)"
        );

        ArrowSerializerConfig config;
        config.maxMemoryBytes = 1024;  // Very small limit (1 KB)

        auto result = serializeToArrowIPC(fixture.result, config);

        // Should either succeed with bounded memory or fail gracefully
        if (!result.success) {
            CHECK(!result.errorMessage.empty());
            CHECK(result.errorMessage.find("memory") != std::string::npos);
        }
    }
}

TEST_CASE("Arrow Type Mapping - DuckDB to Arrow", "[arrow-serialization][type-mapping]") {
    SECTION("Common types are supported") {
        CHECK(isDuckDBTypeSupported(DUCKDB_TYPE_BOOLEAN));
        CHECK(isDuckDBTypeSupported(DUCKDB_TYPE_TINYINT));
        CHECK(isDuckDBTypeSupported(DUCKDB_TYPE_SMALLINT));
        CHECK(isDuckDBTypeSupported(DUCKDB_TYPE_INTEGER));
        CHECK(isDuckDBTypeSupported(DUCKDB_TYPE_BIGINT));
        CHECK(isDuckDBTypeSupported(DUCKDB_TYPE_FLOAT));
        CHECK(isDuckDBTypeSupported(DUCKDB_TYPE_DOUBLE));
        CHECK(isDuckDBTypeSupported(DUCKDB_TYPE_VARCHAR));
        CHECK(isDuckDBTypeSupported(DUCKDB_TYPE_DATE));
        CHECK(isDuckDBTypeSupported(DUCKDB_TYPE_TIMESTAMP));
    }

    SECTION("Arrow format strings are correct") {
        CHECK(std::string(duckdbTypeToArrowFormat(DUCKDB_TYPE_BOOLEAN)) == "b");
        CHECK(std::string(duckdbTypeToArrowFormat(DUCKDB_TYPE_TINYINT)) == "c");
        CHECK(std::string(duckdbTypeToArrowFormat(DUCKDB_TYPE_SMALLINT)) == "s");
        CHECK(std::string(duckdbTypeToArrowFormat(DUCKDB_TYPE_INTEGER)) == "i");
        CHECK(std::string(duckdbTypeToArrowFormat(DUCKDB_TYPE_BIGINT)) == "l");
        CHECK(std::string(duckdbTypeToArrowFormat(DUCKDB_TYPE_FLOAT)) == "f");
        CHECK(std::string(duckdbTypeToArrowFormat(DUCKDB_TYPE_DOUBLE)) == "g");
        CHECK(std::string(duckdbTypeToArrowFormat(DUCKDB_TYPE_VARCHAR)) == "u");
    }
}

TEST_CASE("Arrow Serialization - Error Handling", "[arrow-serialization][errors]") {
    SECTION("Handle unsupported type gracefully") {
        // This tests that unsupported types don't crash
        // Implementation should either skip or convert to string
        DuckDBTestFixture fixture;

        // UUID might not be directly supported
        fixture.executeQuery("SELECT uuid() AS id");

        ArrowSerializerConfig config;
        auto result = serializeToArrowIPC(fixture.result, config);

        // Should succeed with fallback or fail with clear error
        if (!result.success) {
            CHECK(!result.errorMessage.empty());
        }
    }
}

// Implementation is provided by arrow_serializer.hpp
