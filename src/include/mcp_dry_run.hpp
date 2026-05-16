#pragma once

#include <crow/json.h>
#include <map>
#include <string>

namespace flapi {

// Helpers for W2.2 dry-run / shadow mode. The model is:
//   1. Caller sends `_dryRun: true` alongside the normal tool arguments.
//   2. MCPToolHandler peels the flag off (no validation impact downstream).
//   3. After auth + argument validation + template rendering, the handler
//      returns the rendered SQL instead of executing it.
//
// MCPDryRun groups the flag-stripping helper and the result formatter so
// they can be unit-tested in isolation without spinning up a server.
class MCPDryRun {
public:
    static constexpr const char* kFlagKey = "_dryRun";

    // If `arguments` contains the reserved `_dryRun` key, strip it and return
    // its boolean value. Non-boolean values are treated as false but still
    // stripped, so a hostile caller can't smuggle the key into validation.
    static bool extractFlag(crow::json::wvalue& arguments);

    // Render the dry-run JSON payload returned to the agent in place of real
    // query results. Always emits a `parameters` object (possibly empty) so
    // callers don't need to special-case missing args.
    static std::string formatResult(const std::string& tool_name,
                                    const std::string& rendered_sql,
                                    const std::map<std::string, std::string>& parameters);
};

} // namespace flapi
