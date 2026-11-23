#include "flapi_telemetry.hpp"

#include "telemetry.hpp"

#include <openssl/sha.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace flapi {

namespace {

// SHA-256 hex digest. Used to hash a license id into the (non-PII) account
// group key. OpenSSL is already linked into flapi-lib.
std::string Sha256Hex(const std::string& input) {
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    ::SHA256(reinterpret_cast<const unsigned char*>(input.data()), input.size(),
             digest.data());
    static const char* const kHex = "0123456789abcdef";
    std::string out;
    out.reserve(digest.size() * 2);
    for (unsigned char byte : digest) {
        out.push_back(kHex[byte >> 4]);
        out.push_back(kHex[byte & 0x0f]);
    }
    return out;
}

bool EnvDisablesTelemetry() {
    const char* val = std::getenv("DATAZOO_DISABLE_TELEMETRY");
    if (val == nullptr) {
        return false;
    }
    std::string s(val);
    return (s == "1" || s == "true" || s == "yes");
}

} // namespace

// ── PostHogBackend ────────────────────────────────────────────────────────────

void PostHogBackend::setProduct(const std::string& name, const std::string& version,
                                const std::string& edition) {
    duckdb::PostHogTelemetry::Instance().SetProduct(name, version, edition);
}

void PostHogBackend::associateGroup(const std::string& type, const std::string& key) {
    duckdb::PostHogTelemetry::Instance().AssociateGroup(type, key);
}

void PostHogBackend::capture(const std::string& event, duckdb::PropertyMap props) {
    duckdb::PostHogTelemetry::Instance().Capture(event, std::move(props));
}

void PostHogBackend::captureFeature(const std::string& feature, duckdb::PropertyMap props) {
    duckdb::PostHogTelemetry::Instance().CaptureFeature(feature, std::move(props));
}

void PostHogBackend::captureError(const std::string& error_class, duckdb::PropertyMap props) {
    duckdb::PostHogTelemetry::Instance().CaptureError(error_class, std::move(props));
}

void PostHogBackend::flush() {
    duckdb::PostHogTelemetry::Instance().Flush();
}

// ── FlapiTelemetry ────────────────────────────────────────────────────────────

FlapiTelemetry::FlapiTelemetry()
    : backend_(std::make_unique<PostHogBackend>()) {}

FlapiTelemetry::FlapiTelemetry(std::unique_ptr<ITelemetryBackend> backend)
    : backend_(std::move(backend)) {}

void FlapiTelemetry::setEnabled(bool enabled) {
    enabled_ = enabled;
}

bool FlapiTelemetry::isEnabled() const {
    return active();
}

bool FlapiTelemetry::active() const {
    return enabled_ && !EnvDisablesTelemetry();
}

void FlapiTelemetry::setSampling(double rate) {
    if (!(rate > 0.0) || rate >= 1.0 || std::isnan(rate)) {
        // Out of range / disabled sampling: emit everything.
        sample_rate_ = 1.0;
        sample_stride_ = 1;
        return;
    }
    sample_rate_ = rate;
    // 1-of-N decimation; round to the nearest whole stride.
    sample_stride_ = static_cast<uint64_t>(std::llround(1.0 / rate));
    if (sample_stride_ < 1) {
        sample_stride_ = 1;
    }
}

bool FlapiTelemetry::sampleHot() {
    if (sample_stride_ <= 1) {
        return true;
    }
    // Emit exactly one of every `sample_stride_` calls. Deterministic so the
    // decimation is testable and needs no shared RNG on the hot path.
    return (sample_counter_++ % sample_stride_) == 0;
}

void FlapiTelemetry::stampCommon(duckdb::PropertyMap& props, bool hot) const {
    props["install_kind"] = INSTALL_KIND;
    if (hot && sample_stride_ > 1) {
        props["sample_rate"] = sample_rate_;   // JSON number, scales counts back up
    }
}

std::string FlapiTelemetry::statusClass(int code) {
    if (code >= 100 && code < 600) {
        switch (code / 100) {
            case 1: return "1xx";
            case 2: return "2xx";
            case 3: return "3xx";
            case 4: return "4xx";
            case 5: return "5xx";
            default: break;
        }
    }
    return "unknown";
}

void FlapiTelemetry::configureProduct(const std::string& version,
                                      const std::string& edition) {
    if (!active()) {
        return;
    }
    backend_->setProduct(PRODUCT, version, edition);
}

void FlapiTelemetry::associateDeployment() {
    if (!active()) {
        return;
    }
    // The deployment group key is the pseudonymous per-machine distinct_id.
    backend_->associateGroup("deployment", duckdb::PostHogTelemetry::GetDistinctId());
}

void FlapiTelemetry::associateAccount(const std::string& license_id) {
    if (!active() || license_id.empty()) {
        return;
    }
    backend_->associateGroup("account", Sha256Hex(license_id));
}

void FlapiTelemetry::serverStarted(int endpoint_count, const std::string& auth_kind) {
    if (!active()) {
        return;
    }
    duckdb::PropertyMap props;
    props["endpoint_count"] = endpoint_count;   // JSON number
    props["auth_kind"] = auth_kind;             // enum: none|basic|bearer|oidc
    stampCommon(props, /*hot=*/false);
    backend_->capture("server_started", std::move(props));
}

void FlapiTelemetry::restEndpointServed(const std::string& method,
                                        const std::string& route_template,
                                        int status_code, double duration_ms,
                                        bool cache_hit) {
    if (!active() || !sampleHot()) {
        return;
    }
    duckdb::PropertyMap props;
    const std::string status = statusClass(status_code);
    props["method"] = method;                       // enum: GET|POST|...
    props["route_template"] = route_template;       // TEMPLATE, never filled path
    props["status_class"] = status;
    props["cache_hit"] = cache_hit;                 // JSON bool
    stampCommon(props, /*hot=*/true);
    aggregate("rest|" + method + "|" + route_template + "|" + status + "|" + (cache_hit ? "1" : "0"),
              "rest_endpoint_served", std::move(props), duration_ms);
}

void FlapiTelemetry::mcpToolCalled(const std::string& tool, bool success,
                                   double duration_ms) {
    if (!active() || !sampleHot()) {
        return;
    }
    duckdb::PropertyMap props;
    props["tool"] = tool;                           // registered, bounded tool name
    props["status_class"] = success ? "2xx" : "5xx";
    stampCommon(props, /*hot=*/true);
    aggregate(std::string("mcp|") + tool + "|" + (success ? "1" : "0"), "mcp_tool_called", std::move(props),
              duration_ms);
}

void FlapiTelemetry::authEnforced(const std::string& auth_kind, bool allow) {
    if (!active()) {
        return;
    }
    duckdb::PropertyMap props;
    props["auth_kind"] = auth_kind;                 // enum: basic|bearer|oidc
    props["outcome"] = allow ? "allow" : "deny";
    stampCommon(props, /*hot=*/false);
    // Runs once per authenticated request: aggregated like the request events.
    aggregate(std::string("auth|") + auth_kind + "|" + (allow ? "1" : "0"), "auth_enforced", std::move(props),
              std::nullopt);
}

void FlapiTelemetry::error(const std::string& error_class, const std::string& feature,
                           const std::string& route_template) {
    if (!active()) {
        return;
    }
    duckdb::PropertyMap props;
    props["feature"] = feature;                     // enum: which capability
    props["route_template"] = route_template;       // TEMPLATE, never filled path
    stampCommon(props, /*hot=*/false);
    // error_class must be an enumerated class — never a message/SQL/user data.
    backend_->captureError(error_class, std::move(props));
}

void FlapiTelemetry::setAggregationInterval(std::chrono::milliseconds interval) {
    std::lock_guard<std::mutex> lock(aggregate_mutex_);
    aggregation_interval_ = interval;
}

void FlapiTelemetry::aggregate(const std::string& key, const std::string& event, duckdb::PropertyMap props,
                               std::optional<double> duration_ms) {
    bool due = false;
    {
        std::lock_guard<std::mutex> lock(aggregate_mutex_);
        auto [it, inserted] = aggregates_.try_emplace(key);
        Aggregate& agg = it->second;
        if (inserted) {
            agg.event = event;
            agg.props = std::move(props);
        }
        ++agg.count;
        if (duration_ms) {
            agg.has_duration = true;
            agg.duration_sum += *duration_ms;
            agg.duration_max = std::max(agg.duration_max, *duration_ms);
        }
        due = std::chrono::steady_clock::now() - last_emit_ >= aggregation_interval_;
    }
    if (due) {
        emitAggregates();
    }
}

bool FlapiTelemetry::hasPendingAggregates() {
    std::lock_guard<std::mutex> lock(aggregate_mutex_);
    return !aggregates_.empty();
}

void FlapiTelemetry::emitAggregates() {
    std::unordered_map<std::string, Aggregate> pending;
    {
        std::lock_guard<std::mutex> lock(aggregate_mutex_);
        pending.swap(aggregates_);
        last_emit_ = std::chrono::steady_clock::now();
    }
    for (auto& [key, agg] : pending) {
        duckdb::PropertyMap props = std::move(agg.props);
        props["count"] = static_cast<int64_t>(agg.count);              // requests this event stands for
        if (agg.has_duration) {
            props["duration_ms"] = agg.duration_sum / static_cast<double>(agg.count);   // mean
            props["duration_ms_max"] = agg.duration_max;
        }
        backend_->captureFeature(agg.event, std::move(props));
    }
}

void FlapiTelemetry::flush() {
    if (!active()) {
        return;
    }
    emitAggregates();
    backend_->flush();
}

// ── Process-wide instance ─────────────────────────────────────────────────────

FlapiTelemetry& GlobalTelemetry() {
    // Intentionally leaked (never destroyed): matches the underlying library
    // singleton lifetime and keeps late captures during teardown safe.
    static FlapiTelemetry* instance = new FlapiTelemetry();
    return *instance;
}

} // namespace flapi
