#include "flapi_telemetry.hpp"

#include "telemetry.hpp"

#include <cstdlib>
#include <string>

namespace flapi {

// ── PostHogBackend ────────────────────────────────────────────────────────────

void PostHogBackend::captureStart(const std::string& app_name,
                                   const std::string& app_version)
{
    duckdb::PostHogTelemetry::Instance().CaptureApplicationStart(app_name, app_version);
}

void PostHogBackend::captureStop(const std::string& app_name,
                                  const std::string& app_version)
{
    duckdb::PostHogTelemetry::Instance().CaptureApplicationStop(app_name, app_version);
}

// ── FlapiTelemetry ────────────────────────────────────────────────────────────

FlapiTelemetry::FlapiTelemetry()
    : backend_(std::make_unique<PostHogBackend>()), enabled_(true)
{}

FlapiTelemetry::FlapiTelemetry(std::unique_ptr<ITelemetryBackend> backend)
    : backend_(std::move(backend)), enabled_(true)
{}

void FlapiTelemetry::setEnabled(bool enabled)
{
    enabled_ = enabled;
}

bool FlapiTelemetry::isTelemetryDisabled(bool enabled)
{
    if (!enabled) {
        return true;
    }
    const char* val = std::getenv("DATAZOO_DISABLE_TELEMETRY");
    if (!val) {
        return false;
    }
    std::string s(val);
    return (s == "1" || s == "true" || s == "yes");
}

void FlapiTelemetry::notifyStart(const std::string& version)
{
    if (isTelemetryDisabled(enabled_)) {
        return;
    }
    backend_->captureStart(APP_NAME, version);
}

void FlapiTelemetry::notifyStop(const std::string& version)
{
    if (isTelemetryDisabled(enabled_)) {
        return;
    }
    backend_->captureStop(APP_NAME, version);
}

} // namespace flapi
