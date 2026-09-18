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
constexpr std::array<std::string_view, 27> kCredentialStems{{
    "password", "passwd", "pwd", "passphrase", "passcode",
    "secret", "token", "apikey", "authorization", "cookie",
    "credential", "privatekey", "privkey", "connectionstring", "connstr",
    "signature", "bearer", "jwt", "accesskey", "clientsecret",
    "authkey", "sessionid", "dsn", "databaseurl", "hmac",
    "subscriptionkey", "functionskey",
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
    return std::any_of(kCredentialStems.begin(), kCredentialStems.end(),
                       [&normalised](std::string_view stem) {
                           return normalised.find(stem) != std::string::npos;
                       });
}

}  // namespace flapi
