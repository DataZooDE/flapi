#include "bearer_token_verifier.hpp"

#include <algorithm>
#include <jwt-cpp/jwt.h>

namespace flapi {

namespace {

std::optional<BearerIdentity> Refuse(std::string* reason, const std::string& why) {
    if (reason != nullptr) {
        *reason = why;
    }
    return std::nullopt;
}

}  // namespace

std::optional<BearerIdentity> VerifyBearerToken(const std::string& token, const BearerVerifierConfig& config,
                                                std::string* reason) {
    // An empty HMAC key is no secret: anyone can sign a token with it.
    if (config.secret.empty()) {
        return Refuse(reason, "jwt-secret is empty, so any token would verify");
    }
    try {
        const auto decoded = jwt::decode(token);

        // Only HS256 is allowed: jwt-cpp refuses any other `alg` in the header,
        // `none` included. It also checks `exp` when present, and refuses an
        // `nbf` or `iat` in the future.
        auto verifier = jwt::verify().allow_algorithm(jwt::algorithm::hs256{config.secret});
        if (!config.issuer.empty()) {
            verifier.with_issuer(config.issuer);
        }
        verifier.verify(decoded);

        // A token without `exp` never expires: one leaked token is good
        // forever. Refused unless the operator opts out.
        if (config.require_exp && !decoded.has_expires_at()) {
            return Refuse(reason, "the token has no 'exp' claim (set jwt-require-exp: false to accept that)");
        }

        if (!config.audiences.empty()) {
            if (!decoded.has_audience()) {
                return Refuse(reason, "the token has no 'aud' claim");
            }
            const auto token_audiences = decoded.get_audience();   // string or array
            const bool match = std::any_of(config.audiences.begin(), config.audiences.end(),
                                           [&](const std::string& a) { return token_audiences.count(a) > 0; });
            if (!match) {
                return Refuse(reason, "the token's audience is not one of jwt-audience");
            }
        }

        if (!decoded.has_subject() || decoded.get_payload_claim("sub").get_type() != jwt::json::type::string) {
            return Refuse(reason, "the token has no string 'sub' claim");
        }

        BearerIdentity identity;
        identity.username = decoded.get_subject();
        if (decoded.has_payload_claim("email")) {
            const auto email = decoded.get_payload_claim("email");
            if (email.get_type() == jwt::json::type::string) {
                identity.email = email.as_string();
            }
        }
        // Roles are optional: a token without them is an authenticated
        // principal with no roles, which `auth.roles` on an endpoint then
        // refuses with 403 - not an unauthenticated one.
        if (decoded.has_payload_claim("roles")) {
            const auto roles = decoded.get_payload_claim("roles");
            if (roles.get_type() == jwt::json::type::array) {
                for (const auto& role : roles.as_array()) {
                    if (role.is<std::string>()) {
                        identity.roles.push_back(role.get<std::string>());
                    }
                }
            } else if (roles.get_type() == jwt::json::type::string) {
                identity.roles.push_back(roles.as_string());
            }
        }
        return identity;
    } catch (const std::exception& e) {
        return Refuse(reason, e.what());
    }
}

}  // namespace flapi
