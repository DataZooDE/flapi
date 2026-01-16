#include "type_converter.hpp"
#include <crow/logging.h>

namespace flapi {

// Singleton instance
TypeConverterRegistry& TypeConverterRegistry::getInstance() {
    static TypeConverterRegistry registry;
    return registry;
}

TypeConverterRegistry::TypeConverterRegistry() {
    registerDefaultConverters();
}

void TypeConverterRegistry::registerConverter(duckdb_type type, std::unique_ptr<TypeConverter> converter) {
    converters_[type] = std::move(converter);
}

const TypeConverter* TypeConverterRegistry::getConverter(duckdb_type type) const {
    auto it = converters_.find(type);
    if (it != converters_.end()) {
        return it->second.get();
    }
    return nullptr;
}

bool TypeConverterRegistry::hasConverter(duckdb_type type) const {
    return converters_.find(type) != converters_.end();
}

size_t TypeConverterRegistry::converterCount() const {
    return converters_.size();
}

crow::json::wvalue TypeConverterRegistry::convertValue(duckdb_result* result, duckdb_type type,
                                                       idx_t col, idx_t row) const {
    const TypeConverter* converter = getConverter(type);

    if (converter) {
        try {
            return converter->convert(result, col, row);
        } catch (const std::exception& e) {
            CROW_LOG_WARNING << "Error converting value with type converter: " << e.what();
            // Fall through to fallback
        }
    }

    // Fallback: convert to string
    char* value = duckdb_value_varchar(result, col, row);
    std::string result_str(value ? value : "");
    if (value) {
        duckdb_free(value);
    }
    return result_str;
}

void TypeConverterRegistry::registerDefaultConverters() {
    // Boolean converter
    registerConverter(DUCKDB_TYPE_BOOLEAN,
        std::make_unique<ConcreteTypeConverter<bool>>(
            DUCKDB_TYPE_BOOLEAN,
            [](bool value) -> crow::json::wvalue {
                return value;
            }
        ));

    // Integer types
    registerConverter(DUCKDB_TYPE_TINYINT,
        std::make_unique<ConcreteTypeConverter<int8_t>>(
            DUCKDB_TYPE_TINYINT,
            [](int8_t value) -> crow::json::wvalue {
                return static_cast<int64_t>(value);
            }
        ));

    registerConverter(DUCKDB_TYPE_SMALLINT,
        std::make_unique<ConcreteTypeConverter<int16_t>>(
            DUCKDB_TYPE_SMALLINT,
            [](int16_t value) -> crow::json::wvalue {
                return static_cast<int64_t>(value);
            }
        ));

    registerConverter(DUCKDB_TYPE_INTEGER,
        std::make_unique<ConcreteTypeConverter<int32_t>>(
            DUCKDB_TYPE_INTEGER,
            [](int32_t value) -> crow::json::wvalue {
                return static_cast<int64_t>(value);
            }
        ));

    registerConverter(DUCKDB_TYPE_BIGINT,
        std::make_unique<ConcreteTypeConverter<int64_t>>(
            DUCKDB_TYPE_BIGINT,
            [](int64_t value) -> crow::json::wvalue {
                return value;
            }
        ));

    // Unsigned integer types
    registerConverter(DUCKDB_TYPE_UTINYINT,
        std::make_unique<ConcreteTypeConverter<uint8_t>>(
            DUCKDB_TYPE_UTINYINT,
            [](uint8_t value) -> crow::json::wvalue {
                return static_cast<uint64_t>(value);
            }
        ));

    registerConverter(DUCKDB_TYPE_USMALLINT,
        std::make_unique<ConcreteTypeConverter<uint16_t>>(
            DUCKDB_TYPE_USMALLINT,
            [](uint16_t value) -> crow::json::wvalue {
                return static_cast<uint64_t>(value);
            }
        ));

    registerConverter(DUCKDB_TYPE_UINTEGER,
        std::make_unique<ConcreteTypeConverter<uint32_t>>(
            DUCKDB_TYPE_UINTEGER,
            [](uint32_t value) -> crow::json::wvalue {
                return static_cast<uint64_t>(value);
            }
        ));

    registerConverter(DUCKDB_TYPE_UBIGINT,
        std::make_unique<ConcreteTypeConverter<uint64_t>>(
            DUCKDB_TYPE_UBIGINT,
            [](uint64_t value) -> crow::json::wvalue {
                return static_cast<int64_t>(value);  // JSON doesn't have unsigned, cast to signed
            }
        ));

    // Floating point types
    registerConverter(DUCKDB_TYPE_FLOAT,
        std::make_unique<ConcreteTypeConverter<float>>(
            DUCKDB_TYPE_FLOAT,
            [](float value) -> crow::json::wvalue {
                return static_cast<double>(value);
            }
        ));

    registerConverter(DUCKDB_TYPE_DOUBLE,
        std::make_unique<ConcreteTypeConverter<double>>(
            DUCKDB_TYPE_DOUBLE,
            [](double value) -> crow::json::wvalue {
                return value;
            }
        ));

    // String type
    registerConverter(DUCKDB_TYPE_VARCHAR,
        std::make_unique<ConcreteTypeConverter<const char*>>(
            DUCKDB_TYPE_VARCHAR,
            [](const char* value) -> crow::json::wvalue {
                return std::string(value ? value : "");
            }
        ));

    CROW_LOG_DEBUG << "Registered " << converters_.size() << " default type converters";
}

// Template specializations for getValue methods

template<>
bool ConcreteTypeConverter<bool>::getValue(duckdb_result* result, idx_t col, idx_t row) const {
    return duckdb_value_boolean(result, col, row);
}

template<>
int8_t ConcreteTypeConverter<int8_t>::getValue(duckdb_result* result, idx_t col, idx_t row) const {
    return duckdb_value_int8(result, col, row);
}

template<>
int16_t ConcreteTypeConverter<int16_t>::getValue(duckdb_result* result, idx_t col, idx_t row) const {
    return duckdb_value_int16(result, col, row);
}

template<>
int32_t ConcreteTypeConverter<int32_t>::getValue(duckdb_result* result, idx_t col, idx_t row) const {
    return duckdb_value_int32(result, col, row);
}

template<>
int64_t ConcreteTypeConverter<int64_t>::getValue(duckdb_result* result, idx_t col, idx_t row) const {
    return duckdb_value_int64(result, col, row);
}

template<>
uint8_t ConcreteTypeConverter<uint8_t>::getValue(duckdb_result* result, idx_t col, idx_t row) const {
    return duckdb_value_uint8(result, col, row);
}

template<>
uint16_t ConcreteTypeConverter<uint16_t>::getValue(duckdb_result* result, idx_t col, idx_t row) const {
    return duckdb_value_uint16(result, col, row);
}

template<>
uint32_t ConcreteTypeConverter<uint32_t>::getValue(duckdb_result* result, idx_t col, idx_t row) const {
    return duckdb_value_uint32(result, col, row);
}

template<>
uint64_t ConcreteTypeConverter<uint64_t>::getValue(duckdb_result* result, idx_t col, idx_t row) const {
    return duckdb_value_uint64(result, col, row);
}

template<>
float ConcreteTypeConverter<float>::getValue(duckdb_result* result, idx_t col, idx_t row) const {
    return duckdb_value_float(result, col, row);
}

template<>
double ConcreteTypeConverter<double>::getValue(duckdb_result* result, idx_t col, idx_t row) const {
    return duckdb_value_double(result, col, row);
}

template<>
const char* ConcreteTypeConverter<const char*>::getValue(duckdb_result* result, idx_t col, idx_t row) const {
    return duckdb_value_varchar(result, col, row);
}

} // namespace flapi
