#include "trace_context.hpp"

#include <algorithm>

namespace flapi {

namespace {

bool isLowerHex(std::string_view s) {
    return std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

bool isAllZero(std::string_view s) {
    return std::all_of(s.begin(), s.end(), [](char c) { return c == '0'; });
}

std::uint8_t parseHexByte(std::string_view s) {
    std::uint8_t v = 0;
    for (char c : s) {
        v = static_cast<std::uint8_t>(v << 4);
        v = static_cast<std::uint8_t>(v | (c <= '9' ? c - '0' : c - 'a' + 10));
    }
    return v;
}

std::string clamp(std::string_view s, std::size_t max_bytes) {
    return std::string(s.substr(0, std::min(s.size(), max_bytes)));
}

}  // namespace

SpanContextIds parseTraceparent(std::string_view traceparent,
                                std::string_view tracestate,
                                std::string_view baggage) {
    SpanContextIds out;

    // Reject oversized input before doing any work: this is unbounded data from
    // an upstream, and the valid form is exactly 55 bytes (longer only for a
    // future version, which we cap generously).
    if (traceparent.size() < 55 || traceparent.size() > 256) {
        return out;
    }
    if (traceparent[2] != '-' || traceparent[35] != '-' || traceparent[52] != '-') {
        return out;
    }

    const auto version = traceparent.substr(0, 2);
    const auto trace_id = traceparent.substr(3, 32);
    const auto span_id = traceparent.substr(36, 16);
    const auto flags = traceparent.substr(53, 2);

    if (!isLowerHex(version) || !isLowerHex(trace_id) || !isLowerHex(span_id) || !isLowerHex(flags)) {
        return out;
    }
    // Version ff is forbidden by the spec.
    if (version == "ff") {
        return out;
    }
    // Version 00 is exactly 55 bytes. A later version may append fields, which
    // W3C says to parse as far as understood rather than discard - otherwise a
    // newer caller would be silently un-traced. Anything trailing must begin with
    // the field separator.
    if (version == "00") {
        if (traceparent.size() != 55) {
            return out;
        }
    } else if (traceparent.size() > 55 && traceparent[55] != '-') {
        return out;
    }

    // An all-zero id is the spec's "invalid" sentinel; accepting it would produce
    // spans parented to nothing under a shared, meaningless trace id.
    if (isAllZero(trace_id) || isAllZero(span_id)) {
        return out;
    }

    out.trace_id = std::string(trace_id);
    out.span_id = std::string(span_id);
    out.flags = parseHexByte(flags);
    out.tracestate = clamp(tracestate, kMaxTracestateBytes);
    out.baggage = clamp(baggage, kMaxBaggageBytes);
    return out;
}

std::string formatTraceparent(const SpanContextIds& ids) {
    if (!ids.valid()) {
        return {};
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(55);
    out += "00-";
    out += ids.trace_id;
    out += '-';
    out += ids.span_id;
    out += '-';
    out += kHex[(ids.flags >> 4) & 0xF];
    out += kHex[ids.flags & 0xF];
    return out;
}

ExtractedContext resolvePrecedence(const SpanContextIds& from_meta,
                                   const SpanContextIds& from_header) {
    ExtractedContext out;

    if (from_meta.valid()) {
        out.ids = from_meta;
        // Report disagreement rather than hide it: a mismatch usually means a
        // gateway inserted its own span, and an operator needs to be able to see
        // that rather than wonder why the parent looks wrong.
        const bool disagrees = from_header.valid() &&
                               (from_header.trace_id != from_meta.trace_id ||
                                from_header.span_id != from_meta.span_id);
        out.source = disagrees ? ContextSource::MetaOverHeader : ContextSource::Meta;
        return out;
    }

    if (from_header.valid()) {
        out.ids = from_header;
        out.source = ContextSource::Header;
        return out;
    }

    out.source = ContextSource::None;
    return out;
}

const char* contextSourceName(ContextSource source) {
    switch (source) {
        case ContextSource::Header:         return "header";
        case ContextSource::Meta:           return "meta";
        case ContextSource::MetaOverHeader: return "meta_over_header";
        case ContextSource::None:           return "none";
    }
    return "none";
}

}  // namespace flapi
