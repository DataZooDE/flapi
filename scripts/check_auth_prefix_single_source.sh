#!/usr/bin/env bash
# The `__auth_*` rule must have exactly one definition.
#
# It is the prefix under which flAPI injects the authenticated principal into
# a template context. A caller who can supply it chooses who they are, and a
# template filtering on `{{ auth.username }}` - the documented multi-tenant
# pattern - then filters on the caller's claim.
#
# The rule had five copies: REST read, REST write, the MCP tool path, and the
# config service's template/expand and template/test routes. The last two were
# found missing by a review, and one of them EXECUTES what it renders. Every
# re-opening this area has had came from fixing one site and missing a
# sibling, so a sixth literal is a build failure.
set -euo pipefail
cd "$(dirname "$0")/.."

offenders=$(grep -rn '"__auth_"' src/ --include='*.cpp' --include='*.hpp' \
            | grep -v '^src/include/auth_params.hpp:' || true)

if [ -n "$offenders" ]; then
    echo "ERROR: the __auth_ prefix is spelled outside auth_params.hpp:" >&2
    echo "$offenders" >&2
    echo >&2
    echo "Use flapi::isReservedAuthKey() or flapi::stripReservedAuthParams()." >&2
    exit 1
fi

# ...and the single source must actually still be there.
if ! grep -q 'kReservedAuthPrefix = "__auth_"' src/include/auth_params.hpp; then
    echo "ERROR: auth_params.hpp no longer defines the reserved prefix." >&2
    exit 1
fi

echo "OK: the __auth_ prefix is defined once, in auth_params.hpp."
