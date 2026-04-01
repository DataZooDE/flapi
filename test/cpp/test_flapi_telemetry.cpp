#include <catch2/catch_test_macros.hpp>

#include "flapi_telemetry.hpp"

#include <cstdlib>
#include <string>

namespace {

// RAII helper to set/restore an env var
struct EnvGuard {
    explicit EnvGuard(const char* name, const char* value) : name_(name) {
        const char* existing = std::getenv(name);
        prev_ = existing ? existing : "";
        has_prev_ = (existing != nullptr);
        setenv(name, value, 1);
    }
    ~EnvGuard() {
        if (has_prev_) {
            setenv(name_, prev_.c_str(), 1);
        } else {
            unsetenv(name_);
        }
    }
    const char* name_;
    std::string prev_;
    bool has_prev_;
};

struct MockBackend : public flapi::ITelemetryBackend {
    int start_calls = 0;
    int stop_calls = 0;
    std::string last_start_app;
    std::string last_start_ver;
    std::string last_stop_app;
    std::string last_stop_ver;

    void captureStart(const std::string& app_name,
                      const std::string& app_version) override {
        start_calls++;
        last_start_app = app_name;
        last_start_ver = app_version;
    }

    void captureStop(const std::string& app_name,
                     const std::string& app_version) override {
        stop_calls++;
        last_stop_app = app_name;
        last_stop_ver = app_version;
    }
};

} // anonymous namespace

TEST_CASE("FlapiTelemetry - notifyStart calls backend once with correct args", "[flapi_telemetry]") {
    auto mock = std::make_unique<MockBackend>();
    MockBackend* raw = mock.get();

    flapi::FlapiTelemetry telemetry(std::move(mock));
    telemetry.notifyStart("1.2.3");

    REQUIRE(raw->start_calls == 1);
    REQUIRE(raw->last_start_ver == "1.2.3");
    REQUIRE(raw->last_start_app == "flapi");
}

TEST_CASE("FlapiTelemetry - notifyStop calls backend once with correct args", "[flapi_telemetry]") {
    auto mock = std::make_unique<MockBackend>();
    MockBackend* raw = mock.get();

    flapi::FlapiTelemetry telemetry(std::move(mock));
    telemetry.notifyStop("1.2.3");

    REQUIRE(raw->stop_calls == 1);
    REQUIRE(raw->last_stop_ver == "1.2.3");
    REQUIRE(raw->last_stop_app == "flapi");
}

TEST_CASE("FlapiTelemetry - DATAZOO_DISABLE_TELEMETRY=1 suppresses notifyStart", "[flapi_telemetry]") {
    EnvGuard guard("DATAZOO_DISABLE_TELEMETRY", "1");

    auto mock = std::make_unique<MockBackend>();
    MockBackend* raw = mock.get();

    flapi::FlapiTelemetry telemetry(std::move(mock));
    telemetry.notifyStart("1.2.3");

    REQUIRE(raw->start_calls == 0);
}

TEST_CASE("FlapiTelemetry - DATAZOO_DISABLE_TELEMETRY=1 suppresses notifyStop", "[flapi_telemetry]") {
    EnvGuard guard("DATAZOO_DISABLE_TELEMETRY", "1");

    auto mock = std::make_unique<MockBackend>();
    MockBackend* raw = mock.get();

    flapi::FlapiTelemetry telemetry(std::move(mock));
    telemetry.notifyStop("1.2.3");

    REQUIRE(raw->stop_calls == 0);
}

TEST_CASE("FlapiTelemetry - DATAZOO_DISABLE_TELEMETRY=true suppresses calls", "[flapi_telemetry]") {
    EnvGuard guard("DATAZOO_DISABLE_TELEMETRY", "true");

    auto mock = std::make_unique<MockBackend>();
    MockBackend* raw = mock.get();

    flapi::FlapiTelemetry telemetry(std::move(mock));
    telemetry.notifyStart("0.3.0");
    telemetry.notifyStop("0.3.0");

    REQUIRE(raw->start_calls == 0);
    REQUIRE(raw->stop_calls == 0);
}

TEST_CASE("FlapiTelemetry - env var not set allows calls through", "[flapi_telemetry]") {
    // Ensure the env var is not set
    EnvGuard guard("DATAZOO_DISABLE_TELEMETRY", "0");

    auto mock = std::make_unique<MockBackend>();
    MockBackend* raw = mock.get();

    flapi::FlapiTelemetry telemetry(std::move(mock));
    telemetry.notifyStart("0.3.0");
    telemetry.notifyStop("0.3.0");

    REQUIRE(raw->start_calls == 1);
    REQUIRE(raw->stop_calls == 1);
}

TEST_CASE("FlapiTelemetry - multiple notifyStart calls each forwarded", "[flapi_telemetry]") {
    auto mock = std::make_unique<MockBackend>();
    MockBackend* raw = mock.get();

    flapi::FlapiTelemetry telemetry(std::move(mock));
    telemetry.notifyStart("0.3.0");
    telemetry.notifyStart("0.3.0");

    REQUIRE(raw->start_calls == 2);
}

TEST_CASE("FlapiTelemetry - production constructor compiles and constructs", "[flapi_telemetry]") {
    // Just verify the production path can be instantiated without throwing.
    // We can't easily verify it calls PostHogTelemetry without network,
    // but DATAZOO_DISABLE_TELEMETRY=1 ensures no HTTP is sent.
    EnvGuard guard("DATAZOO_DISABLE_TELEMETRY", "1");

    REQUIRE_NOTHROW(flapi::FlapiTelemetry{});
}
