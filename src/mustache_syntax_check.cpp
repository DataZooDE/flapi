#include "mustache_syntax_check.hpp"

#include "path_validator.hpp"
#include "vfs_adapter.hpp"

#include <stdexcept>
#include <vector>

namespace flapi {

namespace {

constexpr const char* kOpen = "{{";
constexpr const char* kClose = "}}";
constexpr std::size_t kDelimiterSize = 2;

// crow trims only ' ' around a tag name, so this does too: a name it would see
// as "a\t" must stay distinct from "a" here.
std::string TrimSpaces(const std::string& body, std::size_t begin, std::size_t end) {
    while (begin < end && body[begin] == ' ') {
        ++begin;
    }
    while (end > begin && body[end - 1] == ' ') {
        --end;
    }
    return body.substr(begin, end - begin);
}

MustacheSyntaxError ErrorAt(const std::string& body, std::size_t offset, std::string message) {
    MustacheSyntaxError error;
    error.message = std::move(message);
    for (std::size_t i = 0; i < offset && i < body.size(); ++i) {
        if (body[i] == '\n') {
            ++error.line;
            error.column = 1;
        } else {
            ++error.column;
        }
    }
    return error;
}

struct OpenSection {
    std::string name;
    char kind;           // '#' or '^'
    std::size_t offset;  // of the opening "{{"
};

} // namespace

std::optional<MustacheSyntaxError> CheckMustacheSyntax(const std::string& body) {
    std::vector<OpenSection> sections;
    std::size_t current = 0;

    while (true) {
        const std::size_t tag_start = body.find(kOpen, current);
        if (tag_start == std::string::npos) {
            break;
        }
        const std::size_t idx = tag_start + kDelimiterSize;
        const std::size_t close = body.find(kClose, idx);
        if (close == std::string::npos) {
            return ErrorAt(body, tag_start, "'{{' is never closed by '}}'");
        }
        if (close == idx) {
            return ErrorAt(body, tag_start, "empty tag '{{}}'");
        }
        current = close + kDelimiterSize;

        const char kind = body[idx];
        switch (kind) {
            case '#':
            case '^': {
                std::string name = TrimSpaces(body, idx + 1, close);
                if (name.empty()) {
                    return ErrorAt(body, tag_start, std::string("section tag '{{") + kind + "}}' has no name");
                }
                sections.push_back(OpenSection{std::move(name), kind, tag_start});
                break;
            }
            case '/': {
                const std::string name = TrimSpaces(body, idx + 1, close);
                if (name.empty()) {
                    return ErrorAt(body, tag_start, "closing tag '{{/}}' has no name");
                }
                if (sections.empty()) {
                    return ErrorAt(body, tag_start,
                                   "'{{/" + name + "}}' closes a section that was never opened (no matching '{{#" +
                                       name + "}}' or '{{^" + name + "}}')");
                }
                const OpenSection& open = sections.back();
                if (open.name != name) {
                    const auto opened_at = ErrorAt(body, open.offset, "");
                    return ErrorAt(body, tag_start,
                                   "'{{/" + name + "}}' does not match the innermost open section '{{" + open.kind +
                                       open.name + "}}' (opened at line " + std::to_string(opened_at.line) +
                                       "); sections must be closed in reverse order");
                }
                sections.pop_back();
                break;
            }
            case '{': {
                // crow requires the third brace right after "}}" and skips it.
                if (close + kDelimiterSize >= body.size() || body[close + kDelimiterSize] != '}') {
                    return ErrorAt(body, tag_start, "'{{{' must be closed by '}}}'");
                }
                if (TrimSpaces(body, idx + 1, close).empty()) {
                    return ErrorAt(body, tag_start, "empty tag '{{{}}}'");
                }
                current++;
                break;
            }
            case '&': {
                if (TrimSpaces(body, idx + 1, close).empty()) {
                    return ErrorAt(body, tag_start, "empty tag '{{&}}'");
                }
                break;
            }
            case '!':
                // A comment; anything goes up to the first "}}".
                break;
            case '>':
                return ErrorAt(body, tag_start,
                               "partials ('{{> name}}') are not supported in flAPI templates");
            case '=':
                return ErrorAt(body, tag_start,
                               "set-delimiter tags ('{{= =}}') are not supported in flAPI templates");
            default: {
                if (TrimSpaces(body, idx, close).empty()) {
                    return ErrorAt(body, tag_start, "empty tag '{{ }}'");
                }
                break;
            }
        }
    }

    if (!sections.empty()) {
        const OpenSection& open = sections.back();
        return ErrorAt(body, open.offset,
                       std::string("section '{{") + open.kind + open.name + "}}' is never closed (missing '{{/" +
                           open.name + "}}')");
    }
    return std::nullopt;
}

std::string FormatMustacheSyntaxError(const MustacheSyntaxError& error, const std::string& label) {
    if (label.empty()) {
        return "line " + std::to_string(error.line) + ", column " + std::to_string(error.column) + ": " +
               error.message;
    }
    return label + ":" + std::to_string(error.line) + ":" + std::to_string(error.column) + ": " + error.message;
}

void RequireValidMustacheSyntax(const std::string& body, const std::string& label) {
    if (const auto error = CheckMustacheSyntax(body)) {
        throw std::invalid_argument("Invalid template " + FormatMustacheSyntaxError(*error, label));
    }
}

std::optional<std::string> CheckMustacheTemplateFile(const std::string& path) {
    if (path.empty() || PathValidator::IsRemotePath(path)) {
        return std::nullopt;
    }
    std::string content;
    try {
        auto provider = FileProviderFactory::CreateProvider(path);
        if (!provider->FileExists(path)) {
            return std::nullopt;
        }
        content = provider->ReadFile(path);
    } catch (const std::exception&) {
        // Unreadable here is reported by the existence checks, not as syntax.
        return std::nullopt;
    }
    if (const auto error = CheckMustacheSyntax(content)) {
        return "Invalid template " + FormatMustacheSyntaxError(*error, path);
    }
    return std::nullopt;
}

} // namespace flapi
