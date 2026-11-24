#include "redaction.hpp"

#include "config_manager.hpp"
#include "vfs_adapter.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <vector>

namespace flapi {

namespace {

// Substring stems over the normalised key. Every entry is long enough that it
// cannot appear inside an ordinary data-API field name:
//   - "authorization", not "auth"  -> `author` is not a credential
//   - "signature",     not "sig"   -> `design` is not a credential
//   - "apikey",        not "key"   -> `sort_key`, `primary_key` survive
//
// Short stems (pin, sid, sig, otp) are deliberately absent: matched as
// substrings they would redact half the ordinary fields in a data API, and a
// denylist that redacts everything teaches operators to turn it off.
constexpr std::array<std::string_view, 28> kCredentialStems{{
    "password", "passwd", "pwd", "passphrase", "passcode",
    "secret", "token", "apikey", "authorization", "cookie",
    "credential", "privatekey", "privkey", "connectionstring", "connstr",
    "signature", "bearer", "jwt", "accesskey", "clientsecret",
    "authkey", "sessionid", "dsn", "databaseurl", "hmac",
    "subscriptionkey", "functionskey",
    // The JSON blob GCP hands out, under its own name.
    "serviceaccount",
}};

// Credentials whose names are too SHORT to be substring stems.
//
// `pass` would swallow `passenger_count`, and `sas` any word containing it -
// exactly the over-redaction the ordinary-field-names test guards, and it
// caught both. Matched on the whole normalised key instead, plus the
// prefixed forms that actually occur.
constexpr std::array<std::string_view, 8> kCredentialWholeKeys{{
    "pass", "userpass", "dbpass", "sas", "sastoken", "saskey",
    // Whole-key so `pw_bucket` and `keyword` are unaffected. A DuckDB S3 or
    // HTTP connection property is sometimes literally named `pw` or `key`.
    "pw", "key",
}};

// Exceptions, matched on the WHOLE normalised key rather than as substrings.
//
// These contain a credential stem but are not credentials. They matter because
// flAPI is an MCP/LLM tool surface: `max_tokens` and `token_count` are ordinary
// fields here, far more common than any credential called "token count", and
// redacting them would gut the payload tier for exactly the workload it exists
// to observe. Over-redaction is the safe direction for a denylist, but it is
// not free.
constexpr std::array<std::string_view, 8> kNotCredentials{{
    "maxtokens", "mintokens", "inputtokens", "outputtokens",
    "tokencount", "tokensused", "totaltokens",
    "secretary",
}};

}  // namespace

std::string normaliseKey(std::string_view key) {
    std::string out;
    out.reserve(key.size());
    for (const char c : key) {
        if (c == '-' || c == '_') {
            continue;
        }
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

bool isCredentialKey(std::string_view key) {
    const std::string normalised = normaliseKey(key);
    if (normalised.empty()) {
        return false;
    }
    // Exceptions are whole-key, so an attacker cannot smuggle a credential past
    // the denylist by naming it `my_max_tokens`.
    if (std::find(kNotCredentials.begin(), kNotCredentials.end(), normalised)
        != kNotCredentials.end()) {
        return false;
    }
    if (std::find(kCredentialWholeKeys.begin(), kCredentialWholeKeys.end(), normalised)
        != kCredentialWholeKeys.end()) {
        return true;
    }
    return std::any_of(kCredentialStems.begin(), kCredentialStems.end(),
                       [&normalised](std::string_view stem) {
                           return normalised.find(stem) != std::string::npos;
                       });
}

namespace {

struct SqlToken {
    std::size_t begin;   // first byte of the body, after the opening quote
    std::size_t end;     // one past the body (the closing quote, or the end)
    char kind;           // '\'' literal, '"' identifier, '$' dollar-quoted
};

// String literals, quoted identifiers and dollar-quoted bodies of `sql`, as
// DuckDB's lexer sees them ('' and "" are escapes). Comments are skipped. An
// unterminated one runs to the end.
std::vector<SqlToken> TokeniseQuoted(std::string_view sql) {
    std::vector<SqlToken> out;
    std::size_t i = 0;
    while (i < sql.size()) {
        const char c = sql[i];
        if (c == '-' && i + 1 < sql.size() && sql[i + 1] == '-') {
            const auto nl = sql.find('\n', i);
            i = nl == std::string_view::npos ? sql.size() : nl + 1;
            continue;
        }
        if (c == '/' && i + 1 < sql.size() && sql[i + 1] == '*') {
            const auto close = sql.find("*/", i + 2);
            i = close == std::string_view::npos ? sql.size() : close + 2;
            continue;
        }
        if (c == '\'' || c == '"') {
            std::size_t j = i + 1;
            while (j < sql.size()) {
                if (sql[j] == c) {
                    if (j + 1 < sql.size() && sql[j + 1] == c) {
                        j += 2;
                        continue;
                    }
                    break;
                }
                ++j;
            }
            out.push_back({i + 1, std::min(j, sql.size()), c});
            i = j + 1;
            continue;
        }
        if (c == '$') {
            std::size_t j = i + 1;
            while (j < sql.size() && (std::isalnum(static_cast<unsigned char>(sql[j])) || sql[j] == '_')) {
                ++j;
            }
            if (j < sql.size() && sql[j] == '$') {
                const std::string_view tag = sql.substr(i, j - i + 1);
                const auto close = sql.find(tag, j + 1);
                const std::size_t body_end = close == std::string_view::npos ? sql.size() : close;
                out.push_back({j + 1, body_end, '$'});
                i = close == std::string_view::npos ? sql.size() : close + tag.size();
                continue;
            }
        }
        ++i;
    }
    return out;
}

void ReplaceAll(std::string& text, const std::string& needle, const std::string& with) {
    if (needle.empty()) {
        return;
    }
    for (auto pos = text.find(needle); pos != std::string::npos; pos = text.find(needle, pos + with.size())) {
        text.replace(pos, needle.size(), with);
    }
}

}  // namespace

std::string redactSqlErrorMessage(std::string_view message, std::string_view statement) {
    std::string out(message);
    // DuckDB's parser appends the statement around the failure position:
    // "\n\nLINE 1: ...SECRET 'abc'def'...\n        ^". Everything from there on
    // is statement text.
    if (const auto at = out.find("\nLINE "); at != std::string::npos) {
        out.erase(at);
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) {
        out.pop_back();
    }
    // Every quoted piece of the statement (4+ bytes), wherever the message
    // repeats it - quoted, double-quoted, nested in another message, or bare.
    std::vector<std::string> pieces;
    for (const auto& token : TokeniseQuoted(statement)) {
        std::string raw(statement.substr(token.begin, token.end - token.begin));
        if (raw.size() >= 4) {
            std::string unescaped = raw;
            const std::string doubled(2, token.kind == '$' ? '\0' : token.kind);
            ReplaceAll(unescaped, doubled, std::string(1, token.kind));
            pieces.push_back(raw);
            if (unescaped != raw) {
                pieces.push_back(unescaped);
            }
        }
    }
    std::sort(pieces.begin(), pieces.end(), [](const auto& a, const auto& b) { return a.size() > b.size(); });
    for (const auto& piece : pieces) {
        ReplaceAll(out, piece, "<redacted>");
    }
    // A syntax error names the token it stopped at - with an unescaped secret,
    // a piece of the secret that is in no literal. The rest of that line goes.
    static constexpr std::string_view kNear = "at or near ";
    for (auto pos = out.find(kNear); pos != std::string::npos; pos = out.find(kNear, pos + 1)) {
        const auto value = pos + kNear.size();
        const auto eol = out.find('\n', value);
        out.replace(value, (eol == std::string::npos ? out.size() : eol) - value, "<redacted>");
    }
    // URLs, then connection strings, an extension echoes without quotes. URLs
    // first: `<redacted>` ends a URL for RedactUrlsInText.
    out = RedactUrlsInText(out);
    out = RedactConnectionSecrets(out);
    return out;
}

std::string redactSqlLiterals(std::string_view statement) {
    std::string out;
    out.reserve(statement.size());
    std::size_t copied = 0;
    for (const auto& token : TokeniseQuoted(statement)) {
        if (token.kind == '"') {
            continue;   // an identifier names something; it is not a value
        }
        out.append(statement.substr(copied, token.begin - copied));
        out += "<redacted>";
        copied = token.end;
    }
    out.append(statement.substr(copied));
    return out;
}

}  // namespace flapi
