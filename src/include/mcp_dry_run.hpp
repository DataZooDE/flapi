#pragma once

#include <crow/json.h>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "template_secrets.hpp"
#include "prepared_template_rewriter.hpp"
#include "sql_parameter_classifier.hpp"
#include "config_manager.hpp"

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
    /// The message returned in place of the SQL when a secret cannot be
    /// scrubbed safely.
    static const char* withheldPreview();

    /// `parameters` is echoed back to the caller, so a credential-valued
    /// default is disclosed by the echo even when the SQL itself is clean.
    /// Redacted by KEY here, which is exact - unlike the SQL, the key is
    /// still available at this point.
    static std::string formatResult(const std::string& tool_name,
                                    const std::string& rendered_sql,
                                    const std::map<std::string, std::string>& parameters);

    /// The same, for SQL rendered the way the server RUNS it (prepared: a
    /// typed `{{ params.x }}` is a `?`), with the `bindings` list beside it and
    /// `parameters` typed by the fields' validators (an int field is 3, not
    /// "3"). Pasting bound values into the preview showed SQL that never ran
    /// (`ALFKI IS NULL OR`, or `( IS NULL OR c <= )` for an absent value).
    static std::string formatResult(const std::string& tool_name,
                                    const std::string& rendered_sql,
                                    const std::map<std::string, std::string>& parameters,
                                    const std::vector<PreparedBindingSpec>& bindings,
                                    const std::vector<RequestFieldConfig>& fields,
                                    const TemplateSecrets& secrets);

    /// A parameter value as JSON, typed by the field's validators.
    static crow::json::wvalue typedParameter(const std::string& value, const RequestFieldConfig* field);
    /// [{position, name, type, value}] for each `?` in order; value null when
    /// the parameter is absent (the placeholder binds NULL).
    static crow::json::wvalue bindingsJson(const std::vector<PreparedBindingSpec>& bindings,
                                           const std::map<std::string, std::string>& values,
                                           const std::vector<RequestFieldConfig>& fields,
                                           const TemplateSecrets& secrets);
};

} // namespace flapi
