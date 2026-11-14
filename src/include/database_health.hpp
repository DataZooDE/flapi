#pragma once

#include <optional>
#include <string>

namespace flapi {

// Process-wide record of a DuckDB instance that has been INVALIDATED.
//
// After an INTERNAL error DuckDB marks the whole database invalid, and every
// later query on any connection fails with "FATAL Error: ... database has been
// invalidated ...". Nothing short of a restart recovers it. flAPI used to keep
// answering /health 200 and turn each request into a 500, so a supervisor
// never restarted it. DuckDB errors flAPI raises pass through
// NoteDatabaseError(); the first one that reports an invalidation sets this
// flag, and /health, /health/live, data and config-service requests answer 503
// from then on.
class DatabaseHealth {
public:
    // Looks at a DuckDB error message; on an invalidation records it (first one
    // wins) and logs FATAL once. Cheap enough for every error path.
    static void NoteDatabaseError(const std::string& error_message);

    // True for a message that reports an invalidated database.
    static bool IsInvalidationError(const std::string& error_message);

    static bool IsFatal();
    // The recorded cause (the DuckDB message, clamped), when fatal.
    static std::optional<std::string> FatalReason();

    // The text every 503 a fatal database causes carries.
    static std::string FatalMessage();
    // {"error":"database_invalidated","message":...} - the body of a data or
    // config-service request refused because the database is invalidated. No
    // `reason`: the DuckDB text can quote SQL; /health carries the cause.
    static std::string FatalResponseJson();

    // Tests only.
    static void ResetForTesting();
};

} // namespace flapi
