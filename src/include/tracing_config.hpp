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

struct TracingSampleConfig {
    std::string type = "parentbased_traceidratio";   // always_on | always_off | parentbased_traceidratio
    double ratio = 1.0;
};

struct TracingFlushConfig {
    std::string mode = "batch";          // batch | on_response
    int timeout_ms = 2000;
    int max_queue_size = 2048;
};

struct TracingConfig {
    bool enabled = false;                // BR-6: off by default, always
    std::string service_name = "flapi";
    std::string service_namespace;
    std::string exporter = "otlp_http";  // otlp_http | otlp_file | none
    std::optional<std::string> endpoint;
    std::optional<std::string> protocol;
    std::map<std::string, std::string> headers;
    int timeout_ms = 10000;

    CaptureTier capture = CaptureTier::Metadata;
    bool openinference = false;
    bool metrics = true;
    bool client_spans = true;

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
    std::size_t payload_max_documents = 50;
};

CaptureTier parseCaptureTier(const std::string& value, CaptureTier fallback = CaptureTier::Metadata);
const char* captureTierName(CaptureTier tier);

}  // namespace flapi
