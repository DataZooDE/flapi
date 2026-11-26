#include "prepared_template_rewriter.hpp"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <regex>

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

// ---------------------------------------------------------------------------
// Binding sites (#192 for triple braces, #277 for double braces)
//
// A `params.x` tag of a typed field outside a Mustache section is bound
// instead of pasted, so the value can never change the statement. Both brace
// forms bind at the same sites:
//
//   n = {{ params.x }}            ->  n = ?
//   n = '{{ params.x }}'          ->  n = ?             (quotes dropped)
//   n LIKE '%{{ params.x }}%'     ->  n LIKE ('%' || ? || '%')
//
// Sites where a `?` is not a value stay literal: inside a section (the number
// of `?` would then depend on the data), a triple brace or a quoted literal
// inside `AT (VERSION|TIMESTAMP => ...)` (DuckLake cannot take a bound string
// there, #217; an unquoted `{{ params.x }}` there stays bound, which works for
// the int and date fields it is used with), after a keyword
// that makes the next token a name or a typed literal (`FROM`, `DATE '...'`,
// `INTERVAL '...'`, `ATTACH '...'`), glued to an identifier, in a comment, or
// for an `enum` field used unquoted (a whitelisted identifier such as
// `ORDER BY`). A double-brace enum that follows a comparison or `(` is still a
// value (`status = {{ params.k }}`, `IN ({{ params.k }})`), as it always was.
// A tag left literal is rendered by Mustache: `{{ }}` doubles single quotes
// (SQLTemplateProcessor), `{{{ }}}` pastes the value as it is.
// ---------------------------------------------------------------------------

struct Span {
    std::size_t open = 0;    // index of the first character (the opening quote / paren)
    std::size_t close = 0;   // index of the last character (the closing quote / paren)
};

bool isIdentChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '$';
}

// Skips one Mustache tag starting at `i` (which points at "{{"); returns the
// index one past it, or s.size() when it is unterminated.
std::size_t skipTag(const std::string& s, std::size_t i) {
    const bool triple = startsWith(s, i, "{{{");
    const std::size_t end = triple ? s.find("}}}", i + 3) : s.find("}}", i + 2);
    if (end == std::string::npos) {
        return s.size();
    }
    return end + (triple ? 3 : 2);
}

// The single-quoted SQL string literals of the template, tags treated as
// opaque. Comments and double-quoted identifiers are skipped. An unterminated
// literal ends the scan (nothing after it can be trusted).
std::vector<Span> findLiterals(const std::string& s, std::vector<Span>& comments) {
    std::vector<Span> out;
    std::size_t i = 0;
    while (i < s.size()) {
        const char c = s[i];
        if (startsWith(s, i, "{{")) {
            i = skipTag(s, i);
        } else if (startsWith(s, i, "--")) {
            const std::size_t nl = s.find('\n', i);
            comments.push_back({i, nl == std::string::npos ? s.size() - 1 : nl});
            i = nl == std::string::npos ? s.size() : nl + 1;
        } else if (startsWith(s, i, "/*")) {
            const std::size_t end = s.find("*/", i + 2);
            comments.push_back({i, end == std::string::npos ? s.size() - 1 : end + 1});
            i = end == std::string::npos ? s.size() : end + 2;
        } else if (c == '"') {
            const std::size_t end = s.find('"', i + 1);
            i = end == std::string::npos ? s.size() : end + 1;
        } else if (c == '\'') {
            std::size_t j = i + 1;
            bool closed = false;
            while (j < s.size()) {
                if (startsWith(s, j, "{{")) {
                    j = skipTag(s, j);
                } else if (s[j] == '\'') {
                    if (j + 1 < s.size() && s[j + 1] == '\'') {
                        j += 2;
                    } else {
                        closed = true;
                        break;
                    }
                } else {
                    ++j;
                }
            }
            if (!closed) {
                return out;
            }
            out.push_back({i, j});
            i = j + 1;
        } else {
            ++i;
        }
    }
    return out;
}

// `AT (VERSION => ...)` / `AT (TIMESTAMP => ...)` argument lists.
std::vector<Span> findTimeTravelRegions(const std::string& s, const std::vector<Span>& literals) {
    std::vector<Span> out;
    static const std::regex at_clause(R"(\bAT\s*\(\s*(VERSION|TIMESTAMP)\s*=>)", std::regex::icase);
    for (auto it = std::sregex_iterator(s.begin(), s.end(), at_clause); it != std::sregex_iterator(); ++it) {
        const std::size_t open = s.find('(', static_cast<std::size_t>(it->position()));
        int depth = 0;
        std::size_t j = open;
        std::size_t close = s.size() - 1;
        while (j < s.size()) {
            const auto lit = std::find_if(literals.begin(), literals.end(),
                                          [j](const Span& l) { return l.open == j; });
            if (lit != literals.end()) {
                j = lit->close + 1;
                continue;
            }
            if (startsWith(s, j, "{{")) {
                j = skipTag(s, j);
                continue;
            }
            if (s[j] == '(') {
                ++depth;
            } else if (s[j] == ')') {
                if (--depth == 0) {
                    close = j;
                    break;
                }
            }
            ++j;
        }
        out.push_back({open, close});
    }
    return out;
}

bool within(const std::vector<Span>& spans, std::size_t pos) {
    return std::any_of(spans.begin(), spans.end(),
                       [pos](const Span& sp) { return pos >= sp.open && pos <= sp.close; });
}

// The identifier-ish word that ends just before `pos` (whitespace skipped),
// lower-cased, and the character that ends there ('\0' at the start).
struct Preceding {
    std::string word;
    char last = '\0';
};

Preceding precedingToken(const std::string& s, std::size_t pos) {
    Preceding out;
    std::size_t k = pos;
    while (k > 0 && std::isspace(static_cast<unsigned char>(s[k - 1])) != 0) {
        --k;
    }
    if (k == 0) {
        return out;
    }
    out.last = s[k - 1];
    if (isIdentChar(out.last)) {
        std::size_t start = k;
        while (start > 0 && isIdentChar(s[start - 1])) {
            --start;
        }
        out.word = s.substr(start, k - start);
        std::transform(out.word.begin(), out.word.end(), out.word.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    }
    return out;
}

bool oneOf(const std::string& word, std::initializer_list<const char*> list) {
    return std::any_of(list.begin(), list.end(), [&word](const char* w) { return word == w; });
}

// A quoted literal is a plain string value (so a bound string can replace it)
// unless a word glued to it makes it something else: DATE '..', INTERVAL '..',
// E'..', ATTACH '..', FROM '..'. Operators, `(`, `,` and the value-taking
// keywords below leave it a value.
bool literalIsAValue(const std::string& s, const Span& lit) {
    const Preceding before = precedingToken(s, lit.open);
    if (before.last == '\0') {
        return true;
    }
    if (isIdentChar(before.last)) {
        return oneOf(before.word, {"like", "ilike", "glob", "and", "or", "not", "then", "else", "when",
                                   "between", "escape", "select"});
    }
    return !(before.last == '\'' || before.last == '"' || before.last == ')' || before.last == ']' ||
             before.last == '}');
}

// An unquoted `{{{ params.x }}}` is a value (so a `?` can replace it) unless it
// is part of a name or follows a keyword that expects one.
bool unquotedIsAValue(const std::string& s, const TagScan& tag) {
    const auto glued = [](char c) {
        return isIdentChar(c) || c == '.' || c == '"' || c == '`' || c == '\'';
    };
    if (tag.start > 0 && glued(s[tag.start - 1])) {
        return false;
    }
    if (tag.end < s.size() && glued(s[tag.end])) {
        return false;
    }
    const Preceding before = precedingToken(s, tag.start);
    if (isIdentChar(before.last)) {
        return !oneOf(before.word, {"from", "join", "into", "table", "update", "by", "as", "using", "attach",
                                    "detach", "database", "schema", "view", "index", "set", "pragma", "call",
                                    "on", "exists", "type", "sequence"});
    }
    return true;
}

bool hasValidatorType(const RequestFieldConfig& field, const char* type) {
    return std::any_of(field.validators.begin(), field.validators.end(),
                       [type](const ValidatorConfig& v) { return v.type == type; });
}

// An unquoted enum is a whitelisted name unless it plainly sits on the right of
// a comparison or opens a list: `= {{ k }}`, `<> {{ k }}`, `IN ({{ k }})`.
// Triple braces never had this exemption (an unquoted enum there is a name).
bool enumIsAValue(const std::string& s, const TagScan& tag) {
    if (tag.kind != TagKind::DoubleBrace) {
        return false;
    }
    const Preceding before = precedingToken(s, tag.start);
    return before.last == '=' || before.last == '<' || before.last == '>' || before.last == '(';
}

} // namespace

PreparedRewriteResult PreparedTemplateRewriter::rewrite(
    const std::string& template_text,
    const std::vector<RequestFieldConfig>& request_fields,
    const SqlParameterClassifier& classifier) const {

    PreparedRewriteResult result;
    result.rewritten_template.reserve(template_text.size());

    std::vector<Span> comments;
    const std::vector<Span> literals = findLiterals(template_text, comments);
    const std::vector<Span> time_travel = findTimeTravelRegions(template_text, literals);
    std::size_t next_literal = 0;

    // The `?` (or `('%' || ? || '%')`) a quoted literal becomes, when every tag
    // in it is a bindable `{{{ params.x }}}`. Values are bound as text, as the
    // quoted form always was, and an absent one binds '' as it used to render.
    const auto bindLiteral = [&](const Span& lit, int depth, std::string& replacement,
                                 std::vector<PreparedBindingSpec>& specs) -> bool {
        if (depth > 0 || within(time_travel, lit.open) || !literalIsAValue(template_text, lit)) {
            return false;
        }
        std::vector<std::string> parts;
        std::size_t at = lit.open + 1;
        bool any_tag = false;
        while (at < lit.close) {
            const std::size_t brace = template_text.find("{{", at);
            if (brace == std::string::npos || brace >= lit.close) {
                parts.push_back("'" + template_text.substr(at, lit.close - at) + "'");
                break;
            }
            const TagScan inner = nextTag(template_text, brace);
            if ((inner.kind != TagKind::TripleBrace && inner.kind != TagKind::DoubleBrace) ||
                inner.end > lit.close) {
                return false;
            }
            const std::string name = paramName(inner.inner);
            const RequestFieldConfig* field = name.empty() ? nullptr : findField(name, request_fields);
            if (field == nullptr || !classifier.classify(*field).bindable) {
                return false;
            }
            if (brace > at) {
                parts.push_back("'" + template_text.substr(at, brace - at) + "'");
            }
            parts.push_back("?");
            PreparedBindingSpec spec;
            spec.field_name = name;
            spec.type = SqlParameterType::Varchar;
            spec.absent_as_empty = true;
            specs.push_back(std::move(spec));
            any_tag = true;
            at = inner.end;
        }
        if (!any_tag) {
            return false;
        }
        if (parts.size() == 1) {
            replacement = parts.front();
        } else {
            replacement = "(";
            for (std::size_t i = 0; i < parts.size(); ++i) {
                replacement += (i == 0 ? "" : " || ") + parts[i];
            }
            replacement += ")";
        }
        return true;
    };

    std::size_t cursor = 0;
    int section_depth = 0;

    while (cursor < template_text.size()) {
        while (next_literal < literals.size() && literals[next_literal].open < cursor) {
            ++next_literal;
        }
        const TagScan tag = nextTag(template_text, cursor);
        if (next_literal < literals.size() &&
            (tag.kind == TagKind::NoTag || literals[next_literal].open < tag.start)) {
            const Span lit = literals[next_literal++];
            appendRange(result.rewritten_template, template_text, cursor, lit.open);
            cursor = lit.open;
            std::string replacement;
            std::vector<PreparedBindingSpec> specs;
            if (bindLiteral(lit, section_depth, replacement, specs)) {
                result.rewritten_template += replacement;
                for (auto& spec : specs) {
                    spec.position = result.bindings.size();
                    result.bindings.push_back(std::move(spec));
                }
                cursor = lit.close + 1;
            } else {
                // Left as written: tags inside it take the ordinary route below.
                // Step over the opening quote so this literal is not offered again.
                result.rewritten_template += '\'';
                cursor = lit.open + 1;
            }
            continue;
        }
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
            case TagKind::DoubleBrace: {
                // An unquoted value site binds. A tag in a literal that was not
                // bound as a whole stays text (the literal may be a typed literal
                // such as DATE '...'); so does every site that is not a value.
                const std::string name = paramName(tag.inner);
                const RequestFieldConfig* field = name.empty() ? nullptr : findField(name, request_fields);
                // A `?` in a comment is not a placeholder, so it must not enter the plan.
                const Bindability bindability = field != nullptr ? classifier.classify(*field) : Bindability{};
                if (section_depth > 0 || field == nullptr || !bindability.bindable ||
                    (hasValidatorType(*field, "enum") && !enumIsAValue(template_text, tag)) ||
                    within(literals, tag.start) || within(comments, tag.start) ||
                    (tag.kind == TagKind::TripleBrace && within(time_travel, tag.start)) ||
                    !unquotedIsAValue(template_text, tag)) {
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
