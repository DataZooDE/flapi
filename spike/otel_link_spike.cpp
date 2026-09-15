// Issue -1 link spike: does opentelemetry-cpp 1.17.0 (vcpkg pinned baseline)
// link alongside statically-linked DuckDB under -Wl,--no-undefined?
//
// Exercises exactly the surface the plan depends on:
//   - TracerProvider + BatchSpanProcessor         (lifecycle, background thread)
//   - OTLP HTTP exporter, JSON content type       (issue 6 secondary harness)
//   - HttpTraceContext propagator                 (W3C extract/inject, issue 3)
//   - ParentBased + TraceIdRatioBased sampler     (tracing.sample, issue 5)
//   - Resource attributes                         (service.name, issue 5)
//   - A custom SpanExporter                       (issue 6: the missing otlp-file)
//
// It must COMPILE and LINK. Running it is a bonus; nothing here needs a collector.
#include <opentelemetry/exporters/otlp/otlp_http_exporter_factory.h>
#include <opentelemetry/exporters/otlp/otlp_http_exporter_options.h>
#include <opentelemetry/sdk/trace/batch_span_processor_factory.h>
#include <opentelemetry/sdk/trace/batch_span_processor_options.h>
#include <opentelemetry/sdk/trace/tracer_provider_factory.h>
#include <opentelemetry/sdk/trace/simple_processor_factory.h>
#include <opentelemetry/sdk/trace/exporter.h>
#include <opentelemetry/sdk/trace/span_data.h>
#include <opentelemetry/sdk/trace/samplers/parent_factory.h>
#include <opentelemetry/sdk/trace/samplers/trace_id_ratio_factory.h>
#include <opentelemetry/sdk/resource/resource.h>
#include <opentelemetry/trace/provider.h>
#include <opentelemetry/trace/propagation/http_trace_context.h>
#include <opentelemetry/context/propagation/text_map_propagator.h>

#include <cstdio>
#include <map>
#include <memory>
#include <string>

namespace otlp   = opentelemetry::exporter::otlp;
namespace trace_sdk = opentelemetry::sdk::trace;
namespace res   = opentelemetry::sdk::resource;
namespace trace_api = opentelemetry::trace;
namespace nostd = opentelemetry::nostd;

// ── The custom exporter the plan needs, because 1.17.0 has no otlp-file ──────
class JsonFileSpanExporter final : public trace_sdk::SpanExporter {
public:
    std::unique_ptr<trace_sdk::Recordable> MakeRecordable() noexcept override {
        return std::unique_ptr<trace_sdk::Recordable>(new trace_sdk::SpanData());
    }
    opentelemetry::sdk::common::ExportResult Export(
        const nostd::span<std::unique_ptr<trace_sdk::Recordable>>& spans) noexcept override {
        count_ += spans.size();
        return opentelemetry::sdk::common::ExportResult::kSuccess;
    }
    bool ForceFlush(std::chrono::microseconds) noexcept override { return true; }
    bool Shutdown(std::chrono::microseconds) noexcept override { return true; }
    std::size_t count() const { return count_; }
private:
    std::size_t count_ = 0;
};

// ── A carrier so the propagator surface is actually linked ───────────────────
struct MapCarrier : public opentelemetry::context::propagation::TextMapCarrier {
    std::map<std::string, std::string> headers;
    nostd::string_view Get(nostd::string_view key) const noexcept override {
        auto it = headers.find(std::string(key));
        return it == headers.end() ? nostd::string_view{} : nostd::string_view{it->second};
    }
    void Set(nostd::string_view key, nostd::string_view value) noexcept override {
        headers[std::string(key)] = std::string(value);
    }
};

int main() {
    // 1. OTLP HTTP exporter, JSON content type (what the Python collector test uses)
    otlp::OtlpHttpExporterOptions opts;
    opts.url          = "http://localhost:4318/v1/traces";
    opts.content_type = otlp::HttpRequestContentType::kJson;
    auto http_exporter = otlp::OtlpHttpExporterFactory::Create(opts);
    std::printf("otlp http exporter: %s\n", http_exporter ? "ok" : "NULL");

    // 2. BatchSpanProcessor — the background export thread
    trace_sdk::BatchSpanProcessorOptions bsp;
    bsp.max_queue_size = 2048;
    auto processor = trace_sdk::BatchSpanProcessorFactory::Create(std::move(http_exporter), bsp);

    // 3. Sampler: parentbased_traceidratio
    auto sampler = trace_sdk::ParentBasedSamplerFactory::Create(
        trace_sdk::TraceIdRatioBasedSamplerFactory::Create(1.0));

    // 4. Resource attributes
    auto resource = res::Resource::Create({{"service.name", "flapi"},
                                           {"service.version", "spike"}});

    // 5. Provider
    auto provider = trace_sdk::TracerProviderFactory::Create(
        std::move(processor), resource, std::move(sampler));
    std::printf("tracer provider: %s\n", provider ? "ok" : "NULL");

    // 6. The custom file exporter, through a simple processor on its own provider
    auto file_exp = std::unique_ptr<trace_sdk::SpanExporter>(new JsonFileSpanExporter());
    auto* file_raw = static_cast<JsonFileSpanExporter*>(file_exp.get());
    auto simple = trace_sdk::SimpleSpanProcessorFactory::Create(std::move(file_exp));
    auto file_provider = trace_sdk::TracerProviderFactory::Create(std::move(simple));

    {
        auto tracer = file_provider->GetTracer("flapi-spike");
        auto span   = tracer->StartSpan("spike.span");
        span->SetAttribute("http.route", "/customers/{id}");
        span->End();
    }
    std::printf("custom exporter received %zu span(s)\n", file_raw->count());

    // 7. W3C propagator extract + inject
    trace_api::propagation::HttpTraceContext propagator;
    MapCarrier in;
    in.headers["traceparent"] = "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01";
    auto root = opentelemetry::context::RuntimeContext::GetCurrent();   // Extract takes Context&
    auto ctx  = propagator.Extract(in, root);
    auto sctx = trace_api::GetSpan(ctx)->GetContext();

    char tid[32]; sctx.trace_id().ToLowerBase16(tid);
    std::printf("extracted trace_id: %.32s (valid=%d)\n", tid, sctx.IsValid() ? 1 : 0);

    MapCarrier out;
    propagator.Inject(out, ctx);
    std::printf("injected traceparent: %s\n", out.headers["traceparent"].c_str());

    static_cast<trace_sdk::TracerProvider*>(file_provider.get())->Shutdown();
    static_cast<trace_sdk::TracerProvider*>(provider.get())->Shutdown();
    std::printf("SPIKE OK\n");
    return 0;
}
