#pragma once

#include <memory>
#include <string>

namespace flapi {

// Pure interface – inject a mock in unit tests, PostHogBackend in production
struct ITelemetryBackend {
    virtual ~ITelemetryBackend() = default;
    virtual void captureStart(const std::string& app_name,
                              const std::string& app_version) = 0;
    virtual void captureStop(const std::string& app_name,
                             const std::string& app_version) = 0;
};

// Production backend: delegates to duckdb::PostHogTelemetry singleton
class PostHogBackend : public ITelemetryBackend {
public:
    void captureStart(const std::string& app_name,
                      const std::string& app_version) override;
    void captureStop(const std::string& app_name,
                     const std::string& app_version) override;
};

// flapi-level telemetry facade.
// Checks DATAZOO_DISABLE_TELEMETRY before delegating to the backend.
class FlapiTelemetry {
public:
    // Production: creates a PostHogBackend
    FlapiTelemetry();

    // Test injection: takes ownership of provided backend
    explicit FlapiTelemetry(std::unique_ptr<ITelemetryBackend> backend);

    // Emit application_start event
    void notifyStart(const std::string& version);

    // Emit application_stop event
    void notifyStop(const std::string& version);

private:
    static bool isTelemetryDisabled();

    std::unique_ptr<ITelemetryBackend> backend_;
    static constexpr const char* APP_NAME = "flapi";
};

} // namespace flapi
