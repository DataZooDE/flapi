#pragma once

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace flapi {

/// The reserved prefix under which flAPI injects the authenticated principal
/// into a template context, surfaced to templates as `auth.username`,
/// `auth.roles`, `auth.groups`, `auth.email`, `auth.type` and
/// `auth.authenticated`.
///
/// It is SERVER data. A caller must never be able to supply it - a template
/// filtering rows on `'{{{ auth.username }}}'`, the documented multi-tenant
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

/// True when `value` holds a C0 control character (0x00-0x1F) or DEL.
///
/// An identity carrying one is refused, never cleaned up: a newline ends a
/// SQL `--` comment and a NUL truncates C strings, and stripping them would
/// map one principal ("bob\n") onto another ("bob").
inline bool ContainsControlCharacter(std::string_view value) {
    for (const char c : value) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7F) {
            return true;
        }
    }
    return false;
}

/// Why an authenticated identity is refused, or nullptr when it is fine: a
/// control character in any value, or a comma in a role or group. Roles and
/// groups are joined with commas for `auth.roles` / `auth.groups` and split on
/// them again (MCP allowed-roles, `string_split('{{{ auth.roles }}}', ',')`),
/// so a role "reader,admin" would read as two roles.
inline const char* IdentityRefusal(std::string_view username, std::string_view email,
                                   const std::vector<std::string>& roles,
                                   const std::vector<std::string>& groups) {
    if (ContainsControlCharacter(username) || ContainsControlCharacter(email)) {
        return "the identity contains a control character";
    }
    for (const auto& role : roles) {
        if (ContainsControlCharacter(role)) {
            return "the identity contains a control character";
        }
        if (role.find(',') != std::string::npos) {
            return "a role contains a comma";
        }
    }
    for (const auto& group : groups) {
        if (ContainsControlCharacter(group)) {
            return "the identity contains a control character";
        }
        if (group.find(',') != std::string::npos) {
            return "a group contains a comma";
        }
    }
    return nullptr;
}

/// `values` joined with commas: how `auth.roles` and `auth.groups` reach a
/// template.
inline std::string JoinIdentityList(const std::vector<std::string>& values) {
    std::string out;
    for (const auto& value : values) {
        if (!out.empty()) {
            out += ',';
        }
        out += value;
    }
    return out;
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
