#include <catch2/catch_test_macros.hpp>

#include "flapi_telemetry.hpp"
#include "telemetry.hpp"

#include <cstdlib>
#include <string>
#include <vector>

namespace {

// RAII helper to set/restore an env var.
struct EnvGuard {
    EnvGuard(const char* name, const char* value) : name_(name) {
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

// Records every call so tests can assert on event names and property maps.
struct FakeBackend : public flapi::ITelemetryBackend {
    struct Call {
        std::string kind;   // "capture" | "feature" | "error"
        std::string name;   // event / feature / error_class
        duckdb::PropertyMap props;
    };
    std::vector<Call> calls;
    std::vector<std::pair<std::string, std::string>> groups;  // (type, key)
    std::string product_name, product_version, product_edition;
    int flushes = 0;

    void setProduct(const std::string& name, const std::string& version,
                    const std::string& edition) override {
        product_name = name;
        product_version = version;
        product_edition = edition;
    }
    void associateGroup(const std::string& type, const std::string& key) override {
        groups.emplace_back(type, key);
    }
    void capture(const std::string& event, duckdb::PropertyMap props) override {
        calls.push_back({"capture", event, std::move(props)});
    }
    void captureFeature(const std::string& feature, duckdb::PropertyMap props) override {
        calls.push_back({"feature", feature, std::move(props)});
    }
    void captureError(const std::string& error_class, duckdb::PropertyMap props) override {
        calls.push_back({"error", error_class, std::move(props)});
    }
    void flush() override { flushes++; }

    const Call* find(const std::string& name) const {
        for (const auto& c : calls) {
            if (c.name == name) {
                return &c;
            }
        }
        return nullptr;
    }
};

// Build a FlapiTelemetry over a FakeBackend and hand back the raw pointer.
flapi::FlapiTelemetry makeTelemetry(FakeBackend*& raw) {
    auto fake = std::make_unique<FakeBackend>();
    raw = fake.get();
    return flapi::FlapiTelemetry(std::move(fake));
}

bool hasStringProp(const duckdb::PropertyMap& p, const std::string& key,
                   const std::string& value) {
    auto it = p.find(key);
    return it != p.end() &&
           it->second.kind == duckdb::PropertyValue::Kind::String &&
           it->second.s == value;
}

} // namespace

TEST_CASE("statusClass buckets HTTP codes", "[flapi_telemetry]") {
    REQUIRE(flapi::FlapiTelemetry::statusClass(200) == "2xx");
    REQUIRE(flapi::FlapiTelemetry::statusClass(204) == "2xx");
    REQUIRE(flapi::FlapiTelemetry::statusClass(301) == "3xx");
    REQUIRE(flapi::FlapiTelemetry::statusClass(404) == "4xx");
    REQUIRE(flapi::FlapiTelemetry::statusClass(500) == "5xx");
    REQUIRE(flapi::FlapiTelemetry::statusClass(0) == "unknown");
}

TEST_CASE("server_started carries only bounded counts/kinds + install_kind", "[flapi_telemetry]") {
    EnvGuard guard("DATAZOO_DISABLE_TELEMETRY", "0");
    FakeBackend* raw = nullptr;
    auto tel = makeTelemetry(raw);

    tel.configureProduct("1.2.3", "oss");
    tel.serverStarted(7, "bearer");

    REQUIRE(raw->product_name == "flapi");
    REQUIRE(raw->product_version == "1.2.3");
    REQUIRE(raw->product_edition == "oss");

    const auto* ev = raw->find("server_started");
    REQUIRE(ev != nullptr);
    REQUIRE(ev->kind == "capture");
    // endpoint_count is a JSON number, not a quoted string.
    REQUIRE(ev->props.at("endpoint_count").kind == duckdb::PropertyValue::Kind::Int);
    REQUIRE(ev->props.at("endpoint_count").i == 7);
    REQUIRE(hasStringProp(ev->props, "auth_kind", "bearer"));
    REQUIRE(hasStringProp(ev->props, "install_kind", "server"));
}

TEST_CASE("associateDeployment associates the deployment group", "[flapi_telemetry]") {
    EnvGuard guard("DATAZOO_DISABLE_TELEMETRY", "0");
    FakeBackend* raw = nullptr;
    auto tel = makeTelemetry(raw);

    tel.associateDeployment();
    REQUIRE(raw->groups.size() == 1);
    REQUIRE(raw->groups[0].first == "deployment");
    // Key is the pseudonymous machine id; may be empty in some CI sandboxes but
    // the group type must be exactly "deployment".
}

TEST_CASE("associateAccount hashes the license id (never raw)", "[flapi_telemetry]") {
    EnvGuard guard("DATAZOO_DISABLE_TELEMETRY", "0");
    FakeBackend* raw = nullptr;
    auto tel = makeTelemetry(raw);

    tel.associateAccount("ACME-LICENSE-123");
    REQUIRE(raw->groups.size() == 1);
    REQUIRE(raw->groups[0].first == "account");
    // The raw license id must never appear — only its sha256 hex digest.
    REQUIRE(raw->groups[0].second != "ACME-LICENSE-123");
    REQUIRE(raw->groups[0].second.size() == 64);
}

TEST_CASE("rest_endpoint_served emits template/enum/number props only", "[flapi_telemetry]") {
    EnvGuard guard("DATAZOO_DISABLE_TELEMETRY", "0");
    FakeBackend* raw = nullptr;
    auto tel = makeTelemetry(raw);

    tel.restEndpointServed("GET", "/customers/:id", 200, 12.5, true);

    const auto* ev = raw->find("rest_endpoint_served");
    REQUIRE(ev != nullptr);
    REQUIRE(ev->kind == "feature");
    REQUIRE(hasStringProp(ev->props, "method", "GET"));
    // The ROUTE TEMPLATE is passed through verbatim (never a filled path).
    REQUIRE(hasStringProp(ev->props, "route_template", "/customers/:id"));
    REQUIRE(hasStringProp(ev->props, "status_class", "2xx"));
    REQUIRE(ev->props.at("duration_ms").kind == duckdb::PropertyValue::Kind::Double);
    REQUIRE(ev->props.at("cache_hit").kind == duckdb::PropertyValue::Kind::Bool);
    REQUIRE(ev->props.at("cache_hit").b == true);
    REQUIRE(hasStringProp(ev->props, "install_kind", "server"));

    // Guard against leaks: only these keys may ever be present.
    for (const auto& kv : ev->props) {
        const std::string& k = kv.first;
        REQUIRE((k == "method" || k == "route_template" || k == "status_class" ||
                 k == "duration_ms" || k == "cache_hit" || k == "install_kind"));
    }
}

TEST_CASE("mcp_tool_called emits bounded tool name + status/duration", "[flapi_telemetry]") {
    EnvGuard guard("DATAZOO_DISABLE_TELEMETRY", "0");
    FakeBackend* raw = nullptr;
    auto tel = makeTelemetry(raw);

    tel.mcpToolCalled("get_customer", false, 3.0);
    const auto* ev = raw->find("mcp_tool_called");
    REQUIRE(ev != nullptr);
    REQUIRE(hasStringProp(ev->props, "tool", "get_customer"));
    REQUIRE(hasStringProp(ev->props, "status_class", "5xx"));
    REQUIRE(ev->props.at("duration_ms").kind == duckdb::PropertyValue::Kind::Double);
}

TEST_CASE("auth_enforced emits kind + outcome only", "[flapi_telemetry]") {
    EnvGuard guard("DATAZOO_DISABLE_TELEMETRY", "0");
    FakeBackend* raw = nullptr;
    auto tel = makeTelemetry(raw);

    tel.authEnforced("oidc", false);
    const auto* ev = raw->find("auth_enforced");
    REQUIRE(ev != nullptr);
    REQUIRE(hasStringProp(ev->props, "auth_kind", "oidc"));
    REQUIRE(hasStringProp(ev->props, "outcome", "deny"));
}

TEST_CASE("error emits enumerated class + template only", "[flapi_telemetry]") {
    EnvGuard guard("DATAZOO_DISABLE_TELEMETRY", "0");
    FakeBackend* raw = nullptr;
    auto tel = makeTelemetry(raw);

    tel.error("server_error", "rest_endpoint_served", "/orders/:id");
    const auto* ev = raw->find("server_error");
    REQUIRE(ev != nullptr);
    REQUIRE(ev->kind == "error");
    REQUIRE(hasStringProp(ev->props, "feature", "rest_endpoint_served"));
    REQUIRE(hasStringProp(ev->props, "route_template", "/orders/:id"));
}

TEST_CASE("DATAZOO_DISABLE_TELEMETRY short-circuits every emit", "[flapi_telemetry]") {
    EnvGuard guard("DATAZOO_DISABLE_TELEMETRY", "1");
    FakeBackend* raw = nullptr;
    auto tel = makeTelemetry(raw);

    tel.configureProduct("1.0.0", "oss");
    tel.associateDeployment();
    tel.serverStarted(3, "none");
    tel.restEndpointServed("GET", "/x", 200, 1.0, false);
    tel.mcpToolCalled("t", true, 1.0);
    tel.authEnforced("basic", true);
    tel.error("bad_request", "rest_endpoint_served", "/x");
    tel.flush();

    REQUIRE(raw->calls.empty());
    REQUIRE(raw->groups.empty());
    REQUIRE(raw->product_name.empty());
    REQUIRE(raw->flushes == 0);
}

TEST_CASE("setEnabled(false) short-circuits every emit", "[flapi_telemetry]") {
    EnvGuard guard("DATAZOO_DISABLE_TELEMETRY", "0");
    FakeBackend* raw = nullptr;
    auto tel = makeTelemetry(raw);
    tel.setEnabled(false);

    tel.serverStarted(3, "none");
    tel.restEndpointServed("GET", "/x", 200, 1.0, false);
    REQUIRE(raw->calls.empty());
    REQUIRE(tel.isEnabled() == false);
}

TEST_CASE("sampling decimates the hot path and stamps sample_rate", "[flapi_telemetry]") {
    EnvGuard guard("DATAZOO_DISABLE_TELEMETRY", "0");
    FakeBackend* raw = nullptr;
    auto tel = makeTelemetry(raw);
    tel.setSampling(0.5);   // emit 1 of every 2

    for (int i = 0; i < 10; ++i) {
        tel.restEndpointServed("GET", "/x", 200, 1.0, false);
    }
    REQUIRE(raw->calls.size() == 5);
    // Surviving events are stamped with sample_rate so counts scale back up.
    REQUIRE(raw->calls.front().props.at("sample_rate").kind == duckdb::PropertyValue::Kind::Double);

    // Low-volume events are never sampled out.
    tel.authEnforced("basic", true);
    REQUIRE(raw->find("auth_enforced") != nullptr);
}

// End-to-end no-leak check against the REAL library transport: drive the
// production PostHogBackend through a test transport that captures the exact
// serialized batch, and assert the outgoing JSON contains only bounded props —
// no filled path, SQL, body, or header ever appears.
TEST_CASE("real transport payload contains no URL/SQL/body leaks", "[flapi_telemetry]") {
    EnvGuard guard("DATAZOO_DISABLE_TELEMETRY", "0");

    auto& lib = duckdb::PostHogTelemetry::Instance();
    lib.ResetShutdownForTesting();
    lib.SetEnabled(true);
    lib.SetAutoFlushEnabledForTesting(false);

    std::vector<std::string> payloads;
    lib.SetTransportForTesting(
        [&](const std::string&, const std::string&,
            const std::vector<duckdb::PostHogEvent>& evs) {
            for (const auto& e : evs) {
                payloads.push_back(e.GetPropertiesJson());
            }
        });

    {
        flapi::FlapiTelemetry tel;   // real PostHogBackend
        tel.setEnabled(true);
        tel.configureProduct("9.9.9", "oss");
        tel.restEndpointServed("GET", "/customers/:id", 200, 4.2, true);
        tel.mcpToolCalled("get_customer", true, 1.0);
        tel.flush();
    }

    lib.SetTransportForTesting({});   // restore real transport

    REQUIRE_FALSE(payloads.empty());
    std::string all;
    for (const auto& p : payloads) {
        all += p;
    }

    // The bounded, expected content is present.
    REQUIRE(all.find("route_template") != std::string::npos);
    REQUIRE(all.find("/customers/:id") != std::string::npos);
    REQUIRE(all.find("\"install_kind\"") != std::string::npos);
    REQUIRE(all.find("server") != std::string::npos);

    // Simulated sensitive material that the API never accepts must be absent.
    REQUIRE(all.find("/customers/12345") == std::string::npos);   // filled path
    REQUIRE(all.find("SELECT") == std::string::npos);             // SQL
    REQUIRE(all.find("Authorization") == std::string::npos);      // header
    REQUIRE(all.find("password") == std::string::npos);           // body/secret
}
