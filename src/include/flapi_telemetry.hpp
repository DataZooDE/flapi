#pragma once

#include <memory>
#include <string>

#include "telemetry.hpp"   // duckdb::PostHogTelemetry, PropertyMap, PropertyValue

namespace flapi {

// ─────────────────────────────────────────────────────────────────────────────
// flapi telemetry facade
//
// flapi is a long-running *server*, not a DuckDB extension, so it emits against
// the shared DataZoo telemetry schema (telemetry_schema: 2) with
// install_kind="server" and one $session_id per uptime (the library already
// mints a per-process session id). See TELEMETRY.md for the exact catalogue of
// events and properties.
//
// Every property flapi attaches is a bounded enum, a template, or a number —
// never a filled URL, SQL text, request/response body, header, or connection
// string. That contract is enforced by construction here (call sites only pass
// enums/templates/numbers) and by the library's 512-byte clamp as a backstop.
// ─────────────────────────────────────────────────────────────────────────────

// Pure interface – inject a fake in unit tests, PostHogBackend in production.
// Takes the library's typed PropertyMap so numeric/boolean props keep their
// JSON type (load-bearing for HogQL sum()/avg() and boolean filters).
struct ITelemetryBackend {
    virtual ~ITelemetryBackend() = default;
    virtual void setProduct(const std::string& name, const std::string& version,
                            const std::string& edition) = 0;
    virtual void associateGroup(const std::string& type, const std::string& key) = 0;
    virtual void capture(const std::string& event, duckdb::PropertyMap props) = 0;
    virtual void captureFeature(const std::string& feature, duckdb::PropertyMap props) = 0;
    virtual void captureError(const std::string& error_class, duckdb::PropertyMap props) = 0;
    virtual void flush() = 0;
};

// Production backend: delegates to the duckdb::PostHogTelemetry singleton.
class PostHogBackend : public ITelemetryBackend {
public:
    void setProduct(const std::string& name, const std::string& version,
                    const std::string& edition) override;
    void associateGroup(const std::string& type, const std::string& key) override;
    void capture(const std::string& event, duckdb::PropertyMap props) override;
    void captureFeature(const std::string& feature, duckdb::PropertyMap props) override;
    void captureError(const std::string& error_class, duckdb::PropertyMap props) override;
    void flush() override;
};

// flapi-level telemetry facade. All emit methods are gated by a single guard
// (`active()`), so a single opt-out — env, YAML, or CLI flag — short-circuits
// everything. Every method is non-blocking: the library enqueues onto a
// background worker and returns immediately, so nothing runs on the request
// thread beyond building a small property map.
class FlapiTelemetry {
public:
    // Production: creates a PostHogBackend.
    FlapiTelemetry();

    // Test injection: takes ownership of the provided backend.
    explicit FlapiTelemetry(std::unique_ptr<ITelemetryBackend> backend);

    // Programmatic opt-out (CLI flag / env var / config file resolve to this).
    void setEnabled(bool enabled);
    bool isEnabled() const;

    // Client-side sampling for the hot per-request/per-tool paths. rate in
    // (0,1]; 1.0 (default) emits every event. Events that survive sampling are
    // stamped with sample_rate so counts scale back up. Low-volume lifecycle
    // events (server_started, auth_enforced, errors) are always emitted.
    void setSampling(double rate);

    // ── Boot ────────────────────────────────────────────────────────────────
    // Set product/version/edition and stamp install_kind on the envelope path.
    void configureProduct(const std::string& version, const std::string& edition);
    // Associate the deployment group (key == pseudonymous per-machine id).
    void associateDeployment();
    // Enterprise only: associate the account group with sha256(license_id).
    void associateAccount(const std::string& license_id);
    // Emit server_started with bounded counts/kinds only.
    void serverStarted(int endpoint_count, const std::string& auth_kind);

    // ── Runtime (hot paths — sampled) ─────────────────────────────────────────
    void restEndpointServed(const std::string& method,
                            const std::string& route_template,
                            int status_code, double duration_ms, bool cache_hit);
    void mcpToolCalled(const std::string& tool, bool success, double duration_ms);

    // ── Runtime (low volume — always emitted) ────────────────────────────────
    void authEnforced(const std::string& auth_kind, bool allow);
    void error(const std::string& error_class, const std::string& feature,
               const std::string& route_template);

    // Synchronously drain buffered events (call on clean shutdown / SIGTERM —
    // the library's at-exit handler *discards* by design).
    void flush();

    // Map an HTTP status code to a bounded status class ("2xx".."5xx").
    static std::string statusClass(int code);

private:
    // enabled_ AND not disabled via DATAZOO_DISABLE_TELEMETRY.
    bool active() const;
    // Decimate a hot-path event; returns true if this call should be emitted.
    bool sampleHot();
    // Stamp install_kind (and, when sampling, sample_rate) onto a prop map.
    void stampCommon(duckdb::PropertyMap& props, bool hot) const;

    std::unique_ptr<ITelemetryBackend> backend_;
    bool enabled_ = true;

    // Deterministic 1-of-N decimation for the hot paths (counter, not RNG, so
    // it is testable and needs no global random state).
    double sample_rate_ = 1.0;
    uint64_t sample_stride_ = 1;
    uint64_t sample_counter_ = 0;

    static constexpr const char* PRODUCT = "flapi";
    static constexpr const char* INSTALL_KIND = "server";
};

// Process-wide telemetry instance used by server components (api_server, MCP
// tool handler, auth middleware) to emit without constructor plumbing. main()
// configures it at boot and flushes it on shutdown. Never destroyed (leaked
// like the underlying library singleton) so late captures during teardown are
// safe.
FlapiTelemetry& GlobalTelemetry();

} // namespace flapi
