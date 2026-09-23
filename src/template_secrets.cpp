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

void TemplateSecrets::addEnv(const std::string& key, const std::string& value) {
    add(key, value);
    if (value.size() >= kOpaqueEnvValue &&
        std::find(values_.begin(), values_.end(), value) == values_.end()) {
        values_.push_back(value);
    }
}

std::string TemplateSecrets::scrub(std::string text) const {
    for (const auto& value : values_) {
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
                secrets.addAll(it->second.properties);
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
    secrets.addAll(params);
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
