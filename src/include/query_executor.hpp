#pragma once

#include <duckdb.h>
#include <crow.h>
#include <string>
#include <map>
#include <vector>

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
            CROW_LOG_WARNING << "Row " << row_idx << " is not valid.";
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
    
    idx_t rowCount() const { return has_result ? duckdb_row_count(&result) : 0; }
    idx_t columnCount() const { return has_result ? duckdb_column_count(&result) : 0; }
    
    crow::json::wvalue toJson() const;

    // Make these public since they're used directly in DatabaseManager
    duckdb_connection conn;
    mutable duckdb_result result;
    bool has_result;
};

} // namespace flapi