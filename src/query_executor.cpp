#include "query_executor.hpp"
#include "duckdb_raii.hpp"
#include "prepared_value_converter.hpp"
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

namespace {

// W3.1 PR B helper. Binds a single PreparedValue at 1-indexed `pos` and
// throws on DuckDB error so the caller can guarantee the statement is
// destroyed in the surrounding RAII scope.
void bindOne(duckdb_prepared_statement stmt,
             idx_t pos,
             const PreparedValue& v,
             const PreparedBindingSpec& spec) {
    duckdb_state st = DuckDBSuccess;
    switch (v.kind) {
        case PreparedValue::Kind::Null:
            st = duckdb_bind_null(stmt, pos);
            break;
        case PreparedValue::Kind::Int64:
            st = duckdb_bind_int64(stmt, pos, v.int64_value);
            break;
        case PreparedValue::Kind::Double:
            st = duckdb_bind_double(stmt, pos, v.double_value);
            break;
        case PreparedValue::Kind::Bool:
            st = duckdb_bind_boolean(stmt, pos, v.bool_value);
            break;
        case PreparedValue::Kind::Date: {
            duckdb_date_struct ds;
            ds.year  = v.year;
            ds.month = v.month;
            ds.day   = v.day;
            duckdb_date d = duckdb_to_date(ds);
            st = duckdb_bind_date(stmt, pos, d);
            break;
        }
        case PreparedValue::Kind::Time: {
            duckdb_time_struct ts;
            ts.hour   = v.hour;
            ts.min    = v.minute;
            ts.sec    = v.second;
            ts.micros = v.micros;
            duckdb_time t = duckdb_to_time(ts);
            st = duckdb_bind_time(stmt, pos, t);
            break;
        }
        case PreparedValue::Kind::Varchar:
            // Use the length-aware variant so embedded NUL bytes survive
            // and are preserved as part of the bound value. The C-string
            // form would silently truncate `alice\0 OR 1=1` to `alice`,
            // letting an attacker smuggle a shorter value past length
            // validators.
            st = duckdb_bind_varchar_length(stmt, pos, v.varchar.data(), v.varchar.size());
            break;
    }
    if (st == DuckDBError) {
        throw std::runtime_error(
            "Failed to bind parameter '" + spec.field_name +
            "' at position " + std::to_string(pos));
    }
}

} // namespace

void QueryExecutor::executeWithBindings(const std::string& sql,
                                        const std::vector<PreparedBindingSpec>& bindings,
                                        const std::map<std::string, std::string>& values,
                                        const std::string& context) {
    duckdb_prepared_statement stmt = nullptr;
    if (duckdb_prepare(conn, sql.c_str(), &stmt) == DuckDBError) {
        const char* err = duckdb_prepare_error(stmt);
        std::string err_str = err ? err : "(no detail)";
        duckdb_destroy_prepare(&stmt);
        std::string context_msg = context.empty() ? "" : " during " + context;
        throw std::runtime_error("Prepare failed" + context_msg + ": " + err_str);
    }

    try {
        PreparedValueConverter converter;
        for (const auto& spec : bindings) {
            auto it = values.find(spec.field_name);
            const bool present = it != values.end();
            const std::string& raw = present ? it->second : std::string();
            auto outcome = converter.convert(spec.type, raw, present);
            if (!outcome.ok) {
                // CLIENT-input failure: the value supplied for a typed
                // request param cannot be converted to its declared SQL
                // type. RequestHandler maps BadRequestError to HTTP 400.
                throw BadRequestError(
                    "Invalid value for parameter '" + spec.field_name + "': " + outcome.error);
            }
            // DuckDB parameter indexes are 1-based.
            bindOne(stmt, spec.position + 1, outcome.value, spec);
        }
        executePrepared(stmt, context);
    } catch (...) {
        duckdb_destroy_prepare(&stmt);
        throw;
    }
    duckdb_destroy_prepare(&stmt);
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

    // DuckDB's JSON type is a logical-type alias over VARCHAR (see
    // DuckDB's LogicalType::JSON()), so `duckdb_get_type_id` returns
    // VARCHAR for it. Detect the alias before dispatching to the VARCHAR
    // handler so JSON columns are embedded as nested JSON rather than
    // emitted as escaped strings.
    bool is_json_alias = false;
    if (type_id == DUCKDB_TYPE_VARCHAR) {
        DuckDBString alias(duckdb_logical_type_get_alias(type));
        is_json_alias = !alias.is_null() && std::string(alias.get()) == "JSON";
    }
    duckdb_destroy_logical_type(&type);

    if (is_json_alias) {
        return convertVectorJsonToJson(vector, row_idx);
    }

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
            return convertVectorHugeintToJson(vector, row_idx);
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
            return convertVectorUhugeintToJson(vector, row_idx);
        case DUCKDB_TYPE_BLOB:
            return convertVectorBlobToJson(vector, row_idx);
        case DUCKDB_TYPE_UUID:
            return convertVectorUuidToJson(vector, row_idx);
        case DUCKDB_TYPE_BIT:
            return convertVectorBitToJson(vector, row_idx);
        case DUCKDB_TYPE_MAP:
            return convertVectorMapToJson(vector, row_idx);
        case DUCKDB_TYPE_ARRAY:
            return convertVectorArrayToJson(vector, row_idx);
        case DUCKDB_TYPE_UNION:
            return convertVectorUnionToJson(vector, row_idx);
        default:
            CROW_LOG_WARNING << "Unknown type: " << type_id;
            return crow::json::wvalue(nullptr);
    }
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

crow::json::wvalue QueryResult::convertVectorJsonToJson(const duckdb_vector &vector, const idx_t row_idx) {
    auto validity = duckdb_vector_get_validity(vector);
    if (!duckdb_validity_row_is_valid(validity, row_idx)) {
        return crow::json::wvalue(nullptr);
    }

    auto data = (duckdb_string_t *)duckdb_vector_get_data(vector);
    const char* str_ptr = duckdb_string_is_inlined(data[row_idx])
                        ? data[row_idx].value.inlined.inlined
                        : (const char*)data[row_idx].value.pointer.ptr;
    const idx_t str_len = duckdb_string_is_inlined(data[row_idx])
                        ? data[row_idx].value.inlined.length
                        : data[row_idx].value.pointer.length;

    auto parsed = crow::json::load(str_ptr, str_len);
    if (!parsed) {
        // Source row contains malformed JSON; degrade to the raw string
        // rather than dropping the row or returning null. The cell stays
        // queryable, just as a string.
        return crow::json::wvalue(std::string(str_ptr, str_len));
    }
    return crow::json::wvalue(parsed);
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
    duckdb_destroy_logical_type(&type);
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
    DuckDBString value_wrapper(duckdb_enum_dictionary_value(type, data[row_idx]));
    std::string str_val = value_wrapper.to_string();

    duckdb_destroy_logical_type(&type);
    return crow::json::wvalue(str_val);
}

crow::json::wvalue QueryResult::convertVectorUuidToJson(const duckdb_vector &vector, const idx_t row_idx) {
    auto validity = duckdb_vector_get_validity(vector);
    if (!duckdb_validity_row_is_valid(validity, row_idx)) {
        return crow::json::wvalue(nullptr);
    }

    // UUID is physically a 128-bit integer (hugeint), NOT a string — reading
    // it via the VARCHAR path dereferences garbage and crashes (issue #89).
    // Format it the way DuckDB's UUID::ToString does: flip the stored MSB
    // back, then emit the canonical 8-4-4-4-12 lowercase hex form.
    auto data = static_cast<duckdb_hugeint *>(duckdb_vector_get_data(vector));
    auto value = data[row_idx];
    uint64_t upper = static_cast<uint64_t>(value.upper) ^ (uint64_t(1) << 63);
    uint64_t lower = value.lower;

    static const char *HEX = "0123456789abcdef";
    char buf[36];
    idx_t pos = 0;
    auto byte_to_hex = [&](uint64_t byte_val) {
        buf[pos++] = HEX[(byte_val >> 4) & 0xf];
        buf[pos++] = HEX[byte_val & 0xf];
    };
    byte_to_hex((upper >> 56) & 0xFF);
    byte_to_hex((upper >> 48) & 0xFF);
    byte_to_hex((upper >> 40) & 0xFF);
    byte_to_hex((upper >> 32) & 0xFF);
    buf[pos++] = '-';
    byte_to_hex((upper >> 24) & 0xFF);
    byte_to_hex((upper >> 16) & 0xFF);
    buf[pos++] = '-';
    byte_to_hex((upper >> 8) & 0xFF);
    byte_to_hex(upper & 0xFF);
    buf[pos++] = '-';
    byte_to_hex((lower >> 56) & 0xFF);
    byte_to_hex((lower >> 48) & 0xFF);
    buf[pos++] = '-';
    byte_to_hex((lower >> 40) & 0xFF);
    byte_to_hex((lower >> 32) & 0xFF);
    byte_to_hex((lower >> 24) & 0xFF);
    byte_to_hex((lower >> 16) & 0xFF);
    byte_to_hex((lower >> 8) & 0xFF);
    byte_to_hex(lower & 0xFF);

    return crow::json::wvalue(std::string(buf, pos));
}

crow::json::wvalue QueryResult::convertVectorHugeintToJson(const duckdb_vector &vector, const idx_t row_idx) {
    auto validity = duckdb_vector_get_validity(vector);
    if (!duckdb_validity_row_is_valid(validity, row_idx)) {
        return crow::json::wvalue(nullptr);
    }

    // HUGEINT is 128-bit: reading it as int64 truncates and mis-strides for
    // multi-row chunks (issue #89). Emit the exact decimal as a JSON string so
    // values beyond 2^63 survive (JSON consumers lose precision above 2^53).
    auto data = static_cast<duckdb_hugeint *>(duckdb_vector_get_data(vector));
    auto value = duckdb_create_hugeint(data[row_idx]);
    DuckDBString str(duckdb_value_to_string(value));
    duckdb_destroy_value(&value);
    return crow::json::wvalue(str.to_string());
}

crow::json::wvalue QueryResult::convertVectorUhugeintToJson(const duckdb_vector &vector, const idx_t row_idx) {
    auto validity = duckdb_vector_get_validity(vector);
    if (!duckdb_validity_row_is_valid(validity, row_idx)) {
        return crow::json::wvalue(nullptr);
    }

    // UHUGEINT is unsigned 128-bit; emit as a decimal string for the same
    // precision reasons as HUGEINT.
    auto data = static_cast<duckdb_uhugeint *>(duckdb_vector_get_data(vector));
    auto value = duckdb_create_uhugeint(data[row_idx]);
    DuckDBString str(duckdb_value_to_string(value));
    duckdb_destroy_value(&value);
    return crow::json::wvalue(str.to_string());
}

crow::json::wvalue QueryResult::convertVectorBlobToJson(const duckdb_vector &vector, const idx_t row_idx) {
    auto validity = duckdb_vector_get_validity(vector);
    if (!duckdb_validity_row_is_valid(validity, row_idx)) {
        return crow::json::wvalue(nullptr);
    }

    // A BLOB's bytes are arbitrary binary; emitting them raw via the VARCHAR
    // path can produce invalid UTF-8 / invalid JSON. Render DuckDB's own blob
    // string form (printable bytes as-is, others as \xNN), matching to_json /
    // CAST AS VARCHAR. (duckdb_value_to_string would add a '...'::BLOB wrapper.)
    auto data = static_cast<duckdb_string_t *>(duckdb_vector_get_data(vector));
    auto length = duckdb_string_is_inlined(data[row_idx])
                ? data[row_idx].value.inlined.length
                : data[row_idx].value.pointer.length;
    auto bytes = duckdb_string_is_inlined(data[row_idx])
               ? reinterpret_cast<const uint8_t *>(data[row_idx].value.inlined.inlined)
               : reinterpret_cast<const uint8_t *>(data[row_idx].value.pointer.ptr);

    static const char *HEX = "0123456789ABCDEF";
    std::string out;
    out.reserve(length);
    for (idx_t i = 0; i < length; i++) {
        uint8_t c = bytes[i];
        bool regular = c >= 32 && c <= 126 && c != '\\' && c != '\'' && c != '"';
        if (regular) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('\\');
            out.push_back('x');
            out.push_back(HEX[(c >> 4) & 0xF]);
            out.push_back(HEX[c & 0xF]);
        }
    }
    return crow::json::wvalue(out);
}

crow::json::wvalue QueryResult::convertVectorBitToJson(const duckdb_vector &vector, const idx_t row_idx) {
    auto validity = duckdb_vector_get_validity(vector);
    if (!duckdb_validity_row_is_valid(validity, row_idx)) {
        return crow::json::wvalue(nullptr);
    }

    // BIT is stored as its internal bitstring blob; the VARCHAR path renders
    // that raw storage (padding byte + bytes) as garbage. Round-trip through a
    // BIT value so it serializes as a "0101" string, matching to_json.
    auto data = static_cast<duckdb_string_t *>(duckdb_vector_get_data(vector));
    auto length = duckdb_string_is_inlined(data[row_idx])
                ? data[row_idx].value.inlined.length
                : data[row_idx].value.pointer.length;
    auto bytes = duckdb_string_is_inlined(data[row_idx])
               ? reinterpret_cast<const uint8_t *>(data[row_idx].value.inlined.inlined)
               : reinterpret_cast<const uint8_t *>(data[row_idx].value.pointer.ptr);

    duckdb_bit bit { const_cast<uint8_t *>(bytes), length };
    auto value = duckdb_create_bit(bit);
    DuckDBString str(duckdb_value_to_string(value));
    duckdb_destroy_value(&value);
    return crow::json::wvalue(str.to_string());
}

crow::json::wvalue QueryResult::convertVectorListToJson(const duckdb_vector &vector, const idx_t row_idx) {
    auto validity = duckdb_vector_get_validity(vector);
    if (!duckdb_validity_row_is_valid(validity, row_idx)) {
        return crow::json::wvalue(nullptr);
    }

    // A LIST column stores every row's elements back-to-back in a single child
    // vector. The per-row slice is described by the list_entry at `row_idx`
    // (offset + length); emitting the whole child vector instead would repeat
    // every other row's elements (issue #89).
    auto entries = static_cast<duckdb_list_entry *>(duckdb_vector_get_data(vector));
    auto entry = entries[row_idx];
    auto child_vector = duckdb_list_vector_get_child(vector);
    auto child_size = duckdb_list_vector_get_size(vector);

    std::vector<crow::json::wvalue> result;
    for (idx_t i = 0; i < entry.length; i++) {
        idx_t child_idx = entry.offset + i;
        if (child_idx >= child_size) {
            break;
        }
        result.push_back(convertVectorEntryToJson(child_vector, child_idx));
    }

    return crow::json::wvalue(result);
}

crow::json::wvalue QueryResult::convertVectorArrayToJson(const duckdb_vector &vector, const idx_t row_idx) {
    auto validity = duckdb_vector_get_validity(vector);
    if (!duckdb_validity_row_is_valid(validity, row_idx)) {
        return crow::json::wvalue(nullptr);
    }

    // A fixed-size ARRAY has no per-row list_entry: every row occupies a
    // constant `array_size` run in the child vector, so this row's slice
    // starts at row_idx * array_size (unlike LIST, which carries offsets).
    auto array_type = duckdb_vector_get_column_type(vector);
    auto array_size = duckdb_array_type_array_size(array_type);
    duckdb_destroy_logical_type(&array_type);

    auto child_vector = duckdb_array_vector_get_child(vector);

    std::vector<crow::json::wvalue> result;
    for (idx_t i = 0; i < array_size; i++) {
        result.push_back(convertVectorEntryToJson(child_vector, row_idx * array_size + i));
    }

    return crow::json::wvalue(result);
}

crow::json::wvalue QueryResult::convertVectorStructToJson(const duckdb_vector &vector, const idx_t row_idx) {
    auto validity = duckdb_vector_get_validity(vector);
    if (!duckdb_validity_row_is_valid(validity, row_idx)) {
        return crow::json::wvalue(nullptr);
    }

    auto struct_type = duckdb_vector_get_column_type(vector);
    auto child_size = duckdb_struct_type_child_count(struct_type);

    crow::json::wvalue result;

    for (idx_t i = 0; i < child_size; i++) {
        DuckDBString child_name_wrapper(duckdb_struct_type_child_name(struct_type, i));
        auto str_name = child_name_wrapper.to_string();

        // Each struct field is a parallel child vector; read this row's entry
        // (`row_idx`), not element 0, so multi-row results and structs nested
        // inside a list resolve the correct element (issue #89).
        auto child_vector = duckdb_struct_vector_get_child(vector, i);
        result[str_name] = convertVectorEntryToJson(child_vector, row_idx);
    }

    duckdb_destroy_logical_type(&struct_type);
    return result;
}

crow::json::wvalue QueryResult::convertVectorUnionToJson(const duckdb_vector &vector, const idx_t row_idx) {
    auto validity = duckdb_vector_get_validity(vector);
    if (!duckdb_validity_row_is_valid(validity, row_idx)) {
        return crow::json::wvalue(nullptr);
    }

    // A UNION is physically a STRUCT: child 0 is the tag (uint8_t), children
    // 1..n are the candidate members. Only the member selected by the row's
    // tag is valid, so emit just that member as {name: value} (matching
    // DuckDB's own to_json) rather than every member, which the generic
    // struct path did, exposing inactive members.
    auto union_type = duckdb_vector_get_column_type(vector);
    auto member_count = duckdb_union_type_member_count(union_type);

    auto tag_vector = duckdb_struct_vector_get_child(vector, 0);
    auto tags = static_cast<uint8_t *>(duckdb_vector_get_data(tag_vector));
    idx_t member_idx = static_cast<idx_t>(tags[row_idx]);
    if (member_idx >= member_count) {
        // Out-of-range tag should not happen for well-formed unions; fail
        // safe rather than reading past the struct children.
        duckdb_destroy_logical_type(&union_type);
        return crow::json::wvalue(nullptr);
    }

    DuckDBString member_name_wrapper(duckdb_union_type_member_name(union_type, member_idx));
    auto member_name = member_name_wrapper.to_string();
    duckdb_destroy_logical_type(&union_type);

    // Member i lives at struct child index i + 1 (the tag occupies slot 0).
    auto member_vector = duckdb_struct_vector_get_child(vector, member_idx + 1);

    crow::json::wvalue result;
    result[member_name] = convertVectorEntryToJson(member_vector, row_idx);
    return result;
}

std::string QueryResult::vectorEntryToMapKey(const duckdb_vector &vector, const idx_t row_idx) {
    auto type = duckdb_vector_get_column_type(vector);
    bool is_string = duckdb_get_type_id(type) == DUCKDB_TYPE_VARCHAR;
    duckdb_destroy_logical_type(&type);

    auto validity = duckdb_vector_get_validity(vector);
    if (!duckdb_validity_row_is_valid(validity, row_idx)) {
        return std::string();
    }

    if (is_string) {
        auto data = static_cast<duckdb_string_t *>(duckdb_vector_get_data(vector));
        return duckdb_string_is_inlined(data[row_idx])
             ? std::string(data[row_idx].value.inlined.inlined, data[row_idx].value.inlined.length)
             : std::string(static_cast<const char *>(data[row_idx].value.pointer.ptr), data[row_idx].value.pointer.length);
    }

    // Non-string key: render the scalar and use its JSON form as the object
    // key (e.g. integer 10 -> "10"), matching DuckDB's to_json for scalar
    // keys. Strip the quotes a string-like rendering (date/UUID/etc.) adds.
    // Composite keys (STRUCT/LIST) are rare and fall back to their JSON
    // rendering rather than DuckDB's VARCHAR cast.
    auto rendered = convertVectorEntryToJson(vector, row_idx);
    auto dumped = rendered.dump();
    if (dumped.size() >= 2 && dumped.front() == '"' && dumped.back() == '"') {
        return dumped.substr(1, dumped.size() - 2);
    }
    return dumped;
}

crow::json::wvalue QueryResult::convertVectorMapToJson(const duckdb_vector &vector, const idx_t row_idx) {
    auto validity = duckdb_vector_get_validity(vector);
    if (!duckdb_validity_row_is_valid(validity, row_idx)) {
        return crow::json::wvalue(nullptr);
    }

    // A MAP is physically LIST(STRUCT(key, value)). Slice the row's entries
    // from the list child via its duckdb_list_entry, then emit a JSON object
    // {key: value} (DuckDB's own to_json shape). A non-null but empty map
    // serializes as {} rather than null.
    auto entries = static_cast<duckdb_list_entry *>(duckdb_vector_get_data(vector));
    auto entry = entries[row_idx];

    auto kv_struct = duckdb_list_vector_get_child(vector);
    auto child_size = duckdb_list_vector_get_size(vector);
    auto key_vector = duckdb_struct_vector_get_child(kv_struct, 0);
    auto value_vector = duckdb_struct_vector_get_child(kv_struct, 1);

    crow::json::wvalue result = crow::json::wvalue::empty_object();
    for (idx_t i = 0; i < entry.length; i++) {
        idx_t child_idx = entry.offset + i;
        if (child_idx >= child_size) {
            break;
        }
        result[vectorEntryToMapKey(key_vector, child_idx)] = convertVectorEntryToJson(value_vector, child_idx);
    }

    return result;
}

} // namespace flapi