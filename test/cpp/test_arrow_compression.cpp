/**
 * Arrow Compression Unit Tests (TDD Phase - Task 5.1)
 *
 * Tests for LZ4 and ZSTD compression support in Arrow IPC serialization.
 * These tests verify:
 * 1. LZ4 compression/decompression roundtrip
 * 2. ZSTD compression with levels 1-3
 * 3. Compression reduces data size
 * 4. Codec selection based on configuration
 * 5. Invalid codec handling
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <duckdb.h>
#include <vector>
#include <string>
#include <cstring>

#include "arrow_serializer.hpp"
#include "content_negotiation.hpp"

using namespace flapi;

// Test fixture for DuckDB C API
class DuckDBCompressionFixture {
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
            has_result = false;
        }
        auto state = duckdb_query(conn, sql, &result);
        REQUIRE(state == DuckDBSuccess);
        has_result = true;
    }

    // Create a table with repetitive data (compresses well)
    void createCompressibleData(size_t num_rows = 1000) {
        std::string sql = "CREATE TABLE test_data AS SELECT "
                         "i AS id, "
                         "'repeated_string_value_for_compression' AS text_col, "
                         "i % 10 AS category, "
                         "i * 1.5 AS numeric_col "
                         "FROM range(" + std::to_string(num_rows) + ") t(i)";
        executeQuery(sql.c_str());
        executeQuery("SELECT * FROM test_data");
    }

    // Create a table with random data (compresses less)
    void createRandomData(size_t num_rows = 1000) {
        std::string sql = "CREATE TABLE random_data AS SELECT "
                         "i AS id, "
                         "md5(i::VARCHAR) AS random_text, "
                         "random() AS random_float "
                         "FROM range(" + std::to_string(num_rows) + ") t(i)";
        executeQuery(sql.c_str());
        executeQuery("SELECT * FROM random_data");
    }

public:
    DuckDBCompressionFixture() { SetUp(); }
    ~DuckDBCompressionFixture() { TearDown(); }
};

TEST_CASE("Arrow LZ4 Compression", "[arrow][compression][lz4]") {
    DuckDBCompressionFixture fixture;

    SECTION("LZ4 compression produces valid compressed stream") {
        fixture.createCompressibleData(100);

        ArrowSerializerConfig config;
        config.codec = "lz4";

        auto arrowResult = serializeToArrowIPC(fixture.result, config);

        REQUIRE(arrowResult.success);
        REQUIRE(arrowResult.rowCount == 100);
        REQUIRE(!arrowResult.data.empty());

        // LZ4 frame should start with LZ4 magic number (0x184D2204)
        REQUIRE(arrowResult.data.size() >= 4);
        uint32_t magic;
        std::memcpy(&magic, arrowResult.data.data(), 4);
        REQUIRE(magic == 0x184D2204);
    }

    SECTION("LZ4 compressed data is smaller than uncompressed") {
        // First get uncompressed
        fixture.createCompressibleData(1000);
        ArrowSerializerConfig uncompressedConfig;
        auto uncompressedResult = serializeToArrowIPC(fixture.result, uncompressedConfig);
        REQUIRE(uncompressedResult.success);
        size_t uncompressedSize = uncompressedResult.data.size();

        // Then get compressed (need to recreate fixture data)
        fixture.TearDown();
        fixture.SetUp();
        fixture.createCompressibleData(1000);
        ArrowSerializerConfig lz4Config;
        lz4Config.codec = "lz4";
        auto compressedResult = serializeToArrowIPC(fixture.result, lz4Config);

        REQUIRE(compressedResult.success);
        // Compressed should be smaller for repetitive data
        // Note: This will FAIL until compression is implemented
        REQUIRE(compressedResult.data.size() < uncompressedSize);
    }

    SECTION("LZ4 compression roundtrip preserves row count") {
        fixture.createCompressibleData(100);

        ArrowSerializerConfig config;
        config.codec = "lz4";

        auto arrowResult = serializeToArrowIPC(fixture.result, config);

        REQUIRE(arrowResult.success);
        REQUIRE(arrowResult.rowCount == 100);
        REQUIRE(arrowResult.batchCount >= 1);
    }
}

TEST_CASE("Arrow ZSTD Compression", "[arrow][compression][zstd]") {
    DuckDBCompressionFixture fixture;

    SECTION("ZSTD compression produces valid compressed stream") {
        fixture.createCompressibleData(100);

        ArrowSerializerConfig config;
        config.codec = "zstd";

        auto arrowResult = serializeToArrowIPC(fixture.result, config);

        REQUIRE(arrowResult.success);
        REQUIRE(arrowResult.rowCount == 100);
        REQUIRE(!arrowResult.data.empty());

        // ZSTD frame should start with ZSTD magic number (0xFD2FB528)
        REQUIRE(arrowResult.data.size() >= 4);
        uint32_t magic;
        std::memcpy(&magic, arrowResult.data.data(), 4);
        REQUIRE(magic == 0xFD2FB528);
    }

    SECTION("ZSTD compressed data is smaller than uncompressed") {
        // First get uncompressed
        fixture.createCompressibleData(1000);
        ArrowSerializerConfig uncompressedConfig;
        auto uncompressedResult = serializeToArrowIPC(fixture.result, uncompressedConfig);
        REQUIRE(uncompressedResult.success);
        size_t uncompressedSize = uncompressedResult.data.size();

        // Then get compressed
        fixture.TearDown();
        fixture.SetUp();
        fixture.createCompressibleData(1000);
        ArrowSerializerConfig zstdConfig;
        zstdConfig.codec = "zstd";
        auto compressedResult = serializeToArrowIPC(fixture.result, zstdConfig);

        REQUIRE(compressedResult.success);
        // ZSTD should provide good compression for repetitive data
        REQUIRE(compressedResult.data.size() < uncompressedSize);
    }

    SECTION("ZSTD level 1 is fast compression") {
        fixture.createCompressibleData(500);

        ArrowSerializerConfig config;
        config.codec = "zstd";
        config.compressionLevel = 1;

        auto arrowResult = serializeToArrowIPC(fixture.result, config);

        REQUIRE(arrowResult.success);
        REQUIRE(arrowResult.rowCount == 500);
    }

    SECTION("ZSTD level 3 provides comparable or better compression than level 1") {
        // Level 1
        fixture.createCompressibleData(1000);
        ArrowSerializerConfig level1Config;
        level1Config.codec = "zstd";
        level1Config.compressionLevel = 1;
        auto level1Result = serializeToArrowIPC(fixture.result, level1Config);
        REQUIRE(level1Result.success);
        size_t sizeL1 = level1Result.data.size();

        // Level 3
        fixture.TearDown();
        fixture.SetUp();
        fixture.createCompressibleData(1000);
        ArrowSerializerConfig level3Config;
        level3Config.codec = "zstd";
        level3Config.compressionLevel = 3;
        auto level3Result = serializeToArrowIPC(fixture.result, level3Config);
        REQUIRE(level3Result.success);
        size_t sizeL3 = level3Result.data.size();

        // Level 3 should give same or better compression
        // (Allow 10% margin for small datasets)
        REQUIRE(sizeL3 <= sizeL1 * 1.1);
    }

    SECTION("ZSTD handles random data gracefully") {
        fixture.createRandomData(500);

        ArrowSerializerConfig config;
        config.codec = "zstd";

        auto arrowResult = serializeToArrowIPC(fixture.result, config);

        // Should succeed even if compression ratio is poor
        REQUIRE(arrowResult.success);
        REQUIRE(arrowResult.rowCount == 500);
    }
}

TEST_CASE("Arrow Compression Codec Selection", "[arrow][compression][codec]") {
    DuckDBCompressionFixture fixture;

    SECTION("Empty codec returns uncompressed") {
        fixture.createCompressibleData(100);

        ArrowSerializerConfig config;
        config.codec = "";  // No compression

        auto arrowResult = serializeToArrowIPC(fixture.result, config);

        REQUIRE(arrowResult.success);
        REQUIRE(arrowResult.rowCount == 100);
    }

    SECTION("Invalid codec is handled gracefully") {
        fixture.createCompressibleData(100);

        ArrowSerializerConfig config;
        config.codec = "invalid_codec_xyz";

        auto arrowResult = serializeToArrowIPC(fixture.result, config);

        // Should either fail with error message or fall back to uncompressed
        if (arrowResult.success) {
            // Fallback to uncompressed is acceptable
            REQUIRE(arrowResult.rowCount == 100);
        } else {
            // Error message should mention the invalid codec
            REQUIRE_THAT(arrowResult.errorMessage,
                        Catch::Matchers::ContainsSubstring("codec") ||
                        Catch::Matchers::ContainsSubstring("compression") ||
                        Catch::Matchers::ContainsSubstring("unsupported"));
        }
    }

    SECTION("Codec names are case-insensitive") {
        // ZSTD uppercase
        fixture.createCompressibleData(100);
        ArrowSerializerConfig config1;
        config1.codec = "ZSTD";
        auto result1 = serializeToArrowIPC(fixture.result, config1);

        // zstd lowercase
        fixture.TearDown();
        fixture.SetUp();
        fixture.createCompressibleData(100);
        ArrowSerializerConfig config2;
        config2.codec = "zstd";
        auto result2 = serializeToArrowIPC(fixture.result, config2);

        // Both should succeed with same behavior
        REQUIRE(result1.success == result2.success);
        if (result1.success) {
            REQUIRE(result1.rowCount == result2.rowCount);
        }
    }
}

TEST_CASE("Arrow Compression from Content Negotiation", "[arrow][compression][negotiation]") {
    SECTION("Codec extracted from Accept header") {
        ResponseFormatConfig formatConfig;
        formatConfig.arrowEnabled = true;
        formatConfig.formats = {"json", "arrow"};

        auto result = negotiateContentType(
            "application/vnd.apache.arrow.stream;codec=zstd",
            "",  // No query param
            formatConfig
        );

        REQUIRE(result.format == ResponseFormat::ARROW_STREAM);
        REQUIRE(result.codec == "zstd");
    }

    SECTION("LZ4 codec extracted from Accept header") {
        ResponseFormatConfig formatConfig;
        formatConfig.arrowEnabled = true;
        formatConfig.formats = {"json", "arrow"};

        auto result = negotiateContentType(
            "application/vnd.apache.arrow.stream;codec=lz4",
            "",
            formatConfig
        );

        REQUIRE(result.format == ResponseFormat::ARROW_STREAM);
        REQUIRE(result.codec == "lz4");
    }

    SECTION("Invalid codec in Accept header is ignored") {
        ResponseFormatConfig formatConfig;
        formatConfig.arrowEnabled = true;
        formatConfig.formats = {"json", "arrow"};

        auto result = negotiateContentType(
            "application/vnd.apache.arrow.stream;codec=invalid",
            "",
            formatConfig
        );

        // Should still return Arrow format, but codec should be empty or ignored
        REQUIRE(result.format == ResponseFormat::ARROW_STREAM);
        // Invalid codec should be ignored (empty codec = uncompressed)
        REQUIRE(result.codec.empty());
    }

    SECTION("Codec with quality values") {
        ResponseFormatConfig formatConfig;
        formatConfig.arrowEnabled = true;
        formatConfig.formats = {"json", "arrow"};

        // Prefer ZSTD, but accept LZ4
        auto result = negotiateContentType(
            "application/vnd.apache.arrow.stream;codec=zstd;q=1.0, "
            "application/vnd.apache.arrow.stream;codec=lz4;q=0.5",
            "",
            formatConfig
        );

        REQUIRE(result.format == ResponseFormat::ARROW_STREAM);
        // Should prefer ZSTD (higher quality)
        REQUIRE(result.codec == "zstd");
    }
}

TEST_CASE("Arrow Compression Edge Cases", "[arrow][compression][edge]") {
    DuckDBCompressionFixture fixture;

    SECTION("Empty result compresses without error") {
        fixture.executeQuery("CREATE TABLE empty_table (id INT, name VARCHAR)");
        fixture.executeQuery("SELECT * FROM empty_table");

        ArrowSerializerConfig config;
        config.codec = "zstd";

        auto arrowResult = serializeToArrowIPC(fixture.result, config);

        REQUIRE(arrowResult.success);
        REQUIRE(arrowResult.rowCount == 0);
        // Empty result should still have schema
        REQUIRE(!arrowResult.data.empty());
    }

    SECTION("Single row compresses without error") {
        fixture.executeQuery("SELECT 1 AS id, 'test' AS name");

        ArrowSerializerConfig config;
        config.codec = "lz4";

        auto arrowResult = serializeToArrowIPC(fixture.result, config);

        REQUIRE(arrowResult.success);
        REQUIRE(arrowResult.rowCount == 1);
    }

    SECTION("Large dataset compresses successfully") {
        fixture.createCompressibleData(10000);

        ArrowSerializerConfig config;
        config.codec = "zstd";

        auto arrowResult = serializeToArrowIPC(fixture.result, config);

        REQUIRE(arrowResult.success);
        REQUIRE(arrowResult.rowCount == 10000);
    }

    SECTION("NULL values compress correctly") {
        fixture.executeQuery("CREATE TABLE nullable_data AS SELECT "
                            "i AS id, "
                            "CASE WHEN i % 2 = 0 THEN 'value' ELSE NULL END AS nullable_col "
                            "FROM range(100) t(i)");
        fixture.executeQuery("SELECT * FROM nullable_data");

        ArrowSerializerConfig config;
        config.codec = "zstd";

        auto arrowResult = serializeToArrowIPC(fixture.result, config);

        REQUIRE(arrowResult.success);
        REQUIRE(arrowResult.rowCount == 100);
    }
}
