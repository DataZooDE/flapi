#include "redaction.hpp"

#include <algorithm>
#include <array>
#include <cctype>

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

std::string redactSqlErrorMessage(std::string_view message) {
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
    // Then the fragments that are statement text, not names: every single-
    // quoted string literal, and the token a syntax error quotes
    // (`at or near "def"`) - with an unescaped secret, a piece of the secret.
    // Double-quoted identifiers elsewhere (an extension, a table) stay.
    std::string redacted;
    redacted.reserve(out.size());
    static constexpr std::string_view kNear = "at or near \"";
    for (std::size_t i = 0; i < out.size(); ++i) {
        char quote = 0;
        if (out[i] == '\'') {
            quote = '\'';
        } else if (out.compare(i, kNear.size(), kNear) == 0) {
            redacted.append(kNear.substr(0, kNear.size() - 1));
            i += kNear.size() - 1;
            quote = '"';
        }
        if (quote == 0) {
            redacted.push_back(out[i]);
            continue;
        }
        const auto close = out.find(quote, i + 1);
        redacted.push_back(quote);
        redacted += "<redacted>";
        if (close == std::string::npos) {
            break;
        }
        redacted.push_back(quote);
        i = close;
    }
    return redacted;
}

}  // namespace flapi
