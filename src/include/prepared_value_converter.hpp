#pragma once

#include <cstdint>
#include <string>

#include "sql_parameter_classifier.hpp"

namespace flapi {

// W3.1 PR B: a tagged-union representation of a single value about to be
// bound into a DuckDB prepared statement. The kind tells the binder which
// duckdb_bind_* call to use; `Null` means "absent or empty value — bind NULL".
//
// Date / Time are stored as their broken-down components rather than a
// duckdb_date / duckdb_time so this header has no DuckDB dependency and
// can be unit-tested without a database harness.
struct PreparedValue {
    enum class Kind {
        Null,
        Int64,
        Double,
        Bool,
        Date,    // calendar date in (year, month, day)
        Time,    // wall clock in (hour, minute, second, micro)
        Varchar,
    };

    Kind kind = Kind::Null;

    std::int64_t int64_value = 0;
    double double_value = 0.0;
    bool bool_value = false;

    std::int32_t year = 0;
    std::int8_t month = 0;
    std::int8_t day = 0;

    std::int8_t hour = 0;
    std::int8_t minute = 0;
    std::int8_t second = 0;
    std::int32_t micros = 0;

    std::string varchar;
};

struct ConvertOutcome {
    bool ok = true;
    PreparedValue value;
    std::string error;   // populated iff !ok; suitable for surfacing as a 400
};

// Pure converter. Stateless; safe to construct on demand.
//
// Contract:
//  - `present == false` always yields `Null` (no error). This is the
//    "param missing from the request map" case — the validator already
//    handled `required: true`, so an absent param here means optional.
//  - `present == true` with an empty string yields `Null` for Date/Time
//    (treating empty as "not supplied") but the *empty string* for
//    Varchar (a valid value). Numeric / boolean empty inputs error.
//  - Bad numeric / date / time inputs return `ok=false` with a stable
//    error string. The caller (RequestHandler) translates that to 400.
class PreparedValueConverter {
public:
    ConvertOutcome convert(SqlParameterType type,
                           const std::string& raw,
                           bool present) const;
};

} // namespace flapi
