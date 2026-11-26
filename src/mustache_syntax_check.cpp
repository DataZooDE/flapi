#include "mustache_syntax_check.hpp"

#include "path_validator.hpp"
#include "vfs_adapter.hpp"

#include <crow/mustache.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <random>
#include <set>
#include <stdexcept>
#include <string_view>
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

namespace {

// Where a position of SQL text sits, as DuckDB's lexer would see it.
enum class SqlRegion { Code, Literal, EscapeLiteral, Identifier, LineComment, BlockComment, DollarString };

const char* DescribeRegion(SqlRegion region) {
    switch (region) {
        case SqlRegion::Code:
            return "outside a quoted SQL literal";
        case SqlRegion::Literal:
            return "inside a '...' literal";
        case SqlRegion::EscapeLiteral:
            return "inside an E'...' escape string, where a backslash escapes the quote";
        case SqlRegion::Identifier:
            return "inside a \"...\" identifier";
        case SqlRegion::LineComment:
            return "inside a -- comment";
        case SqlRegion::BlockComment:
            return "inside a /* */ comment";
        case SqlRegion::DollarString:
            return "inside a dollar-quoted string";
    }
    return "outside a quoted SQL literal";
}

bool IsIdentifierStart(char c) {
    const auto u = static_cast<unsigned char>(c);
    return std::isalpha(u) != 0 || c == '_' || u >= 0x80;
}

bool IsIdentifierChar(char c) {
    return IsIdentifierStart(c) || std::isdigit(static_cast<unsigned char>(c)) != 0 || c == '$';
}

bool IsDollarTagChar(char c) {
    return IsIdentifierStart(c) || std::isdigit(static_cast<unsigned char>(c)) != 0;
}

// True when a marker starts at `at`: a token scan must stop there, so that a
// marker glued to an identifier or a dollar tag is still seen on its own.
bool AtBarrier(const std::string& body, std::size_t at, const std::string* barrier) {
    return barrier != nullptr && !barrier->empty() && body.compare(at, barrier->size(), *barrier) == 0;
}

// The length of a `$$` or `$tag$` opener at `i`, or 0. DuckDB accepts the same
// characters in a tag as in an identifier, bytes >= 0x80 included.
std::size_t DollarTagAt(const std::string& body, std::size_t i, const std::string* barrier) {
    if (body[i] != '$') {
        return 0;
    }
    std::size_t j = i + 1;
    if (j < body.size() && IsIdentifierStart(body[j]) && !AtBarrier(body, j, barrier)) {
        while (j < body.size() && IsDollarTagChar(body[j]) && !AtBarrier(body, j, barrier)) {
            ++j;
        }
    }
    return j < body.size() && body[j] == '$' ? j - i + 1 : 0;
}

// Table and scalar functions that parse a string argument as SQL and run it.
// A literal anywhere inside one of their calls is SQL text, where a doubled
// quote is a plain quote again once that function parses it.
bool IsSqlExecutingFunction(const std::string& lower) {
    static const std::set<std::string> kExact = {
        "query",          "query_table",     "odbc_query",      "bigquery_query",  "bigquery_execute",
        "postgres_query", "postgres_execute", "mysql_query",    "mysql_execute",   "snowflake_query",
        "sqlite_query",   "duckdb_query",    "json_execute_serialized_sql"};
    if (kExact.count(lower) != 0) {
        return true;
    }
    const auto ends_with = [&lower](std::string_view suffix) {
        return lower.size() > suffix.size() && lower.compare(lower.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    return ends_with("_query") || ends_with("_execute");
}

// A parenthesis after one of these is grouping, a subquery or a list, not a
// call whose arguments a function could run.
bool IsGroupingKeyword(const std::string& lower) {
    static const std::set<std::string> kKeywords = {
        "select", "from", "where", "in", "and", "or", "not", "on", "join", "values", "exists",
        "any", "all", "as", "by", "over", "using", "filter", "within", "when", "then", "else",
        "case", "having", "with", "union", "intersect", "except", "lateral", "returning", "into",
        "set", "like", "ilike", "between", "is", "limit", "offset", "order", "group"};
    return kKeywords.count(lower) != 0;
}

struct SqlCall {
    std::string name;             // lower case; "" for a bare parenthesis
    bool executes_sql = false;    // a known SQL-executing function
    bool is_call = false;         // a function call, not grouping
    bool tainted = false;         // an argument literal holds SQL quoting
    std::vector<MustacheVariable> pending;   // auth values in its argument literals
};

struct SqlScan {
    SqlRegion region = SqlRegion::Code;
    int block_depth = 0;          // DuckDB nests /* */ comments
    std::string dollar_tag;       // "$$" or "$tag$" while in a dollar-quoted string
    std::vector<SqlCall> calls;   // open parentheses, outermost first
    std::string last_name;        // the name a following '(' calls, across whitespace and comments
    std::string identifier;       // the "..." identifier being scanned
    // For the literal being scanned:
    bool literal_doubled = false;     // holds a ''
    bool literal_sql_quoting = false; // holds a $$ / $tag$ or a "
    std::string literal_executor;     // an enclosing SQL-executing call, if any

    bool SameQuoting(const SqlScan& other) const {
        return region == other.region && block_depth == other.block_depth && dollar_tag == other.dollar_tag;
    }
};

enum class SqlEvent { None, LiteralClosed, CallClosed };

void StartLiteral(SqlScan& scan, SqlRegion region) {
    scan.region = region;
    scan.literal_doubled = false;
    scan.literal_sql_quoting = false;
    scan.literal_executor.clear();
    for (auto it = scan.calls.rbegin(); it != scan.calls.rend(); ++it) {
        if (it->executes_sql) {
            scan.literal_executor = it->name;
            break;
        }
    }
    scan.last_name.clear();
}

std::string Lower(std::string text) {
    for (auto& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

// Advances `i` past one character or token of SQL text, as DuckDB's lexer
// tokenises it. `barrier` (a marker prefix, or null) stops a token scan.
SqlEvent AdvanceSql(const std::string& body, std::size_t& i, SqlScan& scan, const std::string* barrier) {
    const char c = body[i];
    const char next = i + 1 < body.size() ? body[i + 1] : '\0';
    const auto u = static_cast<unsigned char>(c);
    switch (scan.region) {
        case SqlRegion::Code:
            if (c == '\'') {
                StartLiteral(scan, SqlRegion::Literal);
                ++i;
            } else if (c == '"') {
                scan.region = SqlRegion::Identifier;
                scan.identifier.clear();
                ++i;
            } else if (c == '-' && next == '-') {
                scan.region = SqlRegion::LineComment;   // keeps last_name: `query -- x\n(`
                i += 2;
            } else if (c == '/' && next == '*') {
                scan.region = SqlRegion::BlockComment;
                scan.block_depth = 1;
                i += 2;
            } else if (c == '(') {
                SqlCall call;
                call.name = std::move(scan.last_name);
                call.executes_sql = IsSqlExecutingFunction(call.name);
                call.is_call = !call.name.empty() && !IsGroupingKeyword(call.name);
                scan.calls.push_back(std::move(call));
                scan.last_name.clear();
                ++i;
            } else if (c == ')') {
                scan.last_name.clear();
                ++i;
                return SqlEvent::CallClosed;
            } else if (c == '$') {
                if (const std::size_t tag = DollarTagAt(body, i, barrier)) {
                    scan.region = SqlRegion::DollarString;
                    scan.dollar_tag = body.substr(i, tag);
                    i += tag;
                } else {
                    ++i;   // $1, a positional parameter
                    while (i < body.size() && std::isdigit(static_cast<unsigned char>(body[i])) != 0) {
                        ++i;
                    }
                }
                scan.last_name.clear();
            } else if (IsIdentifierStart(c)) {
                std::size_t end = i;
                while (end < body.size() && IsIdentifierChar(body[end]) && !AtBarrier(body, end, barrier)) {
                    ++end;
                }
                if (end == i + 1 && (c == 'E' || c == 'e') && end < body.size() && body[end] == '\'') {
                    // E'...': a backslash escapes the quote here.
                    StartLiteral(scan, SqlRegion::EscapeLiteral);
                    i = end + 1;
                } else {
                    scan.last_name = Lower(body.substr(i, end - i));
                    i = end;
                }
            } else if (std::isdigit(u) != 0) {
                // A number. What follows it starts a new token, so 1E'...' is an
                // E'...' string and 1$$...$$ a dollar-quoted one, as in DuckDB.
                std::size_t end = i;
                while (end < body.size() && !AtBarrier(body, end, barrier)) {
                    const char d = body[end];
                    if (std::isdigit(static_cast<unsigned char>(d)) != 0 || d == '.' || d == '_') {
                        ++end;
                    } else if ((d == 'e' || d == 'E') && end + 1 < body.size() &&
                               (std::isdigit(static_cast<unsigned char>(body[end + 1])) != 0 ||
                                ((body[end + 1] == '+' || body[end + 1] == '-') && end + 2 < body.size() &&
                                 std::isdigit(static_cast<unsigned char>(body[end + 2])) != 0))) {
                        end += 2;
                    } else {
                        break;
                    }
                }
                scan.last_name.clear();
                i = end;
            } else {
                if (std::isspace(u) == 0) {
                    scan.last_name.clear();   // `.`, an operator, ...: no longer a call target
                }
                ++i;
            }
            return SqlEvent::None;
        case SqlRegion::EscapeLiteral:
            if (c == '\\') {
                i += 2;
                return SqlEvent::None;
            }
            [[fallthrough]];
        case SqlRegion::Literal:
            if (c == '\'' && next == '\'') {
                scan.literal_doubled = true;
                i += 2;
                return SqlEvent::None;
            }
            if (c == '\'') {
                scan.region = SqlRegion::Code;
                ++i;
                return SqlEvent::LiteralClosed;
            }
            if (c == '"' || (c == '$' && DollarTagAt(body, i, barrier) != 0)) {
                scan.literal_sql_quoting = true;
            }
            ++i;
            return SqlEvent::None;
        case SqlRegion::Identifier:
            if (c == '"' && next == '"') {
                scan.identifier += '"';
                i += 2;
            } else if (c == '"') {
                scan.region = SqlRegion::Code;
                scan.last_name = Lower(scan.identifier);   // "query"(...) calls query
                ++i;
            } else {
                scan.identifier += c;
                ++i;
            }
            return SqlEvent::None;
        case SqlRegion::LineComment:
            // DuckDB ends a -- comment at a carriage return too.
            if (c == '\n' || c == '\r') {
                scan.region = SqlRegion::Code;
            }
            ++i;
            return SqlEvent::None;
        case SqlRegion::BlockComment:
            if (c == '/' && next == '*') {
                ++scan.block_depth;
                i += 2;
            } else if (c == '*' && next == '/') {
                i += 2;
                if (--scan.block_depth == 0) {
                    scan.region = SqlRegion::Code;
                }
            } else {
                ++i;
            }
            return SqlEvent::None;
        case SqlRegion::DollarString:
            if (body.compare(i, scan.dollar_tag.size(), scan.dollar_tag) == 0) {
                i += scan.dollar_tag.size();
                scan.region = SqlRegion::Code;
                scan.dollar_tag.clear();
            } else {
                ++i;
            }
            return SqlEvent::None;
    }
    return SqlEvent::None;
}

constexpr const char* kExecutedText =
    "auth.* is not allowed inside SQL text that another function executes; filter in the outer query instead";

// Collects auth values found while scanning SQL text and decides, when their
// literal and every call around it close, whether the literal is a plain
// top-level one. A literal is SQL text when it holds SQL quoting itself (''
// or $$ or "), when it sits in a known SQL-executing call, or when another
// argument literal of an enclosing call holds SQL quoting (a macro handed
// 'SELECT ... $q$' || '<value>' || '$q$').
class AuthPlacementCollector {
public:
    void Value(const std::string& name, std::size_t offset, const SqlScan& scan) {
        MustacheVariable value{name, 1, 1, "", offset};
        if (scan.region == SqlRegion::Literal) {
            _literal_pending.push_back(std::move(value));
        } else {
            value.detail = DescribeRegion(scan.region);
            _problems.push_back(std::move(value));
        }
    }

    void Problem(const std::string& name, std::size_t offset, const std::string& detail) {
        _problems.push_back(MustacheVariable{name, 1, 1, detail, offset});
    }

    void Handle(SqlEvent event, SqlScan& scan) {
        if (event == SqlEvent::LiteralClosed) {
            LiteralClosed(scan);
        } else if (event == SqlEvent::CallClosed) {
            CallClosed(scan);
        }
    }

    std::vector<MustacheVariable> Finish(const std::string& body, SqlScan& scan) {
        if (scan.region == SqlRegion::Literal || scan.region == SqlRegion::EscapeLiteral) {
            Refuse(_literal_pending, "inside a '...' literal that is never closed");
        }
        _literal_pending.clear();
        for (auto& call : scan.calls) {
            Refuse(call.pending, "inside a call that is never closed");
        }
        FillPositions(body, _problems);
        return std::move(_problems);
    }

    // Line and column are computed for the problems only, in one pass: per
    // value they made a large template quadratic.
    static void FillPositions(const std::string& body, std::vector<MustacheVariable>& problems) {
        std::sort(problems.begin(), problems.end(),
                  [](const MustacheVariable& a, const MustacheVariable& b) { return a.offset < b.offset; });
        std::size_t line = 1;
        std::size_t column = 1;
        std::size_t at = 0;
        for (auto& problem : problems) {
            for (; at < problem.offset && at < body.size(); ++at) {
                if (body[at] == '\n') {
                    ++line;
                    column = 1;
                } else {
                    ++column;
                }
            }
            problem.line = line;
            problem.column = column;
        }
    }

private:
    static SqlCall* NearestCall(SqlScan& scan) {
        for (auto it = scan.calls.rbegin(); it != scan.calls.rend(); ++it) {
            if (it->is_call) {
                return &*it;
            }
        }
        return nullptr;
    }

    void Refuse(std::vector<MustacheVariable>& values, const std::string& detail) {
        for (auto& value : values) {
            value.detail = detail;
            _problems.push_back(std::move(value));
        }
        values.clear();
    }

    void LiteralClosed(SqlScan& scan) {
        const bool sql_text = scan.literal_doubled || scan.literal_sql_quoting;
        if (sql_text) {
            for (auto& call : scan.calls) {
                call.tainted = call.tainted || call.is_call;
            }
        }
        if (_literal_pending.empty()) {
            return;
        }
        if (!scan.literal_executor.empty()) {
            Refuse(_literal_pending, "inside SQL text that another function executes (" + scan.literal_executor +
                                         "(...)). " + kExecutedText);
        } else if (sql_text) {
            Refuse(_literal_pending,
                   "inside a '...' literal that also holds SQL quoting ('', $$ or \"), i.e. SQL text. " +
                       std::string(kExecutedText));
        } else if (SqlCall* call = NearestCall(scan)) {
            for (auto& value : _literal_pending) {
                call->pending.push_back(std::move(value));
            }
        }
        _literal_pending.clear();   // top level: a plain literal of the outer query
    }

    void CallClosed(SqlScan& scan) {
        if (scan.calls.empty()) {
            return;
        }
        SqlCall call = std::move(scan.calls.back());
        scan.calls.pop_back();
        if (call.pending.empty()) {
            return;
        }
        if (call.tainted) {
            Refuse(call.pending, "inside a call to " + call.name +
                                     "(...) whose arguments also hold SQL quoting ('', $$ or \"), i.e. SQL text. " +
                                     kExecutedText);
        } else if (SqlCall* parent = NearestCall(scan)) {
            for (auto& value : call.pending) {
                parent->pending.push_back(std::move(value));
            }
        }
    }

    std::vector<MustacheVariable> _literal_pending;
    std::vector<MustacheVariable> _problems;
};

struct OpenAuthScanSection {
    std::string resolved;   // the name as the context resolves it, e.g. "auth.username"
    bool inverted;
    SqlScan at_open;
    std::size_t offset;
};

// The name a tag looks up, as crow resolves it against the section stack:
// `.` is the innermost section's value, and inside `{{#auth}}` a bare
// `username` is auth.username.
std::string ResolveTagName(const std::string& name, const std::vector<OpenAuthScanSection>& sections) {
    if (name == ".") {
        return sections.empty() ? name : sections.back().resolved;
    }
    if (name == "auth" || name.rfind("auth.", 0) == 0) {
        return name;
    }
    const std::string first = name.substr(0, name.find('.'));
    static const std::vector<std::string> kAuthFields = {"username", "email", "roles", "type", "authenticated"};
    for (auto it = sections.rbegin(); it != sections.rend(); ++it) {
        if (!it->inverted && it->resolved == "auth") {
            for (const auto& field : kAuthFields) {
                if (first == field) {
                    return "auth." + name;
                }
            }
            break;
        }
    }
    return name;
}

// auth.authenticated is "true" or empty, set by the server: safe anywhere.
bool IsIdentityValue(const std::string& resolved) {
    if (resolved == "auth") {
        return true;
    }
    if (resolved.rfind("auth.", 0) != 0) {
        return false;
    }
    return resolved != "auth.authenticated" && resolved.rfind("auth.authenticated.", 0) != 0;
}

} // namespace

std::vector<MustacheVariable> FindUnquotedAuthVariables(const std::string& body) {
    // A lint over the template SOURCE, for load and --validate-config: it
    // also sees tags in sections that a sample render would skip. The render
    // itself is checked on the rendered SQL (FindUnsafeAuthMarkers), which a
    // Mustache tag between two SQL characters cannot fool.
    AuthPlacementCollector collector;
    std::vector<MustacheVariable> quoting_changes;
    std::vector<OpenAuthScanSection> sections;
    bool mentions_auth = false;
    SqlScan scan;
    std::size_t i = 0;
    while (i < body.size()) {
        if (body.compare(i, kDelimiterSize, kOpen) != 0) {
            collector.Handle(AdvanceSql(body, i, scan, nullptr), scan);
            continue;
        }
        const std::size_t idx = i + kDelimiterSize;
        const std::size_t close = body.find(kClose, idx);
        if (close == std::string::npos || close == idx) {
            break;   // malformed: CheckMustacheSyntax reports it
        }
        std::size_t next = close + kDelimiterSize;
        const char kind = body[idx];
        if (kind == '=') {
            // A new delimiter hides every following tag from this scan
            // (CheckMustacheSyntax refuses it anyway).
            if (body.find("auth", i) != std::string::npos) {
                collector.Value("auth.*", i, SqlScan{});
            }
            break;
        }
        if (kind == '!' || kind == '>') {
            i = next;
            continue;
        }
        if (kind == '#' || kind == '^') {
            const std::string resolved = ResolveTagName(TrimSpaces(body, idx + 1, close), sections);
            mentions_auth = mentions_auth || resolved == "auth" || resolved.rfind("auth.", 0) == 0;
            sections.push_back(OpenAuthScanSection{resolved, kind == '^', scan, i});
            i = next;
            continue;
        }
        if (kind == '/') {
            if (!sections.empty()) {
                // A section that opens and closes in different regions makes
                // the quoting depend on whether it renders.
                if (!sections.back().at_open.SameQuoting(scan)) {
                    quoting_changes.push_back(MustacheVariable{
                        "auth.*", 1, 1,
                        "in a template whose section '" + sections.back().resolved +
                            "' opens and closes in different SQL quoting, so it cannot be checked",
                        sections.back().offset});
                }
                sections.pop_back();
            }
            i = next;
            continue;
        }
        std::size_t name_begin = idx;
        if (kind == '&') {
            name_begin = idx + 1;
        } else if (kind == '{') {
            name_begin = idx + 1;
            if (next < body.size() && body[next] == '}') {
                ++next;
            }
        }
        const std::string resolved = ResolveTagName(TrimSpaces(body, name_begin, close), sections);
        if (IsIdentityValue(resolved)) {
            mentions_auth = true;
            collector.Value(resolved, i, scan);
        }
        i = next;
    }
    auto out = collector.Finish(body, scan);
    if (out.empty() && mentions_auth && !quoting_changes.empty()) {
        AuthPlacementCollector::FillPositions(body, quoting_changes);
        out = std::move(quoting_changes);
    }
    return out;
}

std::string NewAuthMarkerPrefix() {
    // Letters and digits only, so neither Mustache's HTML escaping nor the SQL
    // lexer treats any of it specially; 128 bits per render from a generator
    // seeded with 256 bits of std::random_device, so no template or request
    // text can contain it by accident.
    thread_local std::mt19937_64 generator = [] {
        std::random_device device;
        std::seed_seq seed{device(), device(), device(), device(), device(), device(), device(), device()};
        return std::mt19937_64(seed);
    }();
    static constexpr char kHex[] = "0123456789abcdef";
    std::string prefix = "flapiauth";
    for (int part = 0; part < 2; ++part) {
        std::uint64_t bits = generator();
        for (int nibble = 0; nibble < 16; ++nibble) {
            prefix += kHex[bits & 0xF];
            bits >>= 4;
        }
    }
    prefix += 'q';
    return prefix;
}

std::vector<MustacheVariable> FindUnsafeAuthMarkers(const std::string& sql, const std::string& prefix,
                                                    const std::vector<AuthMarker>& markers) {
    AuthPlacementCollector collector;
    SqlScan scan;
    std::size_t seen = 0;
    std::size_t i = 0;
    while (i < sql.size()) {
        if (!prefix.empty() && sql[i] == prefix[0] && sql.compare(i, prefix.size(), prefix) == 0) {
            const AuthMarker* found = nullptr;
            for (const auto& marker : markers) {
                if (sql.compare(i, marker.marker.size(), marker.marker) == 0) {
                    found = &marker;
                    break;
                }
            }
            collector.Value(found != nullptr ? found->name : std::string("auth.*"), i, scan);
            ++seen;
            i += found != nullptr ? found->marker.size() : prefix.size();
            continue;
        }
        collector.Handle(AdvanceSql(sql, i, scan, &prefix), scan);
    }
    // Every marker in the SQL must have been seen by the scan above; one the
    // lexer stepped over could not be judged, so it is refused.
    std::size_t occurrences = 0;
    for (std::size_t at = prefix.empty() ? std::string::npos : sql.find(prefix); at != std::string::npos;
         at = sql.find(prefix, at + prefix.size())) {
        ++occurrences;
    }
    if (occurrences != seen) {
        collector.Problem("auth.*", 0, "in a position the SQL check could not follow");
    }
    return collector.Finish(sql, scan);
}

std::string SubstituteAuthMarkers(const std::string& sql, const std::string& prefix,
                                  const std::vector<AuthMarker>& markers) {
    // One pass: a substituted value is never scanned again.
    std::string out;
    out.reserve(sql.size());
    std::size_t i = 0;
    while (i < sql.size()) {
        const std::size_t at = sql.find(prefix, i);
        if (at == std::string::npos) {
            out.append(sql, i, std::string::npos);
            break;
        }
        out.append(sql, i, at - i);
        const AuthMarker* found = nullptr;
        for (const auto& marker : markers) {
            if (sql.compare(at, marker.marker.size(), marker.marker) == 0) {
                found = &marker;
                break;
            }
        }
        if (found != nullptr) {
            out += found->value;
            i = at + found->marker.size();
        } else {
            out.append(sql, at, prefix.size());
            i = at + prefix.size();
        }
    }
    return out;
}

std::string FormatAuthPlacementProblem(const MustacheVariable& problem, const std::string& label) {
    std::string where = label.empty() ? std::string("template") : label;
    return "Unsafe use of auth.* in " + where + ":" + std::to_string(problem.line) + ":" +
           std::to_string(problem.column) + ": '" + problem.name + "' is " + problem.detail +
           ". auth.* values are SQL-quoted for a plain top-level '...' literal of the outer query only "
           "(not E'...', a comment, a \"...\" identifier, a $$ string or SQL text another function runs): "
           "write '{{{ auth.username }}}'";
}

std::optional<std::string> CheckAuthPlacement(const std::string& body, const std::string& label) {
    if (CheckMustacheSyntax(body)) {
        return std::nullopt;   // reported by the syntax check
    }
    auto problems = FindUnquotedAuthVariables(body);
    std::string where = label;
    if (problems.empty()) {
        // Render a sample - every auth.* value a marker, nothing else set - and
        // check where the markers land in the SQL. This sees what the source
        // scan cannot: E{{! x }}'...', a section that renders a quote, ...
        try {
            const std::string prefix = NewAuthMarkerPrefix();
            const std::vector<AuthMarker> markers = {{prefix + "az", "auth.username", ""},
                                                     {prefix + "bz", "auth.roles", ""},
                                                     {prefix + "cz", "auth.email", ""},
                                                     {prefix + "dz", "auth.type", ""}};
            crow::mustache::context ctx;
            ctx["auth"]["username"] = markers[0].marker;
            ctx["auth"]["roles"] = markers[1].marker;
            ctx["auth"]["email"] = markers[2].marker;
            ctx["auth"]["type"] = markers[3].marker;
            ctx["auth"]["authenticated"] = "true";
            const std::string sql = crow::mustache::compile(body).render(ctx).dump();
            problems = FindUnsafeAuthMarkers(sql, prefix, markers);
            where = "the rendered SQL of " + (label.empty() ? std::string("the template") : label);
        } catch (const std::exception&) {
            // Not renderable without request values: the source scan stands.
        }
    }
    if (problems.empty()) {
        return std::nullopt;
    }
    std::string message = FormatAuthPlacementProblem(problems.front(), where);
    if (problems.size() > 1) {
        message += " (and " + std::to_string(problems.size() - 1) + " more)";
    }
    return message;
}

std::optional<std::string> CheckAuthPlacementInTemplateFile(const std::string& path) {
    const auto content = ReadTemplateText(path);
    if (!content) {
        return std::nullopt;   // missing: reported by the other checks
    }
    return CheckAuthPlacement(*content, path);
}

std::vector<MustacheVariable> CollectMustacheVariables(const std::string& body) {
    std::vector<MustacheVariable> out;
    std::size_t current = 0;
    std::size_t depth = 0;
    while (true) {
        const std::size_t tag_start = body.find(kOpen, current);
        if (tag_start == std::string::npos) {
            break;
        }
        const std::size_t idx = tag_start + kDelimiterSize;
        const std::size_t close = body.find(kClose, idx);
        if (close == std::string::npos || close == idx) {
            break;
        }
        current = close + kDelimiterSize;

        std::size_t name_begin = idx;
        std::size_t tag_depth = depth;
        switch (body[idx]) {
            case '#':
            case '^':
                name_begin = idx + 1;
                ++depth;   // the section's own name is looked up outside it
                break;
            case '&':
                name_begin = idx + 1;
                break;
            case '{':
                name_begin = idx + 1;
                current++;   // the third brace
                break;
            case '/':
                if (depth > 0) {
                    --depth;
                }
                continue;
            case '!':
            case '>':
            case '=':
                continue;
            default:
                break;
        }
        std::string name = TrimSpaces(body, name_begin, close);
        if (name.empty()) {
            continue;
        }
        const auto at = ErrorAt(body, tag_start, "");
        MustacheVariable variable{std::move(name), at.line, at.column};
        variable.section_depth = tag_depth;
        out.push_back(std::move(variable));
    }
    return out;
}

std::vector<MustacheVariable> FindUndefinedTemplateVariables(const std::string& body,
                                                             const crow::mustache::context& ctx,
                                                             const std::set<std::string>& declared_params) {
    static const std::set<std::string> kRoots = {"params", "conn", "env", "auth", "cache"};
    static const std::set<std::string> kAuthFields = {"username", "roles", "email", "type", "authenticated"};
    std::vector<MustacheVariable> out;
    std::set<std::string> reported;
    for (const auto& variable : CollectMustacheVariables(body)) {
        const std::string& name = variable.name;
        if (name == "." || reported.count(name) != 0) {
            continue;
        }
        std::vector<std::string> parts;
        for (std::size_t start = 0;;) {
            const std::size_t dot = name.find('.', start);
            parts.push_back(name.substr(start, dot == std::string::npos ? std::string::npos : dot - start));
            if (dot == std::string::npos) {
                break;
            }
            start = dot + 1;
        }
        if (kRoots.count(parts.front()) == 0 && variable.section_depth > 0) {
            continue;   // resolved against the enclosing section's value first
        }
        const crow::mustache::context* at = &ctx;
        bool found = true;
        for (const auto& part : parts) {
            if (at->t() != crow::json::type::Object || at->count(part) == 0) {
                found = false;
                break;
            }
            at = &(*at)[part];
        }
        if (found) {
            continue;
        }
        if (parts.size() >= 2 && parts[0] == "params" && declared_params.count(parts[1]) != 0) {
            continue;   // a declared parameter the request did not send: absent, as it may be
        }
        if (parts.size() == 2 && parts[0] == "auth" && kAuthFields.count(parts[1]) != 0) {
            continue;   // set on an authenticated request; the config service has no caller
        }
        reported.insert(name);
        out.push_back(variable);
    }
    return out;
}

std::optional<std::string> ReadTemplateText(const std::string& path) {
    if (path.empty()) {
        return std::nullopt;
    }
    try {
        auto provider = FileProviderFactory::CreateProvider(path);
        if (!provider->FileExists(path)) {
            return std::nullopt;
        }
        return provider->ReadFile(path);
    } catch (const std::exception&) {
        // Unreadable here is reported by the existence checks, not as syntax.
        return std::nullopt;
    }
}

std::optional<std::string> CheckMustacheTemplateFile(const std::string& path) {
    const auto content = ReadTemplateText(path);
    if (!content) {
        return std::nullopt;
    }
    if (const auto error = CheckMustacheSyntax(*content)) {
        return "Invalid template " + FormatMustacheSyntaxError(*error, path);
    }
    return std::nullopt;
}

} // namespace flapi
