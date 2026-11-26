#pragma once

#include <optional>
#include <string>
#include <vector>

namespace flapi {

/// What an HS256 bearer token is checked against (`type: bearer`).
///
/// REST (AuthMiddleware) and MCP (MCPAuthHandler) used to carry a copy each,
/// and the copies disagreed: an unset `jwt-issuer` required `iss: ""` on REST
/// and `iss: "flapi"` on MCP, REST refused a token without `roles` while MCP
/// accepted it, and only MCP filled the email (#281). Both now call
/// VerifyBearerToken, so a token gets one answer whichever surface it is sent
/// to.
struct BearerVerifierConfig {
    std::string secret;                   // HS256 key; empty refuses everything
    std::string issuer;                   // empty: `iss` is not checked
    std::vector<std::string> audiences;   // empty: `aud` is not checked; else one must match
    bool require_exp = true;              // a token without `exp` is refused
};

/// The principal a verified token names.
struct BearerIdentity {
    std::string username;                 // `sub` (required)
    std::string email;                    // `email`, when the token has a string one
    std::vector<std::string> roles;       // `roles`: array of strings, or one string; else empty
};

/// Verify `token` (the part after "Bearer "): an HS256 signature with
/// `config.secret`, `exp` (required unless `require_exp` is false; an expired
/// one is always refused), `nbf` and `iat` not in the future, the issuer and
/// audience when configured, and a string `sub`. nullopt when any check
/// fails; `reason`, when given, then says which (never the token).
std::optional<BearerIdentity> VerifyBearerToken(const std::string& token, const BearerVerifierConfig& config,
                                                std::string* reason = nullptr);

}  // namespace flapi
