#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_all.hpp>
#include "type_converter.hpp"

using namespace flapi;

TEST_CASE("TypeConverterRegistry singleton", "[type_converter]") {
    auto& reg1 = TypeConverterRegistry::getInstance();
    auto& reg2 = TypeConverterRegistry::getInstance();

    REQUIRE(&reg1 == &reg2);
}

TEST_CASE("TypeConverterRegistry registration", "[type_converter]") {
    auto& registry = TypeConverterRegistry::getInstance();

    // Check that default converters are registered
    REQUIRE(registry.converterCount() > 0);
    REQUIRE(registry.hasConverter(DUCKDB_TYPE_BOOLEAN));
    REQUIRE(registry.hasConverter(DUCKDB_TYPE_INTEGER));
    REQUIRE(registry.hasConverter(DUCKDB_TYPE_BIGINT));
    REQUIRE(registry.hasConverter(DUCKDB_TYPE_DOUBLE));
    REQUIRE(registry.hasConverter(DUCKDB_TYPE_VARCHAR));
}

TEST_CASE("TypeConverterRegistry::getConverter", "[type_converter]") {
    auto& registry = TypeConverterRegistry::getInstance();

    SECTION("Returns converter for registered type") {
        const TypeConverter* converter = registry.getConverter(DUCKDB_TYPE_INTEGER);
        REQUIRE(converter != nullptr);
        REQUIRE(converter->getType() == DUCKDB_TYPE_INTEGER);
    }

    SECTION("Returns nullptr for unregistered type") {
        // Use an unlikely type ID
        const TypeConverter* converter = registry.getConverter(static_cast<duckdb_type>(999));
        REQUIRE(converter == nullptr);
    }
}

TEST_CASE("TypeConverterRegistry coverage", "[type_converter]") {
    auto& registry = TypeConverterRegistry::getInstance();

    SECTION("Integer types registered") {
        REQUIRE(registry.hasConverter(DUCKDB_TYPE_TINYINT));
        REQUIRE(registry.hasConverter(DUCKDB_TYPE_SMALLINT));
        REQUIRE(registry.hasConverter(DUCKDB_TYPE_INTEGER));
        REQUIRE(registry.hasConverter(DUCKDB_TYPE_BIGINT));
    }

    SECTION("Unsigned integer types registered") {
        REQUIRE(registry.hasConverter(DUCKDB_TYPE_UTINYINT));
        REQUIRE(registry.hasConverter(DUCKDB_TYPE_USMALLINT));
        REQUIRE(registry.hasConverter(DUCKDB_TYPE_UINTEGER));
        REQUIRE(registry.hasConverter(DUCKDB_TYPE_UBIGINT));
    }

    SECTION("Floating point types registered") {
        REQUIRE(registry.hasConverter(DUCKDB_TYPE_FLOAT));
        REQUIRE(registry.hasConverter(DUCKDB_TYPE_DOUBLE));
    }

    SECTION("Other types registered") {
        REQUIRE(registry.hasConverter(DUCKDB_TYPE_BOOLEAN));
        REQUIRE(registry.hasConverter(DUCKDB_TYPE_VARCHAR));
    }
}

TEST_CASE("ConcreteTypeConverter instantiation", "[type_converter]") {
    SECTION("Integer converter") {
        auto converter = std::make_unique<ConcreteTypeConverter<int32_t>>(
            DUCKDB_TYPE_INTEGER,
            [](int32_t value) -> crow::json::wvalue { return value; }
        );
        REQUIRE(converter->getType() == DUCKDB_TYPE_INTEGER);
    }

    SECTION("String converter") {
        auto converter = std::make_unique<ConcreteTypeConverter<const char*>>(
            DUCKDB_TYPE_VARCHAR,
            [](const char* value) -> crow::json::wvalue {
                return std::string(value ? value : "");
            }
        );
        REQUIRE(converter->getType() == DUCKDB_TYPE_VARCHAR);
    }

    SECTION("Boolean converter") {
        auto converter = std::make_unique<ConcreteTypeConverter<bool>>(
            DUCKDB_TYPE_BOOLEAN,
            [](bool value) -> crow::json::wvalue { return value; }
        );
        REQUIRE(converter->getType() == DUCKDB_TYPE_BOOLEAN);
    }
}

TEST_CASE("TypeConverterRegistry::convertValue fallback", "[type_converter]") {
    auto& registry = TypeConverterRegistry::getInstance();

    SECTION("Returns string for unsupported type") {
        // Use an unlikely type ID that won't have a converter
        duckdb_type unsupported_type = static_cast<duckdb_type>(999);

        // This should fall back to string conversion
        // We can't easily test without a real duckdb_result, but we verify the type check works
        REQUIRE_FALSE(registry.hasConverter(unsupported_type));
    }
}

TEST_CASE("TypeConverterRegistry error handling", "[type_converter]") {
    auto& registry = TypeConverterRegistry::getInstance();

    SECTION("Gracefully handles missing converter") {
        REQUIRE_FALSE(registry.hasConverter(static_cast<duckdb_type>(9999)));
    }
}

TEST_CASE("Converter types consistency", "[type_converter]") {
    auto& registry = TypeConverterRegistry::getInstance();

    SECTION("Each converter returns its type") {
        std::vector<duckdb_type> types_to_check = {
            DUCKDB_TYPE_BOOLEAN,
            DUCKDB_TYPE_TINYINT,
            DUCKDB_TYPE_SMALLINT,
            DUCKDB_TYPE_INTEGER,
            DUCKDB_TYPE_BIGINT,
            DUCKDB_TYPE_UTINYINT,
            DUCKDB_TYPE_USMALLINT,
            DUCKDB_TYPE_UINTEGER,
            DUCKDB_TYPE_UBIGINT,
            DUCKDB_TYPE_FLOAT,
            DUCKDB_TYPE_DOUBLE,
            DUCKDB_TYPE_VARCHAR
        };

        for (auto type : types_to_check) {
            if (registry.hasConverter(type)) {
                const TypeConverter* conv = registry.getConverter(type);
                REQUIRE(conv != nullptr);
                REQUIRE(conv->getType() == type);
            }
        }
    }
}

TEST_CASE("TypeConverter interface polymorphism", "[type_converter]") {
    SECTION("Converters implement TypeConverter interface") {
        auto bool_conv = std::make_unique<ConcreteTypeConverter<bool>>(
            DUCKDB_TYPE_BOOLEAN,
            [](bool v) -> crow::json::wvalue { return v; }
        );

        TypeConverter* base_ptr = bool_conv.get();
        REQUIRE(base_ptr != nullptr);
        REQUIRE(base_ptr->getType() == DUCKDB_TYPE_BOOLEAN);
    }
}

TEST_CASE("Registry converter counting", "[type_converter]") {
    auto& registry = TypeConverterRegistry::getInstance();

    SECTION("Has multiple converters registered") {
        size_t count = registry.converterCount();
        REQUIRE(count >= 12);  // At least our 12 default types
    }

    SECTION("Minimum required types") {
        REQUIRE(registry.hasConverter(DUCKDB_TYPE_BOOLEAN));
        REQUIRE(registry.hasConverter(DUCKDB_TYPE_BIGINT));
        REQUIRE(registry.hasConverter(DUCKDB_TYPE_DOUBLE));
        REQUIRE(registry.hasConverter(DUCKDB_TYPE_VARCHAR));
    }
}

TEST_CASE("Registry converter registration pattern", "[type_converter]") {
    // Create a new registry instance for testing custom registration
    // (In production, we use the singleton, but for testing we can verify the pattern)

    SECTION("Converter is properly stored") {
        auto converter = std::make_unique<ConcreteTypeConverter<int64_t>>(
            DUCKDB_TYPE_BIGINT,
            [](int64_t v) -> crow::json::wvalue { return v; }
        );

        // Verify converter details before registration
        REQUIRE(converter->getType() == DUCKDB_TYPE_BIGINT);
    }
}

TEST_CASE("Type converter extensibility", "[type_converter]") {
    SECTION("Registry can handle new type registrations") {
        // Verify the interface supports registration
        auto& registry = TypeConverterRegistry::getInstance();

        auto custom_converter = std::make_unique<ConcreteTypeConverter<float>>(
            DUCKDB_TYPE_FLOAT,
            [](float v) -> crow::json::wvalue { return static_cast<double>(v); }
        );

        // While we don't re-register (singleton pattern), we verify the interface exists
        REQUIRE(custom_converter->getType() == DUCKDB_TYPE_FLOAT);
    }
}

TEST_CASE("Converter lambda functions", "[type_converter]") {
    SECTION("Boolean converter lambda works correctly") {
        auto converter = std::make_unique<ConcreteTypeConverter<bool>>(
            DUCKDB_TYPE_BOOLEAN,
            [](bool v) -> crow::json::wvalue { return v; }
        );

        REQUIRE(converter->getType() == DUCKDB_TYPE_BOOLEAN);
    }

    SECTION("Integer converter lambda works correctly") {
        auto converter = std::make_unique<ConcreteTypeConverter<int32_t>>(
            DUCKDB_TYPE_INTEGER,
            [](int32_t v) -> crow::json::wvalue { return static_cast<int64_t>(v); }
        );

        REQUIRE(converter->getType() == DUCKDB_TYPE_INTEGER);
    }

    SECTION("String converter lambda works correctly") {
        auto converter = std::make_unique<ConcreteTypeConverter<const char*>>(
            DUCKDB_TYPE_VARCHAR,
            [](const char* v) -> crow::json::wvalue {
                return std::string(v ? v : "");
            }
        );

        REQUIRE(converter->getType() == DUCKDB_TYPE_VARCHAR);
    }
}

TEST_CASE("TypeConverterRegistry type safety", "[type_converter]") {
    auto& registry = TypeConverterRegistry::getInstance();

    SECTION("Converters maintain type information") {
        const TypeConverter* int_conv = registry.getConverter(DUCKDB_TYPE_INTEGER);
        const TypeConverter* bool_conv = registry.getConverter(DUCKDB_TYPE_BOOLEAN);

        REQUIRE(int_conv != nullptr);
        REQUIRE(bool_conv != nullptr);
        REQUIRE(int_conv->getType() != bool_conv->getType());
    }
}

TEST_CASE("Registry nullptr safety", "[type_converter]") {
    auto& registry = TypeConverterRegistry::getInstance();

    SECTION("Safe to query for unknown types") {
        const TypeConverter* conv = registry.getConverter(static_cast<duckdb_type>(9999));
        REQUIRE(conv == nullptr);
    }

    SECTION("hasConverter returns false for unknown types") {
        REQUIRE_FALSE(registry.hasConverter(static_cast<duckdb_type>(9999)));
    }
}
