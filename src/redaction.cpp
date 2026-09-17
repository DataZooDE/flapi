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
// Deliberate accepted false positive: anything containing "token", such as
// `tokenizer_version`. Missing `auth_token` costs a leaked bearer token;
// redacting a version string costs nothing that matters.
constexpr std::array<std::string_view, 16> kCredentialStems{{
    "password", "passwd", "pwd", "secret",
    "token", "apikey", "authorization", "cookie",
    "credential", "privatekey", "connectionstring", "signature",
    "bearer", "jwt", "accesskey", "clientsecret",
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
    return std::any_of(kCredentialStems.begin(), kCredentialStems.end(),
                       [&normalised](std::string_view stem) {
                           return normalised.find(stem) != std::string::npos;
                       });
}

}  // namespace flapi
