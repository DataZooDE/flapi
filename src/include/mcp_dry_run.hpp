#pragma once

#include <crow/json.h>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

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
    /// Replace credential-valued connection properties wherever they appear
    /// in `sql`. The rendered SQL is returned to the caller verbatim, and a
    /// template that interpolates `{{{ conn.password }}}` therefore handed the
    /// credential back in the dry-run payload - over MCP, which is
    /// unauthenticated by default. Scrubbing by VALUE, not by key, because by
    /// the time the SQL is rendered the key is gone.
    /// Every credential-valued thing a dry-run preview could disclose,
    /// gathered from ALL the sources a template can interpolate.
    ///
    /// Scrubbing used to cover `conn.*` only. A template may equally write
    /// `{{{ env.API_KEY }}}` - the documented environment-variable pattern -
    /// and a request field's configured `default:` is copied into params and
    /// returned in the payload's `parameters` object. Both came back verbatim
    /// to an unauthenticated _dryRun caller. Enumerating the sources in one
    /// place is what stops the next one being missed.
    class Secrets {
    public:
        /// Record `value` as secret if `key` names a credential. A value too
        /// short to replace without corrupting unrelated SQL sets
        /// `withhold()` instead - the same rule the short-connection-secret
        /// case already used: neither leak it nor mangle the preview.
        void add(const std::string& key, const std::string& value);

        template <typename Map>
        void addAll(const Map& entries) {
            for (const auto& entry : entries) {
                add(entry.first, entry.second);
            }
        }

        bool withhold() const { return withhold_; }
        const std::vector<std::string>& values() const { return values_; }

    private:
        bool withhold_ = false;
        std::vector<std::string> values_;
    };

    /// True when a connection holds a credential too short to scrub by value.
    /// The rendered SQL must then be withheld rather than returned.
    static bool hasUnscrubbableCredential(
        const std::unordered_map<std::string, std::string>& connection_properties);

    static std::string scrubConnectionSecrets(
        std::string sql,
        const std::unordered_map<std::string, std::string>& connection_properties);

    /// Replace every value in `secrets` wherever it appears in `sql`.
    static std::string scrub(std::string sql, const Secrets& secrets);

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
};

} // namespace flapi
