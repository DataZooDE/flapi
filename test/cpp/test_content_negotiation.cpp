#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include "content_negotiation.hpp"

// =============================================================================
// TESTS - These define the expected behavior
// =============================================================================

using namespace flapi;
using Catch::Matchers::Equals;

TEST_CASE("Accept Header Parsing - Basic", "[content-negotiation][accept-header]") {
    SECTION("Parse single media type without quality") {
        auto types = parseAcceptHeader("application/json");

        REQUIRE(types.size() == 1);
        CHECK(types[0].type == "application");
        CHECK(types[0].subtype == "json");
        CHECK(types[0].quality == 1.0);
    }

    SECTION("Parse single media type with quality") {
        auto types = parseAcceptHeader("application/json;q=0.8");

        REQUIRE(types.size() == 1);
        CHECK(types[0].type == "application");
        CHECK(types[0].subtype == "json");
        CHECK(types[0].quality == 0.8);
    }

    SECTION("Parse Arrow stream media type") {
        auto types = parseAcceptHeader("application/vnd.apache.arrow.stream");

        REQUIRE(types.size() == 1);
        CHECK(types[0].isArrowStream());
        CHECK(types[0].fullType() == "application/vnd.apache.arrow.stream");
    }

    SECTION("Parse wildcard media type") {
        auto types = parseAcceptHeader("*/*");

        REQUIRE(types.size() == 1);
        CHECK(types[0].isWildcard());
    }
}

TEST_CASE("Accept Header Parsing - Quality Values", "[content-negotiation][accept-header]") {
    SECTION("Quality values sorted descending") {
        auto types = parseAcceptHeader("text/csv;q=0.5, application/json;q=0.9, application/vnd.apache.arrow.stream;q=1.0");

        REQUIRE(types.size() == 3);
        // Should be sorted by quality, highest first
        CHECK(types[0].quality == 1.0);
        CHECK(types[0].isArrowStream());
        CHECK(types[1].quality == 0.9);
        CHECK(types[1].isJson());
        CHECK(types[2].quality == 0.5);
        CHECK(types[2].isCsv());
    }

    SECTION("Default quality is 1.0") {
        auto types = parseAcceptHeader("application/json, text/csv;q=0.5");

        REQUIRE(types.size() == 2);
        CHECK(types[0].quality == 1.0);  // JSON has implicit q=1.0
        CHECK(types[1].quality == 0.5);
    }

    SECTION("Quality of 0 means not acceptable") {
        auto types = parseAcceptHeader("application/json, text/csv;q=0");

        REQUIRE(types.size() == 2);
        CHECK(types[1].quality == 0.0);  // Should parse but indicate not acceptable
    }
}

TEST_CASE("Accept Header Parsing - Parameters", "[content-negotiation][accept-header]") {
    SECTION("Parse codec parameter for Arrow") {
        auto types = parseAcceptHeader("application/vnd.apache.arrow.stream;codec=zstd");

        REQUIRE(types.size() == 1);
        CHECK(types[0].isArrowStream());
        CHECK(types[0].parameters["codec"] == "zstd");
    }

    SECTION("Parse multiple parameters") {
        auto types = parseAcceptHeader("application/vnd.apache.arrow.stream;codec=lz4;q=0.9");

        REQUIRE(types.size() == 1);
        CHECK(types[0].parameters["codec"] == "lz4");
        CHECK(types[0].quality == 0.9);
    }

    SECTION("Parse parameters with whitespace") {
        auto types = parseAcceptHeader("application/json ; q=0.8 ; charset=utf-8");

        REQUIRE(types.size() == 1);
        CHECK(types[0].quality == 0.8);
        CHECK(types[0].parameters["charset"] == "utf-8");
    }
}

TEST_CASE("Accept Header Parsing - Multiple Types", "[content-negotiation][accept-header]") {
    SECTION("Parse comma-separated list") {
        auto types = parseAcceptHeader("application/json, application/vnd.apache.arrow.stream, text/csv");

        REQUIRE(types.size() == 3);
    }

    SECTION("Handle complex real-world Accept header") {
        // Example of a realistic Accept header from a data client
        auto types = parseAcceptHeader(
            "application/vnd.apache.arrow.stream;codec=zstd;q=1.0, "
            "application/vnd.apache.arrow.stream;codec=lz4;q=0.9, "
            "application/json;q=0.5, "
            "*/*;q=0.1"
        );

        REQUIRE(types.size() == 4);
        CHECK(types[0].isArrowStream());
        CHECK(types[0].parameters["codec"] == "zstd");
        CHECK(types[1].isArrowStream());
        CHECK(types[1].parameters["codec"] == "lz4");
        CHECK(types[2].isJson());
        CHECK(types[3].isWildcard());
    }
}

TEST_CASE("Accept Header Parsing - Edge Cases", "[content-negotiation][accept-header]") {
    SECTION("Empty header returns empty list") {
        auto types = parseAcceptHeader("");
        CHECK(types.empty());
    }

    SECTION("Whitespace-only header returns empty list") {
        auto types = parseAcceptHeader("   ");
        CHECK(types.empty());
    }

    SECTION("Malformed quality value treated as 1.0") {
        auto types = parseAcceptHeader("application/json;q=invalid");

        REQUIRE(types.size() == 1);
        CHECK(types[0].quality == 1.0);  // Default on parse error
    }

    SECTION("Missing subtype treated as invalid") {
        auto types = parseAcceptHeader("application");

        // Should either be empty or have default subtype
        // Implementation choice - document expected behavior
        CHECK((types.empty() || types[0].subtype == "*"));
    }

    SECTION("Case insensitive media type matching") {
        auto types = parseAcceptHeader("Application/JSON");

        REQUIRE(types.size() == 1);
        CHECK(types[0].isJson());  // Should normalize to lowercase
    }
}

TEST_CASE("Content Negotiation - Query Parameter Override", "[content-negotiation][negotiation]") {
    ResponseFormatConfig config;
    config.formats = {"json", "arrow"};
    config.arrowEnabled = true;

    SECTION("format=arrow overrides Accept header") {
        auto result = negotiateContentType(
            "application/json",  // Accept prefers JSON
            "arrow",             // But query param says Arrow
            config
        );

        CHECK(result.format == ResponseFormat::ARROW_STREAM);
    }

    SECTION("format=json explicitly requests JSON") {
        auto result = negotiateContentType(
            "application/vnd.apache.arrow.stream",
            "json",
            config
        );

        CHECK(result.format == ResponseFormat::JSON);
    }

    SECTION("format=csv requests CSV") {
        config.formats = {"json", "csv"};
        auto result = negotiateContentType(
            "application/json",
            "csv",
            config
        );

        CHECK(result.format == ResponseFormat::CSV);
    }

    SECTION("Invalid format parameter returns error") {
        auto result = negotiateContentType(
            "application/json",
            "xml",  // Not supported
            config
        );

        CHECK(result.format == ResponseFormat::UNSUPPORTED);
        CHECK(!result.errorMessage.empty());
    }
}

TEST_CASE("Content Negotiation - Accept Header Selection", "[content-negotiation][negotiation]") {
    ResponseFormatConfig config;
    config.formats = {"json", "arrow"};
    config.arrowEnabled = true;

    SECTION("Select Arrow when highest quality") {
        auto result = negotiateContentType(
            "application/vnd.apache.arrow.stream;q=1.0, application/json;q=0.5",
            "",
            config
        );

        CHECK(result.format == ResponseFormat::ARROW_STREAM);
    }

    SECTION("Select JSON when Arrow disabled") {
        config.arrowEnabled = false;

        auto result = negotiateContentType(
            "application/vnd.apache.arrow.stream;q=1.0, application/json;q=0.5",
            "",
            config
        );

        CHECK(result.format == ResponseFormat::JSON);
    }

    SECTION("Select first supported format when equal quality") {
        auto result = negotiateContentType(
            "application/json, application/vnd.apache.arrow.stream",
            "",
            config
        );

        // Both q=1.0, so first one wins
        CHECK(result.format == ResponseFormat::JSON);
    }

    SECTION("Wildcard falls back to server default") {
        auto result = negotiateContentType(
            "*/*",
            "",
            config
        );

        CHECK(result.format == ResponseFormat::JSON);  // Default
    }
}

TEST_CASE("Content Negotiation - Endpoint Configuration", "[content-negotiation][negotiation]") {
    SECTION("Endpoint can disable Arrow") {
        ResponseFormatConfig config;
        config.formats = {"json"};  // Only JSON
        config.arrowEnabled = false;

        auto result = negotiateContentType(
            "application/vnd.apache.arrow.stream",
            "",
            config
        );

        CHECK(result.format == ResponseFormat::UNSUPPORTED);
        CHECK(!result.errorMessage.empty());
    }

    SECTION("Endpoint default format used with wildcard") {
        ResponseFormatConfig config;
        config.formats = {"json", "arrow"};
        config.defaultFormat = "arrow";
        config.arrowEnabled = true;

        auto result = negotiateContentType(
            "*/*",
            "",
            config
        );

        CHECK(result.format == ResponseFormat::ARROW_STREAM);
    }

    SECTION("Endpoint with only CSV") {
        ResponseFormatConfig config;
        config.formats = {"csv"};
        config.defaultFormat = "csv";

        auto result = negotiateContentType(
            "text/csv",
            "",
            config
        );

        CHECK(result.format == ResponseFormat::CSV);
    }
}

TEST_CASE("Content Negotiation - Codec Selection", "[content-negotiation][negotiation]") {
    ResponseFormatConfig config;
    config.formats = {"json", "arrow"};
    config.arrowEnabled = true;

    SECTION("Extract codec from Arrow Accept header") {
        auto result = negotiateContentType(
            "application/vnd.apache.arrow.stream;codec=zstd",
            "",
            config
        );

        CHECK(result.format == ResponseFormat::ARROW_STREAM);
        CHECK(result.codec == "zstd");
    }

    SECTION("LZ4 codec") {
        auto result = negotiateContentType(
            "application/vnd.apache.arrow.stream;codec=lz4",
            "",
            config
        );

        CHECK(result.codec == "lz4");
    }

    SECTION("No codec specified means no compression") {
        auto result = negotiateContentType(
            "application/vnd.apache.arrow.stream",
            "",
            config
        );

        CHECK(result.format == ResponseFormat::ARROW_STREAM);
        CHECK(result.codec.empty());
    }

    SECTION("Invalid codec falls back to no compression") {
        auto result = negotiateContentType(
            "application/vnd.apache.arrow.stream;codec=invalid",
            "",
            config
        );

        CHECK(result.format == ResponseFormat::ARROW_STREAM);
        CHECK(result.codec.empty());  // Invalid codec ignored
    }
}

TEST_CASE("Content Negotiation - 406 Not Acceptable", "[content-negotiation][negotiation]") {
    ResponseFormatConfig config;
    config.formats = {"json"};  // Only JSON supported
    config.arrowEnabled = false;

    SECTION("Return unsupported when no match found") {
        auto result = negotiateContentType(
            "application/xml",  // Not supported
            "",
            config
        );

        CHECK(result.format == ResponseFormat::UNSUPPORTED);
    }

    SECTION("Return unsupported when q=0 for only available type") {
        auto result = negotiateContentType(
            "application/json;q=0",
            "",
            config
        );

        // q=0 means explicitly not acceptable
        CHECK(result.format == ResponseFormat::UNSUPPORTED);
    }
}

TEST_CASE("Content Negotiation - Missing Accept Header", "[content-negotiation][negotiation]") {
    ResponseFormatConfig config;
    config.formats = {"json", "arrow"};
    config.defaultFormat = "json";
    config.arrowEnabled = true;

    SECTION("Empty Accept uses endpoint default") {
        auto result = negotiateContentType(
            "",
            "",
            config
        );

        CHECK(result.format == ResponseFormat::JSON);
    }

    SECTION("Query param works without Accept header") {
        auto result = negotiateContentType(
            "",
            "arrow",
            config
        );

        CHECK(result.format == ResponseFormat::ARROW_STREAM);
    }
}

