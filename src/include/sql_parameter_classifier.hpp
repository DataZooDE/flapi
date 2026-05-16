#pragma once

#include <string>

namespace flapi {

struct RequestFieldConfig;

// W3.1 (PR A): how a typed parameter should be bound into a DuckDB
// prepared statement. The classifier maps a `RequestFieldConfig` to one
// of these — `PreparedTemplateRewriter` then uses the result to decide
// whether a `{{ params.X }}` occurrence should become a `?` placeholder.
enum class SqlParameterType {
    Integer,   // duckdb_bind_int64
    Double,    // duckdb_bind_double
    Boolean,   // duckdb_bind_boolean
    Date,      // duckdb_bind_date  (caller parses yyyy-mm-dd)
    Time,      // duckdb_bind_time  (caller parses hh:mm:ss)
    Varchar,   // duckdb_bind_varchar  (default for string-shaped values)
};

struct Bindability {
    bool bindable = false;
    SqlParameterType type = SqlParameterType::Varchar;
};

// Pure helper. Stateless; safe to construct on demand at the call site.
//
// The classification rule is intentionally conservative: a field without
// any typed validator is NOT considered bindable, so the Mustache path
// remains the safe default during migration. Unknown validator type
// names also fall back to non-bindable for forward compatibility — a
// new validator added in a later version doesn't accidentally route
// through the prepared path before its binder has been written.
class SqlParameterClassifier {
public:
    Bindability classify(const RequestFieldConfig& field) const;
};

} // namespace flapi
