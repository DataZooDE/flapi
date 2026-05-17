#pragma once

#include <duckdb.h>
#include <crow.h>
#include <stdexcept>
#include <string>
#include <map>
#include <vector>

#include "prepared_template_rewriter.hpp"

namespace flapi {

// W3.1 PR B: thrown by `QueryExecutor::executeWithBindings` when a
// supplied parameter cannot be converted to its declared SQL type
// (e.g. `id=abc` for an int field, `d=2024-13-99` for a date field).
// This is a CLIENT-input error — RequestHandler maps it to HTTP 400,
// not 500. Distinct from `std::runtime_error` which signals an
// internal failure (prepare/execute) and remains 500.
class BadRequestError : public std::runtime_error {
public:
    explicit BadRequestError(const std::string& msg) : std::runtime_error(msg) {}
};

} // namespace flapi

// Define standard vector size if not defined by DuckDB
#ifndef STANDARD_VECTOR_SIZE
#define STANDARD_VECTOR_SIZE 1024
#endif

namespace flapi {

class QueryResult {
public:
    crow::json::wvalue data;
    std::string next = "";
    int64_t total_count = -1;
    std::vector<std::map<std::string, std::string>> rows;

    static crow::json::wvalue convertResultToJson(const duckdb_result& result);
    
private:
    static std::tuple<std::vector<std::string>, std::vector<duckdb_type>> extractNamesAndTypes(duckdb_result& result);
    static std::vector<crow::json::wvalue> convertChunkToJson(const std::vector<std::string> &names, duckdb_data_chunk chunk);

    

    static crow::json::wvalue convertVectorEntryToJson(const duckdb_vector &vector, const idx_t row_idx);
    static crow::json::wvalue convertVectorVarcharToJson(const duckdb_vector &vector, const idx_t row_idx);
    static crow::json::wvalue convertVectorDecimalToJson(const duckdb_vector &vector, const idx_t row_idx);
    static crow::json::wvalue convertVectorTimestampToJson(const duckdb_vector &vector, const idx_t row_idx);
    static crow::json::wvalue convertVectorDateToJson(const duckdb_vector &vector, const idx_t row_idx);
    static crow::json::wvalue convertVectorTimeToJson(const duckdb_vector &vector, const idx_t row_idx);
    static crow::json::wvalue convertVectorIntervalToJson(const duckdb_vector &vector, const idx_t row_idx);
    static crow::json::wvalue convertVectorEnumToJson(const duckdb_vector &vector, const idx_t row_idx);
    static crow::json::wvalue convertVectorListToJson(const duckdb_vector &vector, const idx_t row_idx);
    static crow::json::wvalue convertVectorStructToJson(const duckdb_vector &vector, const idx_t row_idx);
    
    template<typename T>
    static crow::json::wvalue convertVectorEntryToJson(const duckdb_vector &vector, const idx_t row_idx) {
        auto validity = duckdb_vector_get_validity(vector);

        if (!duckdb_validity_row_is_valid(validity, row_idx)) {
            return crow::json::wvalue(nullptr);
        }
        
        auto data = (T *)duckdb_vector_get_data(vector);
        return crow::json::wvalue(data[row_idx]);
    }

    template <class T>
    static duckdb_hugeint convertIntegerToHugeint(T input) {
        duckdb_hugeint result;
        result.lower = (uint64_t)input;
        result.upper = (input < 0) * -1;
        return result;
    }
};

class QueryExecutor {
public:
    QueryExecutor(duckdb_database db);
    ~QueryExecutor();
    
    void execute(const std::string& query, const std::string& context = "");
    void executePrepared(duckdb_prepared_statement stmt, const std::string& context = "");

    // W3.1 PR B: prepare a query that contains `?` placeholders, bind
    // each according to `bindings` (typed conversion via
    // PreparedValueConverter; missing/empty values become NULL where
    // semantically appropriate), then execute. The result is exposed via
    // the same `result` / `toJson()` accessors as `execute()` so callers
    // can route through either path interchangeably. Throws on bind or
    // prepare failure; the statement is always destroyed.
    void executeWithBindings(const std::string& sql,
                             const std::vector<PreparedBindingSpec>& bindings,
                             const std::map<std::string, std::string>& values,
                             const std::string& context = "");
    
    idx_t rowCount() const { return has_result ? duckdb_row_count(&result) : 0; }
    idx_t columnCount() const { return has_result ? duckdb_column_count(&result) : 0; }
    
    crow::json::wvalue toJson() const;

    // Make these public since they're used directly in DatabaseManager
    duckdb_connection conn;
    mutable duckdb_result result;
    bool has_result;
};

} // namespace flapi