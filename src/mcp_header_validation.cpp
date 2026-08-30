#include "mcp_header_validation.hpp"

#include <cctype>
#include <cstdlib>

#include <crow/utility.h>

namespace flapi::mcp {

namespace {

// Trim ASCII whitespace from both ends.
std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) {
        ++b;
    }
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) {
        --e;
    }
    return s.substr(b, e - b);
}

// Parse a string strictly as a JSON number (integer or decimal). Returns false
// if there is any trailing garbage. Leading/trailing whitespace is tolerated.
bool parseNumber(const std::string& in, double& out) {
    std::string s = trim(in);
    if (s.empty()) {
        return false;
    }
    const char* begin = s.c_str();
    char* end = nullptr;
    out = std::strtod(begin, &end);
    return end == begin + s.size();
}

} // namespace

std::string decodeSentinel(const std::string& value) {
    // Sentinel form: =?base64?<payload>?=
    static const std::string kPrefix = "=?base64?";
    static const std::string kSuffix = "?=";
    if (value.size() < kPrefix.size() + kSuffix.size()) {
        return value;
    }
    if (value.compare(0, kPrefix.size(), kPrefix) != 0) {
        return value;
    }
    if (value.compare(value.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0) {
        return value;
    }
    std::string payload = value.substr(kPrefix.size(),
                                       value.size() - kPrefix.size() - kSuffix.size());
    try {
        std::string decoded = crow::utility::base64decode(payload);
        // base64decode does not report errors; if the payload was not valid
        // base64 the decoded bytes are meaningless and the later comparison
        // fails, which is the intended -32020 outcome.
        return decoded;
    } catch (...) {
        return value;
    }
}

bool numericEquals(const std::string& a, const std::string& b) {
    double na = 0, nb = 0;
    if (parseNumber(a, na) && parseNumber(b, nb)) {
        return na == nb;
    }
    return false;
}

bool headerMatches(const std::string& header_value, const std::string& body_value) {
    std::string decoded = decodeSentinel(header_value);
    if (decoded == body_value) {
        return true;
    }
    return numericEquals(decoded, body_value);
}

} // namespace flapi::mcp
