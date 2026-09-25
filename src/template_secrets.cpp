#include "template_secrets.hpp"

#include "config_manager.hpp"
#include "redaction.hpp"
#include "sql_template_processor.hpp"

#include <algorithm>

namespace flapi {

namespace {
// Below this length a value cannot be replaced without risking unrelated text.
constexpr std::size_t kMinScrubbableSecret = 4;

// Above this length, a server-configured value - a whitelisted environment
// variable, a connection property - is treated as a secret whatever it is
// called. Chosen so ordinary configuration - a region, a bucket name, a
// hostname, a tuning knob - stays visible, while API keys, tokens and
// service-account blobs do not.
constexpr std::size_t kOpaqueEnvValue = 24;

/// True for a location that demonstrably carries no credential.
///
/// The first version exempted ANY value containing '/' or '://', which is
/// precisely the shape the credentials cloud deployments actually use:
///
///   postgresql://alice:hunter2@db/prod          (userinfo)
///   https://acct.blob.core.windows.net/c/f?sig=…  (SAS)
///   https://bucket.s3.amazonaws.com/k?X-Amz-Signature=…  (presigned)
///
/// None of `database`, `url` or `endpoint` matches a credential stem, so the
/// exemption let all three through to dry-run previews and error messages -
/// re-opening the disclosure the opaque-value rule exists to close. A unit
/// test even pinned it as desired behaviour.
///
/// The exemption is now the narrow case it was meant to be: a plain path, or
/// a URI with no userinfo and no query string. Anything else is treated as a
/// secret, because a secret is what it usually is.
bool looksLikeCredentialFreeLocation(const std::string& value) {
    if (value.find('?') != std::string::npos ||
        value.find('#') != std::string::npos) {
        return false;   // a query or fragment can carry a signature or token
    }

    const auto scheme = value.find("://");
    if (scheme == std::string::npos) {
        // A bare path. No authority, so no userinfo to hide a password in.
        return value.find('/') != std::string::npos ||
               value.find('\\') != std::string::npos;
    }
    if (scheme >= 12) {
        return false;   // not a scheme, just a value that happens to contain "://"
    }

    const auto authority = scheme + 3;
    const auto authority_end = value.find('/', authority);
    const auto authority_part = value.substr(
        authority, authority_end == std::string::npos ? std::string::npos
                                                      : authority_end - authority);
    if (authority_part.find('@') != std::string::npos) {
        return false;   // user:password@host
    }
    return true;
}

/// The three questions a provenance answers.
struct SourcePolicy {
    /// A credential-NAMED value too short to replace safely suppresses the
    /// whole output rather than being leaked or mangled.
    ///
    /// Measured: a one-byte secret "a" rewrote the middle of an unrelated file
    /// path. Withhold rather than leak or mangle.
    ///
    /// NOT for a value the caller supplied, though. Applying this to request
    /// params handed every caller a denial-of-diagnostics switch: `?token=ab`
    /// blanked every error the endpoint could produce - validation errors
    /// included - on REST, tools/call and resources/read alike. A value the
    /// caller sent is not a secret being kept FROM them. It is still recorded
    /// for scrubbing when it is long enough to replace; it just never
    /// withholds.
    bool withhold_when_too_short;

    /// A long value is a secret whatever its NAME suggests.
    ///
    /// The name heuristic is the right gate for a request default - a
    /// `default: "100"` on a `limit` field must stay visible or the preview is
    /// useless. It is the wrong gate for a server-configured value: an
    /// operator chose to expose each whitelisted environment variable and each
    /// connection property to templates, and a name like PAYMENT_VALUE,
    /// SERVICE_ACCOUNT_JSON, `service_account_json` or `sas` carries a secret
    /// past any stem list.
    bool opaque_value_rule;

    /// Exempt a value that is plainly a path or a credential-free URI from the
    /// opaque-value rule, because `path` and `database` are the properties
    /// operators read a preview to check. Connection properties only; it
    /// widens what stays visible and never narrows what is redacted, since a
    /// credential-NAMED value is recorded before this is consulted.
    bool exempt_credential_free_locations;
};

/// THE policy. One row per provenance, so that all four are readable side by
/// side - four near-identical entry points, each answering one review finding
/// in isolation, is the shape that produced a fifth inconsistency.
///
///   source            | withholds on a short value | opaque-value rule
///   ------------------+----------------------------+----------------------
///   Connection        | yes                        | yes, minus locations
///   Environment       | yes                        | yes
///   ConfiguredDefault | yes                        | no
///   Caller            | NO - see above             | no
constexpr SourcePolicy policyFor(TemplateSecrets::Source source) {
    switch (source) {
    case TemplateSecrets::Source::Connection:
        return {/*withhold_when_too_short=*/true, /*opaque_value_rule=*/true,
                /*exempt_credential_free_locations=*/true};
    case TemplateSecrets::Source::Environment:
        return {/*withhold_when_too_short=*/true, /*opaque_value_rule=*/true,
                /*exempt_credential_free_locations=*/false};
    case TemplateSecrets::Source::ConfiguredDefault:
        return {/*withhold_when_too_short=*/true, /*opaque_value_rule=*/false,
                /*exempt_credential_free_locations=*/false};
    case TemplateSecrets::Source::Caller:
        return {/*withhold_when_too_short=*/false, /*opaque_value_rule=*/false,
                /*exempt_credential_free_locations=*/false};
    }
    // Unreachable for a declared enumerator. A new one gets the strictest
    // policy until its row is written, so adding a source cannot silently
    // disclose.
    return {/*withhold_when_too_short=*/true, /*opaque_value_rule=*/true,
            /*exempt_credential_free_locations=*/false};
}

}  // namespace

void TemplateSecrets::add(const std::string& key, const std::string& value,
                          Source source) {
    if (value.empty()) {
        return;
    }
    const SourcePolicy policy = policyFor(source);

    if (isCredentialKey(key)) {
        if (value.size() < kMinScrubbableSecret) {
            // Too short to replace without risking unrelated text, so it is
            // never recorded; whether that suppresses the output instead is
            // the one thing provenance decides here.
            if (policy.withhold_when_too_short) {
                withhold_ = true;
            }
            return;
        }
        record(value);
        return;
    }

    // The name says nothing. Only a source carrying the opaque-value rule
    // records it, and only past the length where ordinary configuration ends.
    if (policy.opaque_value_rule && value.size() >= kOpaqueEnvValue &&
        !(policy.exempt_credential_free_locations &&
          looksLikeCredentialFreeLocation(value))) {
        record(value);
    }
}

void TemplateSecrets::record(const std::string& value) {
    if (std::find(values_.begin(), values_.end(), value) == values_.end()) {
        values_.push_back(value);
    }
}

void TemplateSecrets::add(const std::string& key, const std::string& value) {
    add(key, value, Source::ConfiguredDefault);
}

void TemplateSecrets::addCallerSupplied(const std::string& key, const std::string& value) {
    add(key, value, Source::Caller);
}

void TemplateSecrets::addEnv(const std::string& key, const std::string& value) {
    add(key, value, Source::Environment);
}

void TemplateSecrets::addConnectionProperty(const std::string& key,
                                            const std::string& value) {
    add(key, value, Source::Connection);
}

std::string TemplateSecrets::scrub(std::string text) const {
    // LONGEST first. Replacement was insertion-ordered, so a short secret
    // that is a prefix of a longer one consumed its start and left the tail
    // in the output: with `access_key=sk-live-` recorded before
    // `password=sk-live-supersecret`, scrubbing produced
    // `<redacted>supersecret`.
    std::vector<std::string> ordered = values_;
    std::sort(ordered.begin(), ordered.end(),
              [](const std::string& a, const std::string& b) {
                  return a.size() > b.size();
              });
    for (const auto& value : ordered) {
        std::string::size_type pos = 0;
        while ((pos = text.find(value, pos)) != std::string::npos) {
            text.replace(pos, value.size(), "<redacted>");
            pos += sizeof("<redacted>") - 1;
        }
    }
    return text;
}

TemplateSecrets collectTemplateSecrets(ConfigManager* config_manager,
                                       const EndpointConfig& endpoint,
                                       const std::map<std::string, std::string>& params) {
    TemplateSecrets secrets;
    if (config_manager != nullptr) {
        const auto& connections = config_manager->getConnections();
        for (const auto& conn_name : endpoint.connection) {
            const auto it = connections.find(conn_name);
            if (it != connections.end()) {
                for (const auto& [key, value] : it->second.properties) {
                    secrets.addConnectionProperty(key, value);
                }
            }
        }

        // Only the variables the template layer actually exposes - the same
        // whitelist SQLTemplateProcessor applies, so this neither under- nor
        // over-scrubs. Read here rather than taken from a
        // SQLTemplateProcessor, so every caller can use this: RequestHandler
        // holds no template processor of its own.
        const auto& template_config = config_manager->getTemplateConfig();
        for (const auto& [key, value] : SQLTemplateProcessor::getEnvironmentVariables()) {
            if (template_config.isEnvironmentVariableAllowed(key)) {
                secrets.addEnv(key, value);
            }
        }
    }
    // The DuckLake catalog's own paths. A metadata-path is frequently a DSN
    // with inline credentials (`postgres://user:pw@host/db`) or an object
    // URL with a signature, and a failed cache refresh quotes the statement
    // that attached it - so these reach error messages exactly like a
    // connection property does, and get the same treatment.
    if (config_manager != nullptr) {
        const auto& ducklake = config_manager->getDuckLakeConfig();
        secrets.addConnectionProperty("metadata-path", ducklake.metadata_path);
        secrets.addConnectionProperty("data-path", ducklake.data_path);
    }

    // Params are caller-supplied: scrubbed, never a reason to withhold.
    // A configured `default:` that the caller did not override is
    // server-sourced, so those are added as server values first.
    for (const auto& field : endpoint.request_fields) {
        if (field.defaultValue.empty()) {
            continue;
        }
        const auto it = params.find(field.fieldName);
        if (it != params.end() && it->second == field.defaultValue) {
            secrets.add(field.fieldName, field.defaultValue);
        }
    }
    secrets.addAllCallerSupplied(params);
    return secrets;
}

std::string publicErrorMessage(const std::string& context,
                               const std::string& detail,
                               const TemplateSecrets& secrets) {
    if (secrets.withhold()) {
        // Something reachable from this template is too short to scrub
        // safely, so no part of the failure text can be shown.
        return context + ": the server could not describe this failure without "
                         "risking disclosure; see the server log.";
    }
    return context + ": " + secrets.scrub(detail);
}

}  // namespace flapi
