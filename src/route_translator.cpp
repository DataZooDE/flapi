#include "route_translator.hpp"

#include <crow/logging.h>

#include <mutex>
#include <regex>
#include <shared_mutex>
#include <string_view>
#include <unordered_map>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdint>

namespace flapi {

const std::regex RouteTranslator::param_regex(":([^/]+)");

namespace {

// A route pattern compiles to the same regex every time - it is derived from
// EndpointConfig::urlPath, which only changes on config reload. Rebuilding it per
// call cost ~6us for a one-parameter route, and ConfigManager::getEndpointForPathAndMethod
// scans every endpoint calling matchesPath, from three separate places per request
// (RateLimitMiddleware, AuthMiddleware, handleDynamicRequest). On the examples
// config that was ~160us of pure routing per request, against a ~200us
// time-to-first-byte for /health - plausibly the bulk of the unexplained
// "degrades under high concurrent load" behaviour.
//
// So: compile once, keyed by pattern. Patterns come from configuration, never
// from the request, so the key space is bounded by the endpoint count; the cap
// below is belt-and-braces against a future caller passing request-derived text.
struct CompiledRoute {
    std::regex regex;
    std::vector<std::string> param_names;
    // False when the pattern did not compile: such a route matches nothing.
    bool valid = true;
};

constexpr std::size_t kMaxCachedRoutes = 4096;

std::shared_mutex& cacheMutex() {
    static std::shared_mutex m;
    return m;
}

std::unordered_map<std::string, std::shared_ptr<const CompiledRoute>>& cache() {
    static std::unordered_map<std::string, std::shared_ptr<const CompiledRoute>> c;
    return c;
}

std::shared_ptr<const CompiledRoute> compiledRouteFor(const std::string& pattern) {
    {
        std::shared_lock<std::shared_mutex> read(cacheMutex());
        auto it = cache().find(pattern);
        if (it != cache().end()) {
            return it->second;
        }
    }

    // Build outside the write lock; a concurrent duplicate build is cheaper than
    // serialising every miss, and the result is identical either way.
    auto compiled = std::make_shared<CompiledRoute>();
    const std::string regex_pattern =
        RouteTranslator::translateRoutePath(pattern, compiled->param_names);
    // A pattern that does not compile used to throw from here, inside
    // routing, on EVERY request that reached it in the endpoint scan - the
    // worker died without answering, so one bad url-path (`/customers/{cid}`)
    // hung the whole server, /health included. url-paths are validated at
    // load and literals are escaped below, so this is belt-and-braces: the
    // route matches nothing and says why, once.
    try {
        compiled->regex = std::regex(regex_pattern);
    } catch (const std::regex_error& e) {
        CROW_LOG_ERROR << "Route " << pattern << " cannot be matched (" << e.what()
                       << "); it answers no request. Fix its url-path.";
        compiled->valid = false;
    }

    std::unique_lock<std::shared_mutex> write(cacheMutex());
    if (cache().size() >= kMaxCachedRoutes) {
        // Configuration cannot realistically reach this; if it does, stop growing
        // rather than serve a memory-exhaustion vector.
        return compiled;
    }
    auto [it, inserted] = cache().emplace(pattern, std::move(compiled));
    return it->second;
}

}  // namespace

std::string RouteTranslator::translateRoutePath(const std::string& flapiPath, std::vector<std::string>& paramNames) {
    std::string crowPath;
    crowPath.reserve(flapiPath.size() + 16);

    // Single pass, replacing ":name" with a capture group as it goes. The previous
    // implementation ran a std::regex_replace - constructing a fresh std::regex -
    // once per parameter, on top of the sregex_iterator scan.
    std::size_t i = 0;
    while (i < flapiPath.size()) {
        if (flapiPath[i] == ':') {
            std::size_t start = i + 1;
            std::size_t end = start;
            while (end < flapiPath.size() && flapiPath[end] != '/') {
                ++end;
            }
            if (end > start) {
                paramNames.push_back(flapiPath.substr(start, end - start));
                crowPath += "([^/]+)";
                i = end;
                continue;
            }
        }
        // A literal is matched literally. Copied raw, `.` matched any
        // character and `{`, `(`, `[` made the pattern fail to compile.
        if (std::string_view("\\^$.|?*+()[]{}").find(flapiPath[i]) != std::string_view::npos) {
            crowPath += '\\';
        }
        crowPath += flapiPath[i];
        ++i;
    }

    return "^" + crowPath + "$";
}

bool RouteTranslator::matchAndExtractParams(const std::string& routePattern, const std::string& actualPath,
                                            std::vector<std::string>& paramNames, std::map<std::string, std::string>& pathParams) {
    const auto compiled = compiledRouteFor(routePattern);

    if (!compiled->valid) {
        return false;
    }
    std::smatch matches;
    if (std::regex_match(actualPath, matches, compiled->regex)) {
        for (size_t i = 1; i < matches.size(); ++i) {
            pathParams[compiled->param_names[i - 1]] = matches[i].str();
        }
        paramNames = compiled->param_names;
        return true;
    }

    return false;
}

namespace {

int hexDigitValue(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool isValidUtf8(const std::string& s) {
    std::size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        std::size_t extra = 0;
        std::uint32_t code_point = 0;
        std::uint32_t minimum = 0;
        if (c < 0x80) {
            ++i;
            continue;
        }
        if ((c & 0xE0) == 0xC0) {
            extra = 1;
            code_point = c & 0x1F;
            minimum = 0x80;
        } else if ((c & 0xF0) == 0xE0) {
            extra = 2;
            code_point = c & 0x0F;
            minimum = 0x800;
        } else if ((c & 0xF8) == 0xF0) {
            extra = 3;
            code_point = c & 0x07;
            minimum = 0x10000;
        } else {
            return false;
        }
        if (s.size() - i <= extra) {
            return false;   // truncated sequence
        }
        for (std::size_t k = 1; k <= extra; ++k) {
            const auto cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) {
                return false;
            }
            code_point = (code_point << 6) | (cc & 0x3F);
        }
        if (code_point < minimum || code_point > 0x10FFFF || (code_point >= 0xD800 && code_point <= 0xDFFF)) {
            return false;
        }
        i += extra + 1;
    }
    return true;
}

} // namespace

bool RouteTranslator::percentDecodePathValue(const std::string& raw, std::string& out) {
    out.clear();
    out.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] != '%') {
            out += raw[i];
            continue;
        }
        if (raw.size() - i < 3) {
            return false;   // truncated escape
        }
        const int hi = hexDigitValue(raw[i + 1]);
        const int lo = hexDigitValue(raw[i + 2]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        const char decoded = static_cast<char>((hi << 4) | lo);
        if (decoded == '\0') {
            return false;   // an encoded NUL ends C strings and file paths early
        }
        out += decoded;
        i += 2;
    }
    return isValidUtf8(out);
}

namespace {

struct RouteSegment {
    bool param;
    std::string text;
};

// The segments a url-path is matched by, with translateRoutePath's notion of a
// parameter (`:` followed by at least one character). A trailing `/` is an
// empty literal segment, which no parameter matches (`[^/]+`).
std::vector<RouteSegment> routeSegments(const std::string& url_path) {
    std::vector<RouteSegment> segments;
    std::size_t start = (!url_path.empty() && url_path.front() == '/') ? 1 : 0;
    while (true) {
        const std::size_t end = url_path.find('/', start);
        const std::string part = url_path.substr(start, end == std::string::npos ? std::string::npos : end - start);
        segments.push_back({part.size() > 1 && part.front() == ':', part});
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return segments;
}

// The character at `text[pos]` for a message: the whole UTF-8 sequence (not
// its first byte, which printed as mojibake for `é`) plus its code point.
std::string describeChar(const std::string& text, std::size_t pos) {
    const auto lead = static_cast<unsigned char>(text[pos]);
    if (lead < 0x80) {
        return "'" + std::string(1, text[pos]) + "'";
    }
    std::size_t len = 0;
    uint32_t cp = 0;
    if ((lead & 0xE0) == 0xC0) {
        len = 2;
        cp = lead & 0x1F;
    } else if ((lead & 0xF0) == 0xE0) {
        len = 3;
        cp = lead & 0x0F;
    } else if ((lead & 0xF8) == 0xF0) {
        len = 4;
        cp = lead & 0x07;
    }
    bool valid = len != 0 && pos + len <= text.size();
    for (std::size_t i = 1; valid && i < len; ++i) {
        const auto cont = static_cast<unsigned char>(text[pos + i]);
        valid = (cont & 0xC0) == 0x80;
        cp = (cp << 6) | (cont & 0x3F);
    }
    char hex[16];
    if (!valid) {
        std::snprintf(hex, sizeof(hex), "0x%02X", lead);
        return std::string("an invalid UTF-8 byte ") + hex;
    }
    std::snprintf(hex, sizeof(hex), "U+%04X", static_cast<unsigned int>(cp));
    return "'" + text.substr(pos, len) + "' (" + hex + ")";
}

bool isLiteralChar(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
           c == '~' || c == '-';
}

}  // namespace

std::string RouteTranslator::urlPathProblem(const std::string& url_path) {
    if (url_path.empty() || url_path.front() != '/') {
        return "url-path '" + url_path + "' must start with /";
    }
    if (url_path == "/") {
        return "";
    }
    const auto segments = routeSegments(url_path);
    std::vector<std::string> param_names;
    for (std::size_t i = 0; i < segments.size(); ++i) {
        const auto& segment = segments[i];
        const std::string& text = segment.text;
        if (text.empty()) {
            if (i + 1 == segments.size()) {
                continue;   // one trailing slash: /customers/
            }
            return "url-path '" + url_path + "' has an empty segment (`//`)";
        }
        if (text.front() == ':') {
            const std::string name = text.substr(1);
            bool ok = !name.empty() && (std::isalpha(static_cast<unsigned char>(name.front())) || name.front() == '_');
            for (const char c : name) {
                ok = ok && (std::isalnum(static_cast<unsigned char>(c)) || c == '_');
            }
            if (!ok) {
                return "url-path '" + url_path + "': parameter segment '" + text +
                       "' must be `:name` with a name of letters, digits and _ (not starting with a digit)";
            }
            // Both would be bound to one `params.<name>`: the template would see
            // only one of the two values.
            if (std::find(param_names.begin(), param_names.end(), name) != param_names.end()) {
                return "url-path '" + url_path + "': parameter `:" + name +
                       "` appears more than once; give each path parameter its own name";
            }
            param_names.push_back(name);
            continue;
        }
        // Clients and proxies normalise `.` and `..` away (`/a/../b` is sent
        // as `/b`), so such a route never matches what was written - or matches
        // something else after normalisation.
        if (text == "." || text == "..") {
            return "url-path '" + url_path + "': segment '" + text +
                   "' is not allowed (clients and proxies resolve dot segments before the request reaches flapi)";
        }
        for (std::size_t k = 0; k < text.size(); ++k) {
            const char c = text[k];
            if (!isLiteralChar(c)) {
                std::string hint;
                if (text.size() > 2 && text.front() == '{' && text.back() == '}') {
                    hint = "; path parameters are written `:" + text.substr(1, text.size() - 2) + "`, not `" + text +
                           "`";
                }
                return "url-path '" + url_path + "': segment '" + text + "' contains " + describeChar(text, k) +
                       "; a literal segment may only use letters, digits and . _ ~ -" + hint;
            }
        }
    }
    return "";
}

bool RouteTranslator::routesOverlap(const std::string& a, const std::string& b) {
    const auto sa = routeSegments(a);
    const auto sb = routeSegments(b);
    if (sa.size() != sb.size()) {
        return false;
    }
    for (std::size_t i = 0; i < sa.size(); ++i) {
        if (!sa[i].param && !sb[i].param) {
            if (sa[i].text != sb[i].text) {
                return false;
            }
        } else if (!sa[i].param || !sb[i].param) {
            // A parameter matches one or more characters, never the empty
            // segment after a trailing slash.
            if ((sa[i].param ? sb[i] : sa[i]).text.empty()) {
                return false;
            }
        }
    }
    return true;
}

bool RouteTranslator::routeCovers(const std::string& general, const std::string& specific) {
    const auto sg = routeSegments(general);
    const auto ss = routeSegments(specific);
    if (sg.size() != ss.size()) {
        return false;
    }
    for (std::size_t i = 0; i < sg.size(); ++i) {
        if (sg[i].param) {
            // A parameter matches one or more characters, never the empty
            // segment after a trailing slash.
            if (!ss[i].param && ss[i].text.empty()) {
                return false;
            }
        } else if (ss[i].param || sg[i].text != ss[i].text) {
            return false;
        }
    }
    return true;
}

int RouteTranslator::compareSpecificity(const std::string& a, const std::string& b) {
    const auto sa = routeSegments(a);
    const auto sb = routeSegments(b);
    const std::size_t n = std::min(sa.size(), sb.size());
    for (std::size_t i = 0; i < n; ++i) {
        if (sa[i].param != sb[i].param) {
            return sa[i].param ? 1 : -1;
        }
    }
    // Routes of different lengths never match one request; ordered anyway so
    // the comparison is total.
    if (sa.size() != sb.size()) {
        return sa.size() < sb.size() ? -1 : 1;
    }
    return 0;
}

} // namespace flapi
