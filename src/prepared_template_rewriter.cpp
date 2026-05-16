#include "prepared_template_rewriter.hpp"

#include <algorithm>

#include "config_manager.hpp"

namespace flapi {

namespace {

// Tag types we recognise while scanning the template. `Sentinel` keeps
// the lexer code small without sprinkling magic chars.
enum class TagKind {
    OpenSection,       // {{#name}}
    OpenInvertedSection, // {{^name}}
    CloseSection,      // {{/name}}
    TripleBrace,       // {{{ ... }}}
    DoubleBrace,       // {{ ... }}
    NoTag,
};

struct TagScan {
    TagKind kind = TagKind::NoTag;
    std::size_t start = 0;       // index of the first `{`
    std::size_t end = 0;         // one past the last `}`
    std::string inner;           // trimmed content between braces
};

void appendRange(std::string& out, const std::string& src, std::size_t a, std::size_t b) {
    out.append(src, a, b - a);
}

std::string trim(std::string s) {
    auto begin = std::find_if_not(s.begin(), s.end(), [](char c) { return c == ' ' || c == '\t'; });
    s.erase(s.begin(), begin);
    auto rend = std::find_if_not(s.rbegin(), s.rend(), [](char c) { return c == ' ' || c == '\t'; });
    s.erase(rend.base(), s.end());
    return s;
}

bool startsWith(const std::string& s, std::size_t from, const char* prefix) {
    std::size_t n = 0;
    while (prefix[n] != '\0') ++n;
    if (s.size() < from + n) {
        return false;
    }
    return s.compare(from, n, prefix) == 0;
}

// Find the next Mustache-ish tag starting at `from`. Returns NoTag when
// no further `{{` appears in the template.
TagScan nextTag(const std::string& s, std::size_t from) {
    TagScan out;
    const std::size_t open = s.find("{{", from);
    if (open == std::string::npos) {
        return out;
    }
    out.start = open;

    // Triple-brace?
    if (startsWith(s, open, "{{{")) {
        const std::size_t close = s.find("}}}", open + 3);
        if (close == std::string::npos) {
            return out;  // unterminated; treat as no-tag
        }
        out.kind = TagKind::TripleBrace;
        out.end = close + 3;
        out.inner = trim(s.substr(open + 3, close - (open + 3)));
        return out;
    }

    const std::size_t close = s.find("}}", open + 2);
    if (close == std::string::npos) {
        return out;
    }
    out.end = close + 2;
    std::string raw = s.substr(open + 2, close - (open + 2));
    if (!raw.empty() && raw.front() == '#') {
        out.kind = TagKind::OpenSection;
        out.inner = trim(raw.substr(1));
    } else if (!raw.empty() && raw.front() == '^') {
        out.kind = TagKind::OpenInvertedSection;
        out.inner = trim(raw.substr(1));
    } else if (!raw.empty() && raw.front() == '/') {
        out.kind = TagKind::CloseSection;
        out.inner = trim(raw.substr(1));
    } else {
        out.kind = TagKind::DoubleBrace;
        out.inner = trim(raw);
    }
    return out;
}

// Extract X from "params.X". Returns empty string when the expression
// doesn't have the expected shape.
std::string paramName(const std::string& inner) {
    static const std::string kPrefix = "params.";
    if (inner.compare(0, kPrefix.size(), kPrefix) != 0) {
        return {};
    }
    return inner.substr(kPrefix.size());
}

const RequestFieldConfig* findField(const std::string& name,
                                    const std::vector<RequestFieldConfig>& fields) {
    for (const auto& f : fields) {
        if (f.fieldName == name) {
            return &f;
        }
    }
    return nullptr;
}

} // namespace

PreparedRewriteResult PreparedTemplateRewriter::rewrite(
    const std::string& template_text,
    const std::vector<RequestFieldConfig>& request_fields,
    const SqlParameterClassifier& classifier) const {

    PreparedRewriteResult result;
    result.rewritten_template.reserve(template_text.size());

    std::size_t cursor = 0;
    int section_depth = 0;

    while (cursor < template_text.size()) {
        const TagScan tag = nextTag(template_text, cursor);
        if (tag.kind == TagKind::NoTag) {
            appendRange(result.rewritten_template, template_text, cursor, template_text.size());
            break;
        }

        // Copy untouched text up to the tag.
        appendRange(result.rewritten_template, template_text, cursor, tag.start);

        switch (tag.kind) {
            case TagKind::OpenSection:
            case TagKind::OpenInvertedSection:
                ++section_depth;
                appendRange(result.rewritten_template, template_text, tag.start, tag.end);
                break;
            case TagKind::CloseSection:
                if (section_depth > 0) {
                    --section_depth;
                }
                appendRange(result.rewritten_template, template_text, tag.start, tag.end);
                break;
            case TagKind::TripleBrace:
                // Out of scope for PR A — operators migrate triple-brace
                // sites manually (drop surrounding quotes, switch to
                // double-brace) before they become bindable.
                appendRange(result.rewritten_template, template_text, tag.start, tag.end);
                break;
            case TagKind::DoubleBrace: {
                if (section_depth > 0) {
                    appendRange(result.rewritten_template, template_text, tag.start, tag.end);
                    break;
                }
                const std::string name = paramName(tag.inner);
                if (name.empty()) {
                    appendRange(result.rewritten_template, template_text, tag.start, tag.end);
                    break;
                }
                const RequestFieldConfig* field = findField(name, request_fields);
                if (field == nullptr) {
                    appendRange(result.rewritten_template, template_text, tag.start, tag.end);
                    break;
                }
                const auto bindability = classifier.classify(*field);
                if (!bindability.bindable) {
                    appendRange(result.rewritten_template, template_text, tag.start, tag.end);
                    break;
                }
                result.rewritten_template += '?';
                PreparedBindingSpec spec;
                spec.field_name = name;
                spec.type = bindability.type;
                spec.position = result.bindings.size();
                result.bindings.push_back(std::move(spec));
                break;
            }
            case TagKind::NoTag:
                break;  // unreachable; satisfied by the early break above
        }

        cursor = tag.end;
    }

    return result;
}

} // namespace flapi
