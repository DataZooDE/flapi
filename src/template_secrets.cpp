#include "template_secrets.hpp"

#include "config_manager.hpp"
#include "redaction.hpp"
#include "sql_template_processor.hpp"

#include <algorithm>

namespace flapi {

namespace {
// Below this length a value cannot be replaced without risking unrelated text.
constexpr std::size_t kMinScrubbableSecret = 4;

// Above this length, a whitelisted environment value is treated as a secret
// whatever it is called. Chosen so ordinary configuration - a region, a bucket
// name, a hostname, a tuning knob - stays visible, while API keys, tokens and
// service-account blobs do not.
constexpr std::size_t kOpaqueEnvValue = 24;
}  // namespace

void TemplateSecrets::add(const std::string& key, const std::string& value) {
    if (value.empty() || !isCredentialKey(key)) {
        return;
    }
    if (value.size() < kMinScrubbableSecret) {
        // Measured: a one-byte secret "a" rewrote the middle of an unrelated
        // file path. Withhold rather than leak or mangle.
        withhold_ = true;
        return;
    }
    if (std::find(values_.begin(), values_.end(), value) == values_.end()) {
        values_.push_back(value);
    }
}

void TemplateSecrets::addCallerSupplied(const std::string& key, const std::string& value) {
    if (value.empty() || !isCredentialKey(key)) {
        return;
    }
    // Long enough to replace safely: scrub it. Too short: leave it, rather
    // than suppressing the caller's own diagnostics over a value they chose.
    if (value.size() >= kMinScrubbableSecret &&
        std::find(values_.begin(), values_.end(), value) == values_.end()) {
        values_.push_back(value);
    }
}

void TemplateSecrets::addEnv(const std::string& key, const std::string& value) {
    add(key, value);
    if (value.size() >= kOpaqueEnvValue &&
        std::find(values_.begin(), values_.end(), value) == values_.end()) {
        values_.push_back(value);
    }
}

namespace {

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

}  // namespace

void TemplateSecrets::addConnectionProperty(const std::string& key,
                                            const std::string& value) {
    add(key, value);
    if (value.size() >= kOpaqueEnvValue && !looksLikeCredentialFreeLocation(value) &&
        std::find(values_.begin(), values_.end(), value) == values_.end()) {
        values_.push_back(value);
    }
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
