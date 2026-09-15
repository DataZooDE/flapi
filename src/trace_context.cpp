#include "trace_context.hpp"

#include <algorithm>

namespace flapi {

namespace {

bool isHex(std::string_view s) {
    return std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    });
}

// W3C requires lowercase on the wire, but tolerant parsers accept uppercase and
// some propagators emit it. Rejecting it outright would silently start a NEW root
// trace against a conforming upstream - the exact failure this epic exists to
// remove - so normalise instead.
std::string toLowerHex(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(c >= 'A' && c <= 'F' ? c - 'A' + 'a' : c);
    });
    return out;
}

bool hasControlChars(std::string_view s) {
    return std::any_of(s.begin(), s.end(), [](unsigned char c) {
        return c < 0x20 || c == 0x7F;
    });
}

// Trim optional whitespace, which HTTP permits around a field value.
std::string_view trimOws(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) { s.remove_prefix(1); }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) { s.remove_suffix(1); }
    return s;
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

// Truncate at a LIST-MEMBER boundary, per W3C §3.3.1.
//
// A raw substr can cut inside a key or value, and propagating
// "...,vendorX=abc12" makes the next hop reject the entire header - breaking the
// trace beyond flAPI rather than merely shortening it. Drop whole members from
// the right instead, and drop the field entirely if even one member does not fit.
// Values carrying control characters are rejected outright: they reach exporters
// and outbound headers, where a CR/LF is a response-splitting primitive.
std::string clampListValue(std::string_view s, std::size_t max_bytes) {
    s = trimOws(s);
    if (s.empty() || hasControlChars(s)) {
        return {};
    }
    if (s.size() <= max_bytes) {
        return std::string(s);
    }
    const auto cut = s.rfind(',', max_bytes);
    if (cut == std::string_view::npos) {
        return {};
    }
    return std::string(trimOws(s.substr(0, cut)));
}

}  // namespace

SpanContextIds parseTraceparent(std::string_view traceparent,
                                std::string_view tracestate,
                                std::string_view baggage) {
    SpanContextIds out;
    traceparent = trimOws(traceparent);

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

    if (!isHex(version) || !isHex(trace_id) || !isHex(span_id) || !isHex(flags)) {
        return out;
    }
    // Version ff is forbidden by the spec (compare case-insensitively, since
    // uppercase input is normalised rather than rejected).
    if (toLowerHex(version) == "ff") {
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

    out.trace_id = toLowerHex(trace_id);
    out.span_id = toLowerHex(span_id);
    out.flags = parseHexByte(toLowerHex(flags));
    out.tracestate = clampListValue(tracestate, kMaxTracestateBytes);
    out.baggage = clampListValue(baggage, kMaxBaggageBytes);
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
