// Issue 3 - W3C Trace Context extraction, and the SEP-414 conformance defect.
//
// flAPI advertises MCP revision 2026-07-28. That revision, via SEP-414 (status
// Final), reserves `traceparent`, `tracestate` and `baggage` inside params._meta
// as UNPREFIXED keys - a documented exception to MCP's reverse-DNS convention,
// made precisely so implementations do not invent
// io.modelcontextprotocol.traceparent and break correlation.
//
// flAPI already parses params._meta for three io.modelcontextprotocol/* keys and
// walks straight past traceparent, so every conforming client's trace context is
// discarded and every flAPI call is a hole in its caller's trace.
//
// Parsing is strict per W3C: a malformed traceparent is REJECTED and a new root
// trace begins. It must never abort the request - a broken header from an
// upstream is not the caller's fault and not worth a 400.
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "trace_context.hpp"

using namespace flapi;

TEST_CASE("a valid traceparent is parsed", "[trace_context]") {
    const auto ids = parseTraceparent("00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01");
    REQUIRE(ids.valid());
    REQUIRE(ids.trace_id == "4bf92f3577b34da6a3ce929d0e0e4736");
    REQUIRE(ids.span_id == "00f067aa0ba902b7");
    REQUIRE(ids.sampled());
}

TEST_CASE("the sampled flag is decoded", "[trace_context]") {
    REQUIRE(parseTraceparent("00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01").sampled());
    REQUIRE_FALSE(parseTraceparent("00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-00").sampled());
}

TEST_CASE("malformed traceparents are rejected", "[trace_context]") {
    // Each of these must yield an invalid context - never a throw, never a
    // partially-populated one that would produce a mis-parented span.
    const char* bad[] = {
        "",
        "not-a-traceparent",
        "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7",            // too few fields
        "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01-extra",   // too many
        "ff-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01",         // version ff is forbidden
        "0-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01",          // short version
        "00-4bf92f3577b34da6a3ce929d0e0e473-00f067aa0ba902b7-01",          // 31-char trace id
        "00-4bf92f3577b34da6a3ce929d0e0e47366-00f067aa0ba902b7-01",        // 33-char trace id
        "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b-01",          // 15-char span id
        "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b77-01",        // 17-char span id
        "00-ZZf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01",         // non-hex trace id
        "00-4bf92f3577b34da6a3ce929d0e0e4736-ZZf067aa0ba902b7-01",         // non-hex span id
        "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-ZZ",         // non-hex flags
        "00-00000000000000000000000000000000-00f067aa0ba902b7-01",         // all-zero trace id
        "00-4bf92f3577b34da6a3ce929d0e0e4736-0000000000000000-01",         // all-zero span id
        "00-4BF92F3577B34DA6A3CE929D0E0E4736-00f067aa0ba902b7-01",         // uppercase is invalid
    };
    for (const char* s : bad) {
        INFO("input: " << s);
        REQUIRE_FALSE(parseTraceparent(s).valid());
    }
}

TEST_CASE("a future version with extra fields is accepted forward-compatibly", "[trace_context]") {
    // W3C says an unknown version MUST be parsed as far as it is understood
    // rather than discarded, so a newer caller is not silently un-traced.
    const auto ids = parseTraceparent("01-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01-future");
    REQUIRE(ids.valid());
    REQUIRE(ids.trace_id == "4bf92f3577b34da6a3ce929d0e0e4736");
}

TEST_CASE("tracestate is carried and clamped", "[trace_context]") {
    auto ids = parseTraceparent("00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01",
                                "vendor1=value1,vendor2=value2");
    REQUIRE(ids.tracestate == "vendor1=value1,vendor2=value2");

    // tracestate is attacker-influenced input from an upstream we do not control,
    // so it is bounded before it is ever stored or propagated.
    const std::string huge(4096, 'a');
    auto clamped = parseTraceparent("00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01", huge);
    REQUIRE(clamped.valid());
    REQUIRE(clamped.tracestate.size() <= kMaxTracestateBytes);
}

TEST_CASE("formatTraceparent round-trips", "[trace_context]") {
    const std::string original = "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01";
    REQUIRE(formatTraceparent(parseTraceparent(original)) == original);
}

TEST_CASE("_meta wins over the HTTP header, and disagreement is recorded", "[trace_context]") {
    // The MCP semantic conventions say server instrumentation SHOULD prefer the
    // context from params._meta: over a proxy or gateway the HTTP hop may be the
    // gateway's own span, while _meta carries the agent's.
    const auto from_meta = parseTraceparent("00-11111111111111111111111111111111-1111111111111111-01");
    const auto from_header = parseTraceparent("00-22222222222222222222222222222222-2222222222222222-01");

    SECTION("both present and disagreeing") {
        const auto resolved = resolvePrecedence(from_meta, from_header);
        REQUIRE(resolved.source == ContextSource::MetaOverHeader);
        REQUIRE(resolved.ids.trace_id == "11111111111111111111111111111111");
    }
    SECTION("both present and agreeing") {
        const auto resolved = resolvePrecedence(from_meta, from_meta);
        REQUIRE(resolved.source == ContextSource::Meta);
    }
    SECTION("meta only") {
        const auto resolved = resolvePrecedence(from_meta, SpanContextIds{});
        REQUIRE(resolved.source == ContextSource::Meta);
        REQUIRE(resolved.ids.trace_id == "11111111111111111111111111111111");
    }
    SECTION("header only") {
        const auto resolved = resolvePrecedence(SpanContextIds{}, from_header);
        REQUIRE(resolved.source == ContextSource::Header);
        REQUIRE(resolved.ids.trace_id == "22222222222222222222222222222222");
    }
    SECTION("a malformed _meta falls back to a valid header") {
        const auto resolved = resolvePrecedence(parseTraceparent("garbage"), from_header);
        REQUIRE(resolved.source == ContextSource::Header);
    }
    SECTION("neither") {
        const auto resolved = resolvePrecedence(SpanContextIds{}, SpanContextIds{});
        REQUIRE(resolved.source == ContextSource::None);
        REQUIRE_FALSE(resolved.ids.valid());
    }
}

TEST_CASE("oversized and repeated input cannot exhaust memory", "[trace_context][security]") {
    // Trace context arrives from an upstream we do not control.
    REQUIRE_FALSE(parseTraceparent(std::string(1024 * 1024, 'a')).valid());
    REQUIRE_FALSE(parseTraceparent(std::string(64, '-')).valid());

    const std::string baggage(1024 * 1024, 'x');
    const auto ids = parseTraceparent("00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01", "", baggage);
    REQUIRE(ids.baggage.size() <= kMaxBaggageBytes);
}
