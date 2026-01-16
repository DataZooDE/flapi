#pragma once

#include <duckdb.h>
#include <crow.h>
#include <memory>
#include <map>
#include <functional>

namespace flapi {

// Base interface for type converters
class TypeConverter {
public:
    virtual ~TypeConverter() = default;

    // Convert a value from DuckDB result at given row/column to JSON
    virtual crow::json::wvalue convert(duckdb_result* result, idx_t col, idx_t row) const = 0;

    // Get the DuckDB type this converter handles
    virtual duckdb_type getType() const = 0;
};

// Registry for managing type converters
class TypeConverterRegistry {
public:
    // Get singleton instance
    static TypeConverterRegistry& getInstance();

    // Register a converter for a specific DuckDB type
    void registerConverter(duckdb_type type, std::unique_ptr<TypeConverter> converter);

    // Get converter for a type (returns nullptr if not found)
    const TypeConverter* getConverter(duckdb_type type) const;

    // Convert a value - tries to find appropriate converter, falls back to string
    crow::json::wvalue convertValue(duckdb_result* result, duckdb_type type, idx_t col, idx_t row) const;

    // Check if converter exists for type
    bool hasConverter(duckdb_type type) const;

    // Get count of registered converters (for testing/debugging)
    size_t converterCount() const;

private:
    TypeConverterRegistry();
    void registerDefaultConverters();

    std::map<duckdb_type, std::unique_ptr<TypeConverter>> converters_;
};

// Template-based concrete converter for specific types
template<typename T>
class ConcreteTypeConverter : public TypeConverter {
public:
    using ConverterFunc = std::function<crow::json::wvalue(T value)>;

    ConcreteTypeConverter(duckdb_type type, ConverterFunc func)
        : type_(type), converter_(func) {}

    crow::json::wvalue convert(duckdb_result* result, idx_t col, idx_t row) const override {
        T value = getValue(result, col, row);
        return converter_(value);
    }

    duckdb_type getType() const override { return type_; }

private:
    duckdb_type type_;
    ConverterFunc converter_;

    // Specializations for common types - defined in implementation
    T getValue(duckdb_result* result, idx_t col, idx_t row) const;
};

// Explicit specializations for common DuckDB types
// Note: These are declared here but defined in type_converter.cpp

template<>
bool ConcreteTypeConverter<bool>::getValue(duckdb_result* result, idx_t col, idx_t row) const;

template<>
int8_t ConcreteTypeConverter<int8_t>::getValue(duckdb_result* result, idx_t col, idx_t row) const;

template<>
int16_t ConcreteTypeConverter<int16_t>::getValue(duckdb_result* result, idx_t col, idx_t row) const;

template<>
int32_t ConcreteTypeConverter<int32_t>::getValue(duckdb_result* result, idx_t col, idx_t row) const;

template<>
int64_t ConcreteTypeConverter<int64_t>::getValue(duckdb_result* result, idx_t col, idx_t row) const;

template<>
uint8_t ConcreteTypeConverter<uint8_t>::getValue(duckdb_result* result, idx_t col, idx_t row) const;

template<>
uint16_t ConcreteTypeConverter<uint16_t>::getValue(duckdb_result* result, idx_t col, idx_t row) const;

template<>
uint32_t ConcreteTypeConverter<uint32_t>::getValue(duckdb_result* result, idx_t col, idx_t row) const;

template<>
uint64_t ConcreteTypeConverter<uint64_t>::getValue(duckdb_result* result, idx_t col, idx_t row) const;

template<>
float ConcreteTypeConverter<float>::getValue(duckdb_result* result, idx_t col, idx_t row) const;

template<>
double ConcreteTypeConverter<double>::getValue(duckdb_result* result, idx_t col, idx_t row) const;

template<>
const char* ConcreteTypeConverter<const char*>::getValue(duckdb_result* result, idx_t col, idx_t row) const;

} // namespace flapi
