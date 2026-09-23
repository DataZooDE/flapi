#pragma once

#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace flapi {

class ConfigManager;
struct EndpointConfig;

/// Every credential-valued thing an endpoint's SQL template can interpolate.
///
/// A template can reach three things - `conn.*`, `env.*` and `params.*` - and
/// each of them has, at some point, been returned verbatim to a caller:
/// through the MCP dry-run preview, through the `parameters` echo beside it,
/// and through database error messages, which quote the failing statement.
///
/// Collecting them in ONE place is the fix. Scrubbing one source and calling
/// it done is how this kept recurring; so is fixing one of the paths that
/// returns them.
class TemplateSecrets {
public:
    /// Record `value` as secret if `key` names a credential. A value too
    /// short to replace without corrupting unrelated SQL sets `withhold()`
    /// instead: neither leak it nor mangle the output around it.
    void add(const std::string& key, const std::string& value);

    template <typename Map>
    void addAll(const Map& entries) {
        for (const auto& entry : entries) {
            add(entry.first, entry.second);
        }
    }

    /// Like add(), but also records a long value whose NAME does not look
    /// like a credential.
    ///
    /// The name heuristic is the right gate for a request default - a
    /// `default: "100"` on a `limit` field must stay visible or the preview
    /// is useless. It is the wrong gate for a whitelisted environment
    /// variable: those are server-configured, an operator chose to expose
    /// each one to templates, and a name like PAYMENT_VALUE or
    /// SERVICE_ACCOUNT_JSON carries a secret past any stem list.
    void addEnv(const std::string& key, const std::string& value);

    bool withhold() const { return withhold_; }
    const std::vector<std::string>& values() const { return values_; }

    /// Replace every recorded value wherever it appears in `text`.
    std::string scrub(std::string text) const;

private:
    bool withhold_ = false;
    std::vector<std::string> values_;
};

/// Gather the secrets reachable from `endpoint`'s template: its connections'
/// credential properties, the whitelisted environment variables the template
/// layer exposes, and the (possibly default-sourced) request params.
TemplateSecrets collectTemplateSecrets(ConfigManager* config_manager,
                                       const EndpointConfig& endpoint,
                                       const std::map<std::string, std::string>& params);

/// The message returned to a caller in place of a failure's own text.
///
/// A database error quotes the statement that failed, which is the rendered
/// template - so it carries whatever the template interpolated. The detail
/// belongs in the server log, not in the response.
///
/// Deliberately NOT the dry-run "withheld" wording: that message describes a
/// preview, and reusing it here let any caller suppress all diagnostics for a
/// call simply by passing a short value under a credential-shaped name.
std::string publicErrorMessage(const std::string& context,
                               const std::string& detail,
                               const TemplateSecrets& secrets);

}  // namespace flapi
