// The FLAPI_WITH_TRACING=ON tracing facade: provider lifecycle, sampler,
// exporter selection and the single active() gate.
#include "flapi_tracing.hpp"
#include "trace_scope_internal.hpp"

#if FLAPI_WITH_TRACING

#include <opentelemetry/exporters/otlp/otlp_file_exporter_factory.h>
#include <opentelemetry/exporters/otlp/otlp_file_exporter_options.h>
#include <opentelemetry/exporters/otlp/otlp_http_exporter_factory.h>
#include <opentelemetry/exporters/otlp/otlp_http_exporter_options.h>
#include <opentelemetry/sdk/resource/resource.h>
#include <opentelemetry/sdk/trace/batch_span_processor_factory.h>
#include <opentelemetry/sdk/trace/batch_span_processor_options.h>
#include <opentelemetry/sdk/trace/samplers/always_off_factory.h>
#include <opentelemetry/sdk/trace/samplers/always_on_factory.h>
#include <opentelemetry/sdk/trace/samplers/parent_factory.h>
#include <opentelemetry/sdk/trace/samplers/trace_id_ratio_factory.h>
#include <opentelemetry/sdk/trace/simple_processor_factory.h>
#include <opentelemetry/sdk/trace/tracer_provider_factory.h>
#include <opentelemetry/trace/provider.h>

#include <atomic>
#include <cstdlib>
#include <utility>

namespace flapi {

namespace otel = opentelemetry;
namespace tsdk = opentelemetry::sdk::trace;
namespace otlp = opentelemetry::exporter::otlp;

namespace {

std::atomic<std::uint64_t> g_spans_started{0};

// OTel's own trace ids are opaque; flAPI builds a parent SpanContext from the
// extracted W3C ids so an inbound traceparent actually parents the server span.
otel::trace::SpanContext toSpanContext(const SpanContextIds& ids) {
    if (!ids.valid()) {
        return otel::trace::SpanContext::GetInvalid();
    }
    std::uint8_t trace_bytes[16];
    std::uint8_t span_bytes[8];
    auto hexPair = [](char hi, char lo) -> std::uint8_t {
        auto nib = [](char c) -> std::uint8_t {
            return static_cast<std::uint8_t>(c <= '9' ? c - '0' : c - 'a' + 10);
        };
        return static_cast<std::uint8_t>((nib(hi) << 4) | nib(lo));
    };
    for (int i = 0; i < 16; ++i) {
        trace_bytes[i] = hexPair(ids.trace_id[i * 2], ids.trace_id[i * 2 + 1]);
    }
    for (int i = 0; i < 8; ++i) {
        span_bytes[i] = hexPair(ids.span_id[i * 2], ids.span_id[i * 2 + 1]);
    }
    return otel::trace::SpanContext(
        otel::trace::TraceId(trace_bytes),
        otel::trace::SpanId(span_bytes),
        otel::trace::TraceFlags(ids.flags),
        /*is_remote=*/true);
}

class OtelTracingBackend : public ITracingBackend {
public:
    explicit OtelTracingBackend(const TracingConfig& config) {
        auto processor = makeProcessor(config);
        if (!processor) {
            return;   // exporter: none - the facade stays inert
        }

        otel::sdk::resource::ResourceAttributes attrs{{"service.name", config.service_name}};
        if (!config.service_namespace.empty()) {
            attrs.SetAttribute("service.namespace", config.service_namespace);
        }
        for (const auto& [key, value] : config.resource_attributes) {
            attrs.SetAttribute(key, value);
        }

        provider_ = tsdk::TracerProviderFactory::Create(
            std::move(processor),
            otel::sdk::resource::Resource::Create(attrs),
            makeSampler(config));
        tracer_ = provider_->GetTracer("flapi");
    }

    SpanScope startSpan(const char* name, SpanKind kind, const SpanContextIds* parent) override {
        if (!tracer_) { return {}; }

        otel::trace::StartSpanOptions options;
        options.kind = toOtelKind(kind);
        if (parent != nullptr && parent->valid()) {
            options.parent = toSpanContext(*parent);
        }
        auto span = tracer_->StartSpan(name, {}, options);
        if (!span) { return {}; }

        g_spans_started.fetch_add(1, std::memory_order_relaxed);
        return SpanScope(new SpanScope::Impl(span));
    }

    bool forceFlush(std::chrono::milliseconds timeout) override {
        if (!provider_) { return true; }
        return static_cast<tsdk::TracerProvider*>(provider_.get())->ForceFlush(timeout);
    }

    void shutdown() override {
        if (!provider_) { return; }
        static_cast<tsdk::TracerProvider*>(provider_.get())->Shutdown();
        tracer_ = {};
        provider_.reset();
    }

    std::uint64_t spansDropped() const override { return 0; }
    std::uint64_t spansExported() const override {
        return g_spans_started.load(std::memory_order_relaxed);
    }

private:
    static otel::trace::SpanKind toOtelKind(SpanKind kind) {
        switch (kind) {
            case SpanKind::Server:   return otel::trace::SpanKind::kServer;
            case SpanKind::Client:   return otel::trace::SpanKind::kClient;
            case SpanKind::Producer: return otel::trace::SpanKind::kProducer;
            case SpanKind::Consumer: return otel::trace::SpanKind::kConsumer;
            case SpanKind::Internal: break;
        }
        return otel::trace::SpanKind::kInternal;
    }

    static std::unique_ptr<tsdk::SpanProcessor> makeProcessor(const TracingConfig& config) {
        std::unique_ptr<tsdk::SpanExporter> exporter;

        if (config.exporter == "otlp_file") {
            // Air-gapped deployments: no egress at all. 1.24.0 ships this
            // natively; the pinned baseline's 1.17.0 did not, which is one of the
            // two reasons for the overlay port.
            otlp::OtlpFileExporterOptions options;
            otlp::OtlpFileClientFileSystemOptions fs;
            fs.file_pattern = config.file_path.empty() ? "traces.jsonl" : config.file_path;

            // The exporter's defaults buffer for 30 SECONDS or 256 records. For
            // the air-gapped topology that means losing up to half a minute of
            // spans on a crash - and in on_response mode it would contradict the
            // mode's entire promise, which is that the span is durable before the
            // response returns. Flush per record there; keep a short interval
            // otherwise so a low-traffic deployment does not sit on spans.
            if (config.flush.mode == "on_response") {
                fs.flush_count = 1;
                fs.flush_interval = std::chrono::microseconds(0);
            } else {
                fs.flush_interval =
                    std::chrono::microseconds(config.flush.timeout_ms * 1000LL);
            }

            options.backend_options = fs;
            exporter = otlp::OtlpFileExporterFactory::Create(options);
        } else if (config.exporter == "otlp_http") {
            otlp::OtlpHttpExporterOptions options;
            if (config.endpoint) { options.url = *config.endpoint; }
            if (config.protocol && *config.protocol == "http/json") {
                options.content_type = otlp::HttpRequestContentType::kJson;
            }
            options.timeout = std::chrono::milliseconds(config.timeout_ms);
            for (const auto& [key, value] : config.headers) {
                options.http_headers.insert({key, value});
            }
            exporter = otlp::OtlpHttpExporterFactory::Create(options);
        } else {
            return nullptr;   // "none"
        }

        if (config.flush.mode == "on_response") {
            // Every span is exported as it ends. Correct on a request-billed,
            // scale-to-zero platform where a background thread may never be
            // scheduled after the response - and restricted to the file exporter,
            // because doing this over the network puts a round-trip on the
            // request thread.
            return tsdk::SimpleSpanProcessorFactory::Create(std::move(exporter));
        }

        tsdk::BatchSpanProcessorOptions batch;
        batch.max_queue_size = static_cast<std::size_t>(config.flush.max_queue_size);
        batch.schedule_delay_millis = std::chrono::milliseconds(config.flush.timeout_ms);
        return tsdk::BatchSpanProcessorFactory::Create(std::move(exporter), batch);
    }

    static std::unique_ptr<tsdk::Sampler> makeSampler(const TracingConfig& config) {
        if (config.sample.type == "always_off") {
            return tsdk::AlwaysOffSamplerFactory::Create();
        }
        if (config.sample.type == "always_on") {
            return tsdk::AlwaysOnSamplerFactory::Create();
        }
        return tsdk::ParentBasedSamplerFactory::Create(
            tsdk::TraceIdRatioBasedSamplerFactory::Create(config.sample.ratio));
    }

    std::unique_ptr<otel::trace::TracerProvider> provider_;
    otel::nostd::shared_ptr<otel::trace::Tracer> tracer_;
};

}  // namespace

FlapiTracing::FlapiTracing() = default;
FlapiTracing::FlapiTracing(std::unique_ptr<ITracingBackend> backend)
    : backend_(std::move(backend)), enabled_(backend_ != nullptr) {}
FlapiTracing::~FlapiTracing() { shutdown(); }

void FlapiTracing::configure(const TracingConfig& config) {
    // BR-6 and the deliberate OTEL_* divergence: an injected
    // OTEL_EXPORTER_OTLP_ENDPOINT does NOT by itself start exporting. A
    // platform-wide environment variable is not an operator's consent to ship
    // data off the machine.
    if (!config.enabled) {
        enabled_ = false;
        return;
    }
    // OTEL_SDK_DISABLED is the standard kill switch and must win over config.
    if (const char* disabled = std::getenv("OTEL_SDK_DISABLED");
        disabled != nullptr && std::string(disabled) == "true") {
        enabled_ = false;
        return;
    }
    if (config.capture == CaptureTier::Off) {
        enabled_ = false;
        return;
    }

    backend_ = std::make_unique<OtelTracingBackend>(config);
    enabled_ = true;
}

bool FlapiTracing::active() const { return enabled_ && backend_ != nullptr; }
bool FlapiTracing::isEnabled() const { return active(); }

SpanScope FlapiTracing::startSpan(const char* name, SpanKind kind, const SpanContextIds* parent) {
    if (!active()) { return {}; }
    return backend_->startSpan(name, kind, parent);
}

SpanScope FlapiTracing::startServerSpan(const std::string& name, const ExtractedContext& parent) {
    if (!active()) { return {}; }
    const SpanContextIds* p = parent.ids.valid() ? &parent.ids : nullptr;
    return backend_->startSpan(name.c_str(), SpanKind::Server, p);
}

bool FlapiTracing::forceFlush(std::chrono::milliseconds timeout) {
    return active() ? backend_->forceFlush(timeout) : true;
}

void FlapiTracing::shutdown() {
    if (backend_) { backend_->shutdown(); }
    enabled_ = false;
}

std::uint64_t FlapiTracing::spansDropped() const {
    return backend_ ? backend_->spansDropped() : 0;
}
std::uint64_t FlapiTracing::spansExported() const {
    return backend_ ? backend_->spansExported() : 0;
}

FlapiTracing& Tracing() {
    static FlapiTracing instance;
    return instance;
}

TracingGuard::~TracingGuard() {
    // Deterministic, before static destruction reaches the provider. A
    // BatchSpanProcessor owns a thread; letting it race teardown is an
    // intermittent crash at exit.
    Tracing().forceFlush(std::chrono::milliseconds(2000));
    Tracing().shutdown();
}

}  // namespace flapi

#endif  // FLAPI_WITH_TRACING
