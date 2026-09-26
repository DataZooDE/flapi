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
    /// Where a recorded value came from.
    ///
    /// The four sources differ in PROVENANCE, not in shape, and provenance is
    /// what decides how strictly a value is treated. That decision is made in
    /// exactly one place - the policy table at the top of
    /// template_secrets.cpp - because three of this area's defects were "the
    /// rule was applied at one call site and not at its sibling".
    enum class Source {
        /// A connection property out of flapi.yaml.
        Connection,
        /// A whitelisted environment variable the template layer exposes.
        Environment,
        /// A request field's configured `default:` the caller did not override.
        ConfiguredDefault,
        /// A value the caller sent with the request.
        Caller,
    };

    /// Record `value` as secret, under the policy its `source` earns.
    ///
    /// Common to every source: an empty value is nothing, and a value whose
    /// NAME names a credential is a secret. What differs per source is whether
    /// a value too short to replace safely sets withhold() instead, and
    /// whether a long value is a secret whatever it is called. The differences
    /// are tabulated in template_secrets.cpp; that table is the whole policy.
    void add(const std::string& key, const std::string& value, Source source);

    /// Record every entry of `entries`, all from the same `source`.
    template <typename Map>
    void addAll(const Map& entries, Source source) {
        for (const auto& entry : entries) {
            add(entry.first, entry.second, source);
        }
    }

    bool withhold() const { return withhold_; }
    const std::vector<std::string>& values() const { return values_; }

    /// Replace every recorded value wherever it appears in `text`.
    std::string scrub(std::string text) const;

private:
    /// Record `value` once. add() has already decided it is a secret.
    void record(const std::string& value);

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
