#!/usr/bin/env bash
# Guard for the invariant documented in src/include/flapi_app.hpp:
# `crow::App<...>` must be spelled exactly once, in the FlapiApp alias.
#
# Two spellings of the middleware list are two distinct, valid types. Adding a
# middleware to one and not the other compiles cleanly and yields a second,
# unconfigured middleware tuple - a silent failure that looks like "the
# middleware does nothing on some routes".
set -euo pipefail
cd "$(dirname "$0")/.."

offenders=$(grep -rn 'crow::App<' src/ --include='*.hpp' --include='*.cpp' \
            | grep -v '^src/include/flapi_app.hpp:' || true)

if [ -n "$offenders" ]; then
    echo "ERROR: crow::App<...> spelled outside the FlapiApp alias:" >&2
    echo "$offenders" >&2
    echo >&2
    echo "Use flapi::FlapiApp from src/include/flapi_app.hpp instead." >&2
    exit 1
fi
echo "OK: crow::App<...> appears only in the FlapiApp alias."
