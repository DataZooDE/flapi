/**
 * Arrow Configuration Unit Tests (TDD Phase - Task 6.1)
 *
 * Tests for Arrow IPC configuration and resource limits.
 * These tests verify:
 * 1. Global Arrow configuration parsing
 * 2. Endpoint-level configuration overrides
 * 3. Request-level parameter handling
 * 4. Resource limit enforcement
 *
 * TDD Status:
 * - Some tests related to batch size control are expected to FAIL
 *   because batch size is currently determined by DuckDB's native chunking,
 *   not by our config.batchSize parameter.
 * - These failing tests document the desired behavior for Task 6.2
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
class DuckDBConfigFixture {
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

    void createLargeData(size_t num_rows = 10000) {
        std::string sql = "CREATE TABLE large_data AS SELECT "
                         "i AS id, "
                         "'repeated_string_value' AS text_col, "
                         "i % 100 AS category "
                         "FROM range(" + std::to_string(num_rows) + ") t(i)";
        executeQuery(sql.c_str());
        executeQuery("SELECT * FROM large_data");
    }

public:
    DuckDBConfigFixture() { SetUp(); }
    ~DuckDBConfigFixture() { TearDown(); }
};

TEST_CASE("Arrow Configuration Defaults", "[arrow][config][defaults]") {
    SECTION("Default batch size is 8192") {
        ArrowSerializerConfig config;
        REQUIRE(config.batchSize == 8192);
    }

    SECTION("Default codec is empty (no compression)") {
        ArrowSerializerConfig config;
        REQUIRE(config.codec.empty());
    }

    SECTION("Default compression level is 0") {
        ArrowSerializerConfig config;
        REQUIRE(config.compressionLevel == 0);
    }

    SECTION("Default max memory is 256MB") {
        ArrowSerializerConfig config;
        REQUIRE(config.maxMemoryBytes == 256 * 1024 * 1024);
    }
}

TEST_CASE("Arrow Configuration Custom Values", "[arrow][config][custom]") {
    SECTION("Custom batch size is respected") {
        DuckDBConfigFixture fixture;
        fixture.executeQuery("SELECT i FROM range(1000) t(i)");

        ArrowSerializerConfig config;
        config.batchSize = 100;

        auto arrowResult = serializeToArrowIPC(fixture.result, config);

        REQUIRE(arrowResult.success);
        REQUIRE(arrowResult.rowCount == 1000);
        // With 1000 rows and batch size of 100, we should have ~10 batches
        REQUIRE(arrowResult.batchCount >= 10);
    }

    SECTION("Custom codec is applied") {
        DuckDBConfigFixture fixture;
        fixture.executeQuery("SELECT i FROM range(100) t(i)");

        ArrowSerializerConfig config;
        config.codec = "zstd";

        auto arrowResult = serializeToArrowIPC(fixture.result, config);

        REQUIRE(arrowResult.success);
        // ZSTD compressed should have ZSTD magic number
        REQUIRE(arrowResult.data.size() >= 4);
        uint32_t magic;
        std::memcpy(&magic, arrowResult.data.data(), 4);
        REQUIRE(magic == 0xFD2FB528);
    }

    SECTION("Custom compression level is applied") {
        DuckDBConfigFixture fixture;
        fixture.createLargeData(1000);

        // Level 1 (fast)
        ArrowSerializerConfig config1;
        config1.codec = "zstd";
        config1.compressionLevel = 1;
        auto result1 = serializeToArrowIPC(fixture.result, config1);
        REQUIRE(result1.success);
        size_t size1 = result1.data.size();

        // Level 9 (higher compression)
        fixture.TearDown();
        fixture.SetUp();
        fixture.createLargeData(1000);
        ArrowSerializerConfig config9;
        config9.codec = "zstd";
        config9.compressionLevel = 9;
        auto result9 = serializeToArrowIPC(fixture.result, config9);
        REQUIRE(result9.success);
        size_t size9 = result9.data.size();

        // Higher compression level should produce smaller or equal size
        // (Allow 10% margin for small datasets)
        REQUIRE(size9 <= size1 * 1.1);
    }

    SECTION("Custom max memory is enforced") {
        DuckDBConfigFixture fixture;
        fixture.createLargeData(10000);

        ArrowSerializerConfig config;
        config.maxMemoryBytes = 1024;  // 1KB - too small

        auto arrowResult = serializeToArrowIPC(fixture.result, config);

        // Should fail due to memory limit
        REQUIRE(!arrowResult.success);
        REQUIRE_THAT(arrowResult.errorMessage,
                    Catch::Matchers::ContainsSubstring("memory"));
    }
}

TEST_CASE("Arrow Codec Validation", "[arrow][config][codec]") {
    SECTION("Valid codec names are accepted") {
        REQUIRE(isValidCodec(""));
        REQUIRE(isValidCodec("zstd"));
        REQUIRE(isValidCodec("lz4"));
        REQUIRE(isValidCodec("ZSTD"));
        REQUIRE(isValidCodec("LZ4"));
        REQUIRE(isValidCodec("Zstd"));
    }

    SECTION("Invalid codec names are rejected") {
        REQUIRE(!isValidCodec("gzip"));
        REQUIRE(!isValidCodec("deflate"));
        REQUIRE(!isValidCodec("invalid"));
        REQUIRE(!isValidCodec("snappy"));
    }

    SECTION("Codec name normalization") {
        REQUIRE(normalizeCodecName("ZSTD") == "zstd");
        REQUIRE(normalizeCodecName("LZ4") == "lz4");
        REQUIRE(normalizeCodecName("Zstd") == "zstd");
        REQUIRE(normalizeCodecName("") == "");
    }
}

TEST_CASE("Arrow Memory Limits", "[arrow][config][limits][memory]") {
    DuckDBConfigFixture fixture;

    SECTION("Memory limit prevents large serializations") {
        fixture.createLargeData(50000);

        ArrowSerializerConfig config;
        config.maxMemoryBytes = 100;  // 100 bytes - way too small

        auto arrowResult = serializeToArrowIPC(fixture.result, config);

        REQUIRE(!arrowResult.success);
        REQUIRE_THAT(arrowResult.errorMessage,
                    Catch::Matchers::ContainsSubstring("memory"));
    }

    SECTION("Sufficient memory allows serialization") {
        fixture.createLargeData(1000);

        ArrowSerializerConfig config;
        config.maxMemoryBytes = 100 * 1024 * 1024;  // 100MB - plenty

        auto arrowResult = serializeToArrowIPC(fixture.result, config);

        REQUIRE(arrowResult.success);
        REQUIRE(arrowResult.rowCount == 1000);
    }

    SECTION("Zero memory limit disables the check") {
        fixture.createLargeData(1000);

        ArrowSerializerConfig config;
        config.maxMemoryBytes = 0;  // 0 means no limit

        auto arrowResult = serializeToArrowIPC(fixture.result, config);

        REQUIRE(arrowResult.success);
        REQUIRE(arrowResult.rowCount == 1000);
    }
}

TEST_CASE("Arrow Batch Size Configuration", "[arrow][config][batch]") {
    DuckDBConfigFixture fixture;

    SECTION("Small batch size creates more batches") {
        fixture.createLargeData(10000);

        ArrowSerializerConfig config;
        config.batchSize = 100;  // 100 rows per batch

        auto arrowResult = serializeToArrowIPC(fixture.result, config);

        REQUIRE(arrowResult.success);
        REQUIRE(arrowResult.rowCount == 10000);
        // Should have at least 100 batches (10000 / 100)
        REQUIRE(arrowResult.batchCount >= 100);
    }

    SECTION("Large batch size creates fewer batches") {
        fixture.createLargeData(10000);

        ArrowSerializerConfig config;
        config.batchSize = 50000;  // Larger than data set

        auto arrowResult = serializeToArrowIPC(fixture.result, config);

        REQUIRE(arrowResult.success);
        REQUIRE(arrowResult.rowCount == 10000);
        // Should have just 1 batch
        REQUIRE(arrowResult.batchCount == 1);
    }

    SECTION("Default batch size is reasonable") {
        fixture.createLargeData(50000);

        ArrowSerializerConfig config;  // Default batch size

        auto arrowResult = serializeToArrowIPC(fixture.result, config);

        REQUIRE(arrowResult.success);
        REQUIRE(arrowResult.rowCount == 50000);
        // With default batch size of 8192, should have ~6 batches
        REQUIRE(arrowResult.batchCount >= 5);
        REQUIRE(arrowResult.batchCount <= 10);
    }
}

TEST_CASE("Arrow Endpoint Format Configuration", "[arrow][config][endpoint]") {
    SECTION("Arrow enabled in formats list") {
        ResponseFormatConfig formatConfig;
        formatConfig.arrowEnabled = true;
        formatConfig.formats = {"json", "arrow"};

        auto result = negotiateContentType(
            "application/vnd.apache.arrow.stream",
            "",
            formatConfig
        );

        REQUIRE(result.format == ResponseFormat::ARROW_STREAM);
    }

    SECTION("Arrow disabled returns UNSUPPORTED or JSON fallback") {
        ResponseFormatConfig formatConfig;
        formatConfig.arrowEnabled = false;
        formatConfig.formats = {"json"};

        auto result = negotiateContentType(
            "application/vnd.apache.arrow.stream",
            "",
            formatConfig
        );

        // When Arrow is requested but not enabled, should return UNSUPPORTED
        // or fall back to JSON if available
        REQUIRE((result.format == ResponseFormat::UNSUPPORTED ||
                result.format == ResponseFormat::JSON));
    }

    SECTION("JSON is always available") {
        ResponseFormatConfig formatConfig;
        formatConfig.arrowEnabled = false;
        formatConfig.formats = {"json"};

        auto result = negotiateContentType(
            "application/json",
            "",
            formatConfig
        );

        REQUIRE(result.format == ResponseFormat::JSON);
    }

    SECTION("Query param format=arrow works when enabled") {
        ResponseFormatConfig formatConfig;
        formatConfig.arrowEnabled = true;
        formatConfig.formats = {"json", "arrow"};

        auto result = negotiateContentType(
            "*/*",
            "arrow",
            formatConfig
        );

        REQUIRE(result.format == ResponseFormat::ARROW_STREAM);
    }

    SECTION("Query param format=arrow fails when disabled") {
        ResponseFormatConfig formatConfig;
        formatConfig.arrowEnabled = false;
        formatConfig.formats = {"json"};

        auto result = negotiateContentType(
            "*/*",
            "arrow",
            formatConfig
        );

        // Should return UNSUPPORTED when Arrow is explicitly requested but disabled
        REQUIRE(result.format == ResponseFormat::UNSUPPORTED);
    }
}

TEST_CASE("Arrow Request Parameter Handling", "[arrow][config][request]") {
    SECTION("Codec from Accept header") {
        ResponseFormatConfig formatConfig;
        formatConfig.arrowEnabled = true;
        formatConfig.formats = {"json", "arrow"};

        auto result = negotiateContentType(
            "application/vnd.apache.arrow.stream;codec=zstd",
            "",
            formatConfig
        );

        REQUIRE(result.format == ResponseFormat::ARROW_STREAM);
        REQUIRE(result.codec == "zstd");
    }

    SECTION("LZ4 codec from Accept header") {
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

    SECTION("Invalid codec is ignored") {
        ResponseFormatConfig formatConfig;
        formatConfig.arrowEnabled = true;
        formatConfig.formats = {"json", "arrow"};

        auto result = negotiateContentType(
            "application/vnd.apache.arrow.stream;codec=invalid",
            "",
            formatConfig
        );

        REQUIRE(result.format == ResponseFormat::ARROW_STREAM);
        // Invalid codec should result in empty (no compression)
        REQUIRE(result.codec.empty());
    }
}

TEST_CASE("Arrow Configuration Edge Cases", "[arrow][config][edge]") {
    DuckDBConfigFixture fixture;

    SECTION("Empty result with compression") {
        fixture.executeQuery("CREATE TABLE empty_table (id INT)");
        fixture.executeQuery("SELECT * FROM empty_table");

        ArrowSerializerConfig config;
        config.codec = "zstd";

        auto arrowResult = serializeToArrowIPC(fixture.result, config);

        REQUIRE(arrowResult.success);
        REQUIRE(arrowResult.rowCount == 0);
    }

    SECTION("Very small batch size") {
        fixture.executeQuery("SELECT i FROM range(10) t(i)");

        ArrowSerializerConfig config;
        config.batchSize = 1;  // 1 row per batch

        auto arrowResult = serializeToArrowIPC(fixture.result, config);

        REQUIRE(arrowResult.success);
        REQUIRE(arrowResult.rowCount == 10);
        // Should have 10 batches (one per row)
        REQUIRE(arrowResult.batchCount >= 10);
    }

    SECTION("Zero batch size uses default") {
        fixture.executeQuery("SELECT i FROM range(100) t(i)");

        ArrowSerializerConfig config;
        config.batchSize = 0;  // Should use default

        auto arrowResult = serializeToArrowIPC(fixture.result, config);

        REQUIRE(arrowResult.success);
        REQUIRE(arrowResult.rowCount == 100);
    }

    SECTION("Negative compression level uses default") {
        fixture.executeQuery("SELECT i FROM range(100) t(i)");

        ArrowSerializerConfig config;
        config.codec = "zstd";
        config.compressionLevel = -1;

        auto arrowResult = serializeToArrowIPC(fixture.result, config);

        REQUIRE(arrowResult.success);
        REQUIRE(arrowResult.rowCount == 100);
    }

    SECTION("Very high compression level is clamped") {
        fixture.executeQuery("SELECT i FROM range(100) t(i)");

        ArrowSerializerConfig config;
        config.codec = "zstd";
        config.compressionLevel = 100;  // Should be clamped to 22

        auto arrowResult = serializeToArrowIPC(fixture.result, config);

        REQUIRE(arrowResult.success);
        REQUIRE(arrowResult.rowCount == 100);
    }
}
