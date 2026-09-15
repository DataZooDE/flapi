#pragma once

#include <string>

namespace flapi {

// ISO-8601 UTC timestamp with microsecond precision.
//
// Factored out because audit_logger.cpp and flapi_log_handler.cpp both need it
// and the duplicate in the log handler omitted the _WIN32 guard - MSVC has no
// gmtime_r, so it was a hard build break on the x64-windows-static target.
std::string nowIso8601Utc();

}  // namespace flapi
