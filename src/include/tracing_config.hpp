#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace flapi {

// The `tracing:` block. Every field an operator can set is std::optional where
// an environment variable may also supply it, so "not set in YAML" is
// distinguishable from "set to the default" - otherwise a YAML default silently
// beats an operator's injected OTEL_* variable and Kubernetes auto-configuration
// does nothing.
enum class CaptureTier { Off, Metadata, Payload };

// How much DuckDB execution profiling to attach to the database span.
//
// The two levels are NOT just a volume knob. DuckDB's ProfilingInfo::Expand
// turns CPU_TIME into per-operator timing collection internally, so `Detailed`
// makes DuckDB do measurably more work per query; `Summary` deliberately uses
// only query-level metrics that do not expand.
//
// Off by default: profiling settings are connection-scoped and flAPI opens a
// connection per query, so this costs a round trip per query.
enum class DbProfiling { Off, Summary, Detailed };

struct TracingSampleConfig {
    std::string type = "parentbased_traceidratio";   // always_on | always_off | parentbased_traceidratio
    double ratio = 1.0;
};

struct TracingFlushConfig {
    std::string mode = "batch";          // batch | on_response
    int timeout_ms = 2000;
    int max_queue_size = 2048;

    // Opt-in for request-billed, scale-to-zero platforms (Cloud Run, App
    // Runner). With `mode: on_response` and a NETWORK exporter, this is the
    // hard cap on how long a request may block while its spans are exported.
    //
    // Deliberately no default. There is no safe universal answer to "how much
    // latency will you trade for telemetry", and a defaulted one would be
    // adopted by accident. Absent it, on_response keeps falling back to batch
    // for otlp_http, which is the safe behaviour for a long-lived deployment.
    std::optional<int> blocking_timeout_ms;
};

struct TracingConfig {
    bool enabled = false;                // BR-6: off by default, always
    DbProfiling db_profiling = DbProfiling::Off;
    std::string service_name = "flapi";
    std::string service_namespace;
    std::string exporter = "otlp_http";  // otlp_http | otlp_file | none
    std::optional<std::string> endpoint;
    std::optional<std::string> protocol;
    std::map<std::string, std::string> headers;
    int timeout_ms = 10000;

    CaptureTier capture = CaptureTier::Metadata;
    bool openinference = false;

    // Probes are the highest-volume route in a Kubernetes deployment and are of
    // near-zero diagnostic value once green. They still count toward the duration
    // metric; they just do not each mint a span.
    std::vector<std::string> exclude_routes{
        "/health", "/health/live", "/mcp/health", "/doc", "/doc.yaml"};

    TracingSampleConfig sample;
    TracingFlushConfig flush;
    std::string file_path;               // exporter: otlp_file ("-" means stdout)
    std::map<std::string, std::string> resource_attributes;

    std::size_t payload_max_value_bytes = 8192;
};

DbProfiling parseDbProfiling(const std::string& value, DbProfiling fallback = DbProfiling::Off);
const char* dbProfilingName(DbProfiling level);

CaptureTier parseCaptureTier(const std::string& value, CaptureTier fallback = CaptureTier::Metadata);
const char* captureTierName(CaptureTier tier);

}  // namespace flapi
