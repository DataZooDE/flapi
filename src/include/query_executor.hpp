#pragma once

#include <duckdb.h>
#include <crow.h>
#include <stdexcept>
#include <optional>
#include <functional>
#include <string>
#include <map>
#include <thread>
#include <vector>

#include "prepared_template_rewriter.hpp"
#include "trace_scope.hpp"

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

class QueryExecutor;

// Active-query registry (issue #111). A QueryExecutor registers itself under the
// thread running its query for the duration of that query; another thread can
// then interrupt whatever query is running on a given thread by its id. This
// lets the MCP Tasks worker make tasks/cancel and shutdown preemptive without
// threading an executor handle through the whole tool-execution pipeline. All
// operations are mutex-guarded, and interrupt() only touches a live executor.
void registerActiveExecutor(std::thread::id tid, QueryExecutor* exec);
void unregisterActiveExecutor(std::thread::id tid);
void interruptActiveExecutor(std::thread::id tid);

} // namespace flapi

// Define standard vector size if not defined by DuckDB
#ifndef STANDARD_VECTOR_SIZE
#define STANDARD_VECTOR_SIZE 1024
#endif

namespace flapi {

// Receives result rows one at a time instead of a materialised `data` tree: the response
// writers (REST JSON/CSV, MCP) append each row to their output and drop it, so a large
// result is never held as a JSON tree plus copies of it.
struct ResultSink {
    std::function<void(const std::vector<std::string>& columns)> begin;
    std::function<void(const crow::json::wvalue& row)> row;
    // When set, rows are written as JSON objects (comma-separated, no brackets) straight from
    // the DuckDB vectors into this text - no crow::json::wvalue per row; `row` is then unused.
    // The text is byte-for-byte what appendRow() writes for the same row.
    std::string* json_rows = nullptr;
};

class QueryResult {
public:
    crow::json::wvalue data;
    // Rows delivered to a ResultSink instead of `data` (then `data` stays empty).
    idx_t streamed_rows = 0;
    bool streamed = false;
    idx_t rowCount() const { return streamed ? streamed_rows : data.size(); }
    // When a caller streamed the rows into JSON text itself, it may keep it here;
    // dumpData() then returns it.
    std::string streamed_json;
    std::string next = "";
    int64_t total_count = -1;
    // The result's column names in SELECT order. crow's object is an
    // unordered map, so `data` alone cannot say which column came first.
    std::vector<std::string> columns;

    // `max_rows`: rows past it are read but not converted (the pagination probe row).
    static crow::json::wvalue convertResultToJson(const duckdb_result& result,
                                                  idx_t max_rows = static_cast<idx_t>(-1));
    // Converts row by row into `sink`; returns the number of rows delivered.
    static idx_t streamResult(const duckdb_result& result, idx_t max_rows, const ResultSink& sink);
    // ResultSink::json_rows: rows written from the vectors into `out`, no wvalue per row.
    static idx_t streamResultAsJsonText(const duckdb_result& result, const std::vector<std::string>& names,
                                        idx_t max_rows, std::string& out);
    // One row as dumpData() prints it inside the list (columns in SELECT order).
    static void appendRow(const crow::json::wvalue& row, const std::vector<std::string>& columns, std::string& out);

    // `data` as JSON text: row keys in `columns` order and every double in
    // its shortest round-trip form (crow prints 21 significant digits, so
    // 711.56 came out as 711.55999999999994543). Use this, not data.dump().
    std::string dumpData() const;

    // Any JSON value, doubles in shortest round-trip form. `row_keys`, when
    // given, orders the keys of every object directly inside a list.
    static std::string dumpJson(const crow::json::wvalue& value,
                                const std::vector<std::string>* row_keys = nullptr);

    // One double as JSON: shortest text that reads back as the same value,
    // with ".0" kept on integral values so the column stays a float to
    // typed clients.
    static std::string formatDouble(double value);

    // One JSON number as dump() printed it, re-formatted with formatDouble
    // when it is a floating-point number; integers pass through unchanged.
    static std::string normaliseNumberText(const std::string& text);
    
private:
    static std::tuple<std::vector<std::string>, std::vector<duckdb_type>> extractNamesAndTypes(duckdb_result& result);
    static std::vector<crow::json::wvalue> convertChunkToJson(const std::vector<std::string> &names, duckdb_data_chunk chunk);

    

    static crow::json::wvalue convertVectorEntryToJson(const duckdb_vector &vector, const idx_t row_idx);
    static crow::json::wvalue convertVectorVarcharToJson(const duckdb_vector &vector, const idx_t row_idx);
    static crow::json::wvalue convertVectorJsonToJson(const duckdb_vector &vector, const idx_t row_idx);
    static crow::json::wvalue convertVectorDecimalToJson(const duckdb_vector &vector, const idx_t row_idx);
    // Exact JSON for a DECIMAL value (see the definition for the limits).
    static crow::json::wvalue decimalToJson(const duckdb_decimal& decimal);
    static crow::json::wvalue convertVectorTimestampToJson(const duckdb_vector &vector, const idx_t row_idx);
    static crow::json::wvalue convertVectorDateToJson(const duckdb_vector &vector, const idx_t row_idx);
    static crow::json::wvalue convertVectorTimeToJson(const duckdb_vector &vector, const idx_t row_idx);
    static crow::json::wvalue convertVectorIntervalToJson(const duckdb_vector &vector, const idx_t row_idx);
    static crow::json::wvalue convertVectorEnumToJson(const duckdb_vector &vector, const idx_t row_idx);
    static crow::json::wvalue convertVectorUuidToJson(const duckdb_vector &vector, const idx_t row_idx);
    static crow::json::wvalue convertVectorHugeintToJson(const duckdb_vector &vector, const idx_t row_idx);
    static crow::json::wvalue convertVectorUhugeintToJson(const duckdb_vector &vector, const idx_t row_idx);
    static crow::json::wvalue convertVectorBlobToJson(const duckdb_vector &vector, const idx_t row_idx);
    static crow::json::wvalue convertVectorBitToJson(const duckdb_vector &vector, const idx_t row_idx);
    static crow::json::wvalue convertVectorListToJson(const duckdb_vector &vector, const idx_t row_idx);
    static crow::json::wvalue convertVectorArrayToJson(const duckdb_vector &vector, const idx_t row_idx);
    static crow::json::wvalue convertVectorStructToJson(const duckdb_vector &vector, const idx_t row_idx);
    static crow::json::wvalue convertVectorUnionToJson(const duckdb_vector &vector, const idx_t row_idx);
    static crow::json::wvalue convertVectorMapToJson(const duckdb_vector &vector, const idx_t row_idx);
    static std::string vectorEntryToMapKey(const duckdb_vector &vector, const idx_t row_idx);
    // Universal C++-API fallback for types without a dedicated C-API reader.
    static crow::json::wvalue convertVectorValueToJson(const duckdb_vector &vector, const idx_t row_idx);
    
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
    // While one is alive on a thread, executors created there for the default session settings
    // borrow that thread's pooled connection (and its prepared-statement cache) instead of
    // opening and closing a connection per query. Endpoint reads open one.
    struct ReuseConnectionScope {
        ReuseConnectionScope();
        ~ReuseConnectionScope();
    };
    // Drops every thread's pooled connection at its next use (a database was closed/reopened).
    static void InvalidatePooledConnections();

    QueryExecutor(duckdb_database db);
    // Also applies `SET SESSION default_order` on the new connection.
    // `session_time_zone` is the per-connection fallback for a TimeZone that could not be set globally.
    QueryExecutor(duckdb_database db, const std::optional<std::string>& session_default_order,
                  const std::optional<std::string>& session_time_zone = std::nullopt);
    ~QueryExecutor();
    
    void execute(const std::string& query, const std::string& context = "");
    // The span's db.operation.name comes from duckdb_prepared_statement_type on
    // the handle, so no SQL text is passed in and none can reach a span.
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
    std::vector<std::string> columnNames() const {
        std::vector<std::string> names;
        for (idx_t i = 0; i < columnCount(); i++) {
            names.emplace_back(duckdb_column_name(const_cast<duckdb_result*>(&result), i));
        }
        return names;
    }
    
    crow::json::wvalue toJson(idx_t max_rows = static_cast<idx_t>(-1)) const;

    // Interrupt an in-flight query on this executor's connection from another
    // thread. Used by the MCP Tasks extension (issue #111) to make tasks/cancel
    // and graceful shutdown preemptive: the running duckdb_query returns an
    // error, which execute() then surfaces as a thrown exception. Safe to call
    // concurrently with execute(); a no-op if there is no live connection.
    void interrupt() { if (conn) { duckdb_interrupt(conn); } }

    // Make these public since they're used directly in DatabaseManager
    duckdb_connection conn;

    // DuckDB profiling is CONNECTION-scoped (enable_profiling and
    // custom_profiling_settings are SetLocal only - there is no global setter),
    // and flAPI opens a connection per query. Enabled lazily, at most once per
    // executor, and only when a recording span will actually carry the result.
    bool profiling_enabled_ = false;
    void enableProfilingIfRequested(const SpanScope& span);
    void attachProfilingMetrics(SpanScope& span) const;
    mutable duckdb_result result;
    bool has_result;

private:
    bool borrowed_ = false;
    duckdb_prepared_statement cachedPrepare(const std::string& sql);
};

} // namespace flapi