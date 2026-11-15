#include "mcp_description_scanner.hpp"

#include <algorithm>
#include <array>
#include <cctype>

namespace flapi {

namespace {

bool isSuspiciousControlByte(unsigned char c) {
    // Allow common formatting whitespace; flag everything else below 0x20
    // plus the DEL character. Bytes >= 0x80 are valid UTF-8 continuation
    // and not addressed here.
    if (c == '\n' || c == '\r' || c == '\t') {
        return false;
    }
    return c < 0x20 || c == 0x7F;
}

std::string toLower(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

// The matched phrase and its byte offset, or nullptr. Named in the message:
// "contains a phrase commonly used..." left the author to guess which one -
// `system:` in "Reads the billing system: totals" is easy to miss.
const char* findPromptInjectionPhrase(const std::string& description_lower, std::size_t& offset) {
    // Phrases observed in the prompt-injection corpus. Kept short and
    // anchored so legitimate uses of common words like "ignore" don't
    // trip the detector.
    static const std::array<const char*, 6> kPhrases = {
        "ignore previous instructions",
        "ignore the above",
        "disregard previous instructions",
        "disregard the above",
        "system:",
        "you are now",
    };
    const char* first = nullptr;
    for (const char* phrase : kPhrases) {
        const auto at = description_lower.find(phrase);
        if (at != std::string::npos && (first == nullptr || at < offset)) {
            first = phrase;
            offset = at;
        }
    }
    return first;
}

} // namespace

std::vector<DescriptionIssue> MCPDescriptionScanner::scan(const std::string& description) const {
    std::vector<DescriptionIssue> issues;

    for (char ch : description) {
        if (isSuspiciousControlByte(static_cast<unsigned char>(ch))) {
            issues.push_back({
                "DESCRIPTION_CONTROL_CHARACTER",
                "MCP tool description contains a control character (NUL, BEL, etc.). "
                "Strip non-printable bytes; only newline, carriage return, and tab are tolerated."
            });
            break;  // One report per description is enough.
        }
    }

    if (description.size() > kMaxDescriptionLength) {
        issues.push_back({
            "DESCRIPTION_TOO_LONG",
            "MCP tool description exceeds " + std::to_string(kMaxDescriptionLength) +
            " bytes. Long descriptions waste model context and are sometimes used to "
            "drown out user prompts; trim or move detail to documentation."
        });
    }

    std::size_t offset = 0;
    if (const char* phrase = findPromptInjectionPhrase(toLower(description), offset)) {
        issues.push_back({
            "DESCRIPTION_PROMPT_INJECTION",
            std::string("MCP tool description contains '") + phrase + "' at offset " +
                std::to_string(offset) +
                ", a phrase commonly used in prompt-injection attempts. If this is "
                "intentional copy, rephrase; otherwise treat the YAML as compromised."
        });
    }

    return issues;
}

} // namespace flapi
