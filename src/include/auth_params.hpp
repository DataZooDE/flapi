#pragma once

#include <map>
#include <string>

namespace flapi {

/// The reserved prefix under which flAPI injects the authenticated principal
/// into a template context, surfaced to templates as `auth.username`,
/// `auth.roles`, `auth.email`, `auth.type` and `auth.authenticated`.
///
/// It is SERVER data. A caller must never be able to supply it - a template
/// filtering rows on `{{ auth.username }}`, the documented multi-tenant
/// pattern, would otherwise let the caller choose who they are.
///
/// This rule had FIVE copies: REST read, REST write, the MCP tool path, and
/// the config service's template/expand and template/test routes - the last
/// two added only after a review found them missing, and one of them
/// EXECUTES what it renders. Every re-opening this code has had came from
/// fixing one site and missing its sibling, so the rule lives here once.
inline constexpr const char* kReservedAuthPrefix = "__auth_";

inline bool isReservedAuthKey(const std::string& key) {
    return key.rfind(kReservedAuthPrefix, 0) == 0;
}

/// Remove every caller-supplied `__auth_*` entry from `params`.
inline void stripReservedAuthParams(std::map<std::string, std::string>& params) {
    for (auto it = params.begin(); it != params.end();) {
        if (isReservedAuthKey(it->first)) {
            it = params.erase(it);
        } else {
            ++it;
        }
    }
}

}  // namespace flapi
