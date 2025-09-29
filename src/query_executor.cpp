#include "query_executor.hpp"
#include <crow.h>
#include <fmt/core.h>
#include <sstream>
#include <stdexcept>

namespace flapi {

QueryExecutor::QueryExecutor(duckdb_database db) : has_result(false) {
    if (duckdb_connect(db, &conn) == DuckDBError) {
        throw std::runtime_error("Failed to create database connection");
    }
}

QueryExecutor::~QueryExecutor() {
    if (has_result) {
        duckdb_destroy_result(&result);
        has_result = false;
    }
    duckdb_disconnect(&conn);
}

void QueryExecutor::execute(const std::string& query, const std::string& context) {
    if (has_result) {
        duckdb_destroy_result(&result);
        has_result = false;
    }
    
    if (duckdb_query(conn, query.c_str(), &result) == DuckDBError) {
        std::string error_message = duckdb_result_error(&result);
        std::string context_msg = context.empty() ? "" : " during " + context;
        duckdb_destroy_result(&result);
        throw std::runtime_error("Query execution failed" + context_msg + ": " + error_message);
    }
    has_result = true;
}

void QueryExecutor::executePrepared(duckdb_prepared_statement stmt, const std::string& context) {
    if (has_result) {
        duckdb_destroy_result(&result);
        has_result = false;
    }
    
    if (duckdb_execute_prepared(stmt, &result) == DuckDBError) {
        std::string error_message = duckdb_result_error(&result);
        std::string context_msg = context.empty() ? "" : " during " + context;
        duckdb_destroy_result(&result);
        throw std::runtime_error("Prepared statement execution failed" + context_msg + ": " + error_message);
    }
    has_result = true;
}

crow::json::wvalue QueryExecutor::toJson() const {
    if (!has_result) {
        throw std::runtime_error("No result available - execute query first");
    }
    
    return QueryResult::convertResultToJson(result);
}

// ------------------------------------------------------------------------------------------------
crow::json::wvalue QueryResult::convertResultToJson(const duckdb_result& result) {
    auto [names, types] = extractNamesAndTypes(const_cast<duckdb_result&>(result));

    std::vector<crow::json::wvalue> rows;
    duckdb_data_chunk chunk;
    while ((chunk = duckdb_fetch_chunk(const_cast<duckdb_result&>(result))) != NULL) {
        for (auto &row : QueryResult::convertChunkToJson(names, chunk)) {
            rows.push_back(row);
        }
        duckdb_destroy_data_chunk(&chunk);
    }
    
    return crow::json::wvalue(rows);
}

std::tuple<std::vector<std::string>, std::vector<duckdb_type>> QueryResult::extractNamesAndTypes(duckdb_result& result) {
    std::vector<std::string> names;
    std::vector<duckdb_type> types;
    for (idx_t i = 0; i < duckdb_column_count(&result); i++) {
        names.push_back(std::string(duckdb_column_name(&result, i)));
        types.push_back(duckdb_column_type(&result, i));
    }
    return std::make_tuple(names, types);
}

std::vector<crow::json::wvalue> QueryResult::convertChunkToJson(const std::vector<std::string> &names, duckdb_data_chunk chunk) {
        std::vector<crow::json::wvalue> result;
    idx_t row_count = duckdb_data_chunk_get_size(chunk);
    
    for (idx_t row_idx = 0; row_idx < row_count; row_idx++) {
        crow::json::wvalue row;
        for (idx_t col_idx = 0; col_idx < names.size(); col_idx++) {
            auto vector = duckdb_data_chunk_get_vector(chunk, col_idx);
            row[names[col_idx]] = convertVectorEntryToJson(vector, row_idx);
        }
        result.push_back(row);
    }

    return result;
}

crow::json::wvalue QueryResult::convertVectorEntryToJson(const duckdb_vector &vector, const idx_t row_idx) {
    auto type = duckdb_vector_get_column_type(vector);
    auto type_id = duckdb_get_type_id(type);
    switch (type_id) {
        case DUCKDB_TYPE_SQLNULL:
            return crow::json::wvalue(nullptr);
        case DUCKDB_TYPE_BOOLEAN:
            return convertVectorEntryToJson<bool>(vector, row_idx);
        case DUCKDB_TYPE_TINYINT:
            return convertVectorEntryToJson<std::int8_t>(vector, row_idx);
        case DUCKDB_TYPE_SMALLINT:
            return convertVectorEntryToJson<std::int16_t>(vector, row_idx);
        case DUCKDB_TYPE_INTEGER:
            return convertVectorEntryToJson<std::int32_t>(vector, row_idx);
        case DUCKDB_TYPE_BIGINT:
            return convertVectorEntryToJson<std::int64_t>(vector, row_idx);
        case DUCKDB_TYPE_HUGEINT:
            return convertVectorEntryToJson<std::int64_t>(vector, row_idx);
        case DUCKDB_TYPE_FLOAT:
            return convertVectorEntryToJson<float>(vector, row_idx);
        case DUCKDB_TYPE_DOUBLE:
            return convertVectorEntryToJson<double>(vector, row_idx);
        case DUCKDB_TYPE_VARCHAR:
            return convertVectorVarcharToJson(vector, row_idx);
        case DUCKDB_TYPE_DECIMAL:
            return convertVectorDecimalToJson(vector, row_idx);
        case DUCKDB_TYPE_TIMESTAMP:
            return convertVectorTimestampToJson(vector, row_idx);
        case DUCKDB_TYPE_INTERVAL:
            return convertVectorIntervalToJson(vector, row_idx);
        case DUCKDB_TYPE_DATE:
            return convertVectorDateToJson(vector, row_idx);
        case DUCKDB_TYPE_TIME:
            return convertVectorTimeToJson(vector, row_idx);
        case DUCKDB_TYPE_ENUM:
            return convertVectorEnumToJson(vector, row_idx);
        case DUCKDB_TYPE_LIST:
            return convertVectorListToJson(vector, row_idx);
        case DUCKDB_TYPE_STRUCT:
            return convertVectorStructToJson(vector, row_idx);
        case DUCKDB_TYPE_TIMESTAMP_TZ:
            return convertVectorTimestampToJson(vector, row_idx);  // Treat as regular timestamp
        case DUCKDB_TYPE_TIMESTAMP_S:
            return convertVectorTimestampToJson(vector, row_idx);  // Treat as regular timestamp
        case DUCKDB_TYPE_TIMESTAMP_MS:
            return convertVectorTimestampToJson(vector, row_idx);  // Treat as regular timestamp
        case DUCKDB_TYPE_TIMESTAMP_NS:
            return convertVectorTimestampToJson(vector, row_idx);  // Treat as regular timestamp
        case DUCKDB_TYPE_TIME_TZ:
            return convertVectorTimeToJson(vector, row_idx);  // Treat as regular time
        case DUCKDB_TYPE_TIME_NS:
            return convertVectorTimeToJson(vector, row_idx);  // Treat as regular time
        case DUCKDB_TYPE_UTINYINT:
            return convertVectorEntryToJson<std::uint8_t>(vector, row_idx);
        case DUCKDB_TYPE_USMALLINT:
            return convertVectorEntryToJson<std::uint16_t>(vector, row_idx);
        case DUCKDB_TYPE_UINTEGER:
            return convertVectorEntryToJson<std::uint32_t>(vector, row_idx);
        case DUCKDB_TYPE_UBIGINT:
            return convertVectorEntryToJson<std::uint64_t>(vector, row_idx);
        case DUCKDB_TYPE_UHUGEINT:
            return convertVectorEntryToJson<std::uint64_t>(vector, row_idx);  // Treat as uint64
        case DUCKDB_TYPE_BLOB:
            return convertVectorVarcharToJson(vector, row_idx);  // Treat as string for JSON
        case DUCKDB_TYPE_UUID:
            return convertVectorVarcharToJson(vector, row_idx);  // Treat as string for JSON
        case DUCKDB_TYPE_BIT:
            return convertVectorVarcharToJson(vector, row_idx);  // Treat as string for JSON
        case DUCKDB_TYPE_MAP:
            return convertVectorStructToJson(vector, row_idx);  // Treat as struct for JSON
        case DUCKDB_TYPE_ARRAY:
            return convertVectorListToJson(vector, row_idx);  // Treat as list for JSON
        case DUCKDB_TYPE_UNION:
            return convertVectorStructToJson(vector, row_idx);  // Treat as struct for JSON
        default:
            CROW_LOG_WARNING << "Unknown type: " << type_id;
            return crow::json::wvalue(nullptr);
    }

    duckdb_destroy_logical_type(&type);
    return crow::json::wvalue();
}

crow::json::wvalue QueryResult::convertVectorVarcharToJson(const duckdb_vector &vector, const idx_t row_idx) {
    auto validity = duckdb_vector_get_validity(vector);
    if (!duckdb_validity_row_is_valid(validity, row_idx)) {
        return crow::json::wvalue(nullptr);
    }
    
    auto data = (duckdb_string_t *)duckdb_vector_get_data(vector);
    auto str = duckdb_string_is_inlined(data[row_idx])
             ? std::string(data[row_idx].value.inlined.inlined, data[row_idx].value.inlined.length)
             : std::string((const char*)data[row_idx].value.pointer.ptr, data[row_idx].value.pointer.length);

    return crow::json::wvalue(str);
}

crow::json::wvalue QueryResult::convertVectorDecimalToJson(const duckdb_vector &vector, const idx_t row_idx) {
    auto validity = duckdb_vector_get_validity(vector);

    if (!duckdb_validity_row_is_valid(validity, row_idx)) {
        return crow::json::wvalue(nullptr);
    }

    auto data = (void *)duckdb_vector_get_data(vector);

    auto type = duckdb_vector_get_column_type(vector);
    auto decimal_type = duckdb_decimal_internal_type(type);
    auto decimal_width = duckdb_decimal_width(type);
    auto decimal_scale = duckdb_decimal_scale(type);
    auto hugeint = duckdb_hugeint {0, 0};

    switch (decimal_type) {
        case DUCKDB_TYPE_HUGEINT:
            hugeint = ((duckdb_hugeint *)data)[row_idx];
            break;
        case DUCKDB_TYPE_SMALLINT:
            hugeint = convertIntegerToHugeint<std::int16_t>(((int16_t *)data)[row_idx]);
            break;
        case DUCKDB_TYPE_INTEGER:
            hugeint = convertIntegerToHugeint<std::int32_t>(((int32_t *)data)[row_idx]);
            break;
        case DUCKDB_TYPE_BIGINT:
            hugeint = convertIntegerToHugeint<std::int64_t>(((int64_t *)data)[row_idx]);
            break;
        default:
            CROW_LOG_WARNING << "Unknown internal type for decimal: " << decimal_type;
            return crow::json::wvalue(nullptr);
    }

    auto decimal = duckdb_decimal {decimal_width, decimal_scale, hugeint};
    double value = duckdb_decimal_to_double(decimal);
    
    return crow::json::wvalue(value);
}

crow::json::wvalue QueryResult::convertVectorTimestampToJson(const duckdb_vector &vector, const idx_t row_idx) {
    auto data = (duckdb_timestamp *)duckdb_vector_get_data(vector);
    auto ts = duckdb_from_timestamp(data[row_idx]);

    // Javascript ISO 8601
    std::string iso_8601 = fmt::format("{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}.{:03d}Z", 
                                        ts.date.year, ts.date.month, ts.date.day, ts.time.hour, 
                                        ts.time.min, ts.time.sec, ts.time.micros / 1000);

    return crow::json::wvalue(iso_8601);
}

crow::json::wvalue QueryResult::convertVectorDateToJson(const duckdb_vector &vector, const idx_t row_idx) {
    auto data = (duckdb_date *)duckdb_vector_get_data(vector);
    auto date = duckdb_from_date(data[row_idx]);

    std::string iso_8601 = fmt::format("{:04d}-{:02d}-{:02d}", date.year, date.month, date.day);
    return crow::json::wvalue(iso_8601);
}

crow::json::wvalue QueryResult::convertVectorTimeToJson(const duckdb_vector &vector, const idx_t row_idx) {
    auto data = (duckdb_time *)duckdb_vector_get_data(vector);
    auto time = duckdb_from_time(data[row_idx]);

    std::string iso_8601 = fmt::format("{:02d}:{:02d}:{:02d}.{:03d}", time.hour, time.min, time.sec, time.micros / 1000);
    return crow::json::wvalue(iso_8601);
}

crow::json::wvalue QueryResult::convertVectorIntervalToJson(const duckdb_vector &vector, const idx_t row_idx) {
    auto data = (duckdb_interval *)duckdb_vector_get_data(vector);
    auto interval = data[row_idx];

    std::string str = fmt::format("{:02d}:{:02d}.{:03d}", interval.months, interval.days, interval.micros / 1000);
    return crow::json::wvalue(str);
}

crow::json::wvalue QueryResult::convertVectorEnumToJson(const duckdb_vector &vector, const idx_t row_idx) {
    auto type = duckdb_vector_get_column_type(vector);

    auto data = (idx_t *)duckdb_vector_get_data(vector);
    auto value = duckdb_enum_dictionary_value(type, data[row_idx]);
    std::string str_val = value ? std::string(value) : std::string();
    duckdb_free(value);

    duckdb_destroy_logical_type(&type);
    return crow::json::wvalue(str_val);
}

crow::json::wvalue QueryResult::convertVectorListToJson(const duckdb_vector &vector, const idx_t row_idx) {
    auto child_vector = duckdb_list_vector_get_child(vector);
    auto child_size = duckdb_list_vector_get_size(vector);

    std::vector<crow::json::wvalue> result;
    for (idx_t i = 0; i < child_size; i++) {
        result.push_back(convertVectorEntryToJson(child_vector, i));
    }

    return crow::json::wvalue(result);
}

crow::json::wvalue QueryResult::convertVectorStructToJson(const duckdb_vector &vector, const idx_t row_idx) {
    auto struct_type = duckdb_vector_get_column_type(vector);
    auto child_size = duckdb_struct_type_child_count(struct_type);

    crow::json::wvalue result;

    for (idx_t i = 0; i < child_size; i++) {
        auto child_name = duckdb_struct_type_child_name(struct_type, i);
        auto str_name = std::string(child_name);
        duckdb_free(child_name);

        auto child_vector = duckdb_struct_vector_get_child(vector, i);
        result[str_name] = convertVectorEntryToJson(child_vector, 0);
    }

    duckdb_destroy_logical_type(&struct_type);
    return result;
}

} // namespace flapi