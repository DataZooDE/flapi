// The FLAPI_WITH_TRACING=ON tracing facade: provider lifecycle, sampler,
// exporter selection and the single active() gate.
#include "flapi_tracing.hpp"

#include <crow/logging.h>
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

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <utility>

namespace flapi {

namespace otel = opentelemetry;
namespace tsdk = opentelemetry::sdk::trace;
namespace otlp = opentelemetry::exporter::otlp;

namespace {

std::atomic<std::uint64_t> g_spans_exported{0};
std::atomic<std::uint64_t> g_spans_dropped{0};
std::atomic<std::uint64_t> g_spans_submitted{0};

// Counts export batches by the result the exporter REPORTS.
//
// Read the caveat before trusting spans_dropped with otlp_http.
// OtlpHttpExporter::Export computes the real result, logs a failure, and then
// returns kSuccess unconditionally on both its sync and async branches
// (otlp_http_exporter.cc:193 and :206). So a collector answering 503 to every
// batch is indistinguishable here from one accepting them: verified
// empirically - collector received 0 spans, this counter said 38 exported, 0
// dropped, while the SDK logged "Export 18 trace span(s) error: 1".
//
// No wrapper at this layer can fix that; the result is discarded above us.
//
// spans_submitted closes a DIFFERENT hole - queue-full drops - and it must not
// be oversold. Both spans_exported and spans_dropped derive from the same
// ExportResult that otlp_http hardcodes to success, so submitted minus exported
// reveals queue drops and spans still in flight, NOT HTTP failures. Against a
// refusing collector all three counters look healthy. For otlp_http the flAPI
// log ("[OTLP TRACE HTTP Exporter] ERROR") is the only source of truth.
class CountingSpanExporter final : public tsdk::SpanExporter {
public:
    explicit CountingSpanExporter(std::unique_ptr<tsdk::SpanExporter> inner)
        : inner_(std::move(inner)) {}

    std::unique_ptr<tsdk::Recordable> MakeRecordable() noexcept override {
        return inner_->MakeRecordable();
    }

    opentelemetry::sdk::common::ExportResult Export(
        const opentelemetry::nostd::span<std::unique_ptr<tsdk::Recordable>>& spans) noexcept override {
        const auto count = static_cast<std::uint64_t>(spans.size());
        const auto result = inner_->Export(spans);
        if (result == opentelemetry::sdk::common::ExportResult::kSuccess) {
            g_spans_exported.fetch_add(count, std::memory_order_relaxed);
        } else {
            g_spans_dropped.fetch_add(count, std::memory_order_relaxed);
        }
        return result;
    }

    bool ForceFlush(std::chrono::microseconds timeout) noexcept override {
        return inner_->ForceFlush(timeout);
    }
    bool Shutdown(std::chrono::microseconds timeout) noexcept override {
        return inner_->Shutdown(timeout);
    }

private:
    std::unique_ptr<tsdk::SpanExporter> inner_;
};

// Counts what was HANDED to the processor.
//
// BatchSpanProcessor::OnEnd drops silently when its queue is full - it logs a
// warning and returns void (batch_span_processor.cc:92), so neither the
// exporter nor a wrapper can observe the drop. spans_dropped therefore counts
// only FAILED EXPORT BATCHES, and a flat value has never proved that nothing
// was lost.
//
// Counting submissions closes that hole honestly rather than inventing a drop
// number: submitted - exported - dropped is what is either still queued or
// gone, and in steady state it is the loss. That is a figure an operator can
// act on, and it does not pretend to a precision the SDK does not offer.
class CountingSpanProcessor final : public tsdk::SpanProcessor {
public:
    explicit CountingSpanProcessor(std::unique_ptr<tsdk::SpanProcessor> inner)
        : inner_(std::move(inner)) {}

    std::unique_ptr<tsdk::Recordable> MakeRecordable() noexcept override {
        return inner_->MakeRecordable();
    }
    void OnStart(tsdk::Recordable& span,
                 const opentelemetry::trace::SpanContext& parent) noexcept override {
        inner_->OnStart(span, parent);
    }
    void OnEnd(std::unique_ptr<tsdk::Recordable>&& span) noexcept override {
        g_spans_submitted.fetch_add(1, std::memory_order_relaxed);
        inner_->OnEnd(std::move(span));
    }
    bool ForceFlush(std::chrono::microseconds timeout) noexcept override {
        return inner_->ForceFlush(timeout);
    }
    bool Shutdown(std::chrono::microseconds timeout) noexcept override {
        return inner_->Shutdown(timeout);
    }

private:
    std::unique_ptr<tsdk::SpanProcessor> inner_;
};

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
        if (processor) {
            processor = std::make_unique<CountingSpanProcessor>(std::move(processor));
        }
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

    std::uint64_t spansDropped() const override {
        return g_spans_dropped.load(std::memory_order_relaxed);
    }
    std::uint64_t spansExported() const override {
        return g_spans_exported.load(std::memory_order_relaxed);
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
            // F2: the request budget bounds the CALLER's wait, not the export
            // pipeline. With ENABLE_ASYNC_EXPORT undefined (this build) the
            // single BatchSpanProcessor worker sits inside one synchronous HTTP
            // call for options.timeout - 10s by default. A 300ms request budget
            // would then return ~33 requests, each paying 300ms, while the
            // pipeline stayed wedged and none of their spans left. Clamp it.
            //
            // Written as a comparison rather than std::min because <windows.h>
            // defines min/max as MACROS, so `std::min(` expands to `std::(` and
            // MSVC reports "illegal token on right side of '::'". The usual
            // workaround is (std::min)(...), which is easy to lose in a later
            // edit; a plain comparison cannot regress.
            auto http_timeout = std::chrono::milliseconds(config.timeout_ms);
            if (config.flush.blocking_timeout_ms) {
                const auto budget =
                    std::chrono::milliseconds(*config.flush.blocking_timeout_ms);
                if (budget < http_timeout) {
                    http_timeout = budget;
                }
            }
            options.timeout = http_timeout;
            // http_headers is a MULTIMAP and the constructor already populated it
            // from OTEL_EXPORTER_OTLP_HEADERS. Inserting without erasing first
            // merges rather than overrides: an Authorization set both in the
            // environment and in YAML would be sent twice, with two different
            // credentials. Explicit YAML wins.
            for (const auto& [key, value] : config.headers) {
                options.http_headers.erase(key);
                options.http_headers.insert({key, value});
            }
            exporter = otlp::OtlpHttpExporterFactory::Create(options);
        } else {
            return nullptr;   // "none"
        }

        // Wrap for counting before any processor sees it.
        exporter = std::make_unique<CountingSpanExporter>(std::move(exporter));

        if (config.flush.mode == "on_response") {
            // Every span is exported as it ends. Correct on a request-billed,
            // scale-to-zero platform where a background thread may never be
            // scheduled after the response.
            //
            // ENFORCED, not merely documented: over the network this would put a
            // synchronous HTTP round-trip on the Crow worker, so a hanging
            // collector would block each worker for timeout_ms and deplete the
            // pool - an availability failure caused by a third party flAPI does
            // not control. The earlier version only said file-only in a comment.
            if (config.exporter == "otlp_file") {
                return tsdk::SimpleSpanProcessorFactory::Create(std::move(exporter));
            }
            if (config.flush.blocking_timeout_ms) {
                // Opted in, with a stated latency budget. Note this still falls
                // through to a BatchSpanProcessor rather than a Simple one: the
                // middleware force-flushes it once per request, so a request's
                // ~4 spans leave in ONE round trip. SimpleSpanProcessor would
                // export per span, which over the network is four.
                CROW_LOG_WARNING
                    << "tracing.flush.mode=on_response with a network exporter: every "
                       "request will block up to " << *config.flush.blocking_timeout_ms
                    << "ms exporting its spans. Intended for request-billed "
                       "scale-to-zero platforms; on a long-lived deployment this "
                       "couples your latency to the collector's availability.";
            } else {
                CROW_LOG_WARNING
                    << "tracing.flush.mode=on_response is only supported with "
                       "exporter=otlp_file, or with an explicit "
                       "tracing.flush.blocking_timeout_ms; falling back to batch "
                       "export. Exporting synchronously over the network would put a "
                       "collector round-trip on every request thread.";
            }
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
    if (config.capture == CaptureTier::Payload) {
        // Payload capture exports customer data to a third destination. An
        // operator must not be able to reach that without seeing it said out
        // loud, and a reviewer reading the startup log must be able to see it too.
        CROW_LOG_WARNING
            << "tracing.capture=payload: declared parameter VALUES will be "
               "exported to the configured tracing backend. Credentials and "
               "filled paths are still excluded. Confirm this is intended.";
    }

    backend_ = std::make_unique<OtelTracingBackend>(config);
    enabled_ = true;
    // Only now, so that a config asking for profiling while tracing is disabled
    // (or OTEL_SDK_DISABLED is set) never makes the query path pay for it.
    db_profiling_ = config.db_profiling;
    shutdown_flush_ = std::chrono::milliseconds(config.flush.timeout_ms);
    // Only a NETWORK exporter needs the middleware to flush per request; the
    // file exporter already uses a SimpleSpanProcessor and has nothing buffered.
    if (config.flush.mode == "on_response" && config.exporter != "otlp_file"
        && config.flush.blocking_timeout_ms) {
        blocking_flush_ = std::chrono::milliseconds(*config.flush.blocking_timeout_ms);
    }
    if (db_profiling_ != DbProfiling::Off) {
        CROW_LOG_INFO << "tracing.db_profiling=" << dbProfilingName(db_profiling_)
                      << ": DuckDB execution profiling is enabled per connection, "
                         "which costs one extra round trip per query.";
    }
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

namespace {
// Process-wide: the middleware has no per-instance state to hang this on, and a
// counter that resets per request would be useless.
std::atomic<std::uint64_t> g_flush_timeouts{0};
}  // namespace

void FlapiTracing::noteFlushTimeout() {
    g_flush_timeouts.fetch_add(1, std::memory_order_relaxed);
}

std::uint64_t FlapiTracing::spansSubmitted() const {
    return g_spans_submitted.load(std::memory_order_relaxed);
}

std::uint64_t FlapiTracing::flushTimeouts() const {
    return g_flush_timeouts.load(std::memory_order_relaxed);
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
    // The configured budget here too, not a hardcoded 2s: this drain covers
    // spans produced after the handler ran, and leaving it fixed would deliver
    // the configurable shutdown flush only half the time.
    Tracing().forceFlush(Tracing().shutdownFlushBudget());
    Tracing().shutdown();
}

}  // namespace flapi

#endif  // FLAPI_WITH_TRACING
