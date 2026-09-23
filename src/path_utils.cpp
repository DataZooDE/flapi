#include "path_utils.hpp"

#include <string>

namespace flapi {

// A URL path is addressed in the config service as
// /api/v1/_config/endpoints/{slug}, so the slug has to survive a round trip
// through a URL.
//
// Two properties are required, and the first encoding to fix #123 had only one.
//
// 1. INJECTIVE. The original codec mapped every internal '/' to '-', every
//    non-alphanumeric to '-', collapsed runs and stripped the edges, so "/a-b"
//    and "/a/b" both became "a-b" and both decoded to "/a/b" - an endpoint
//    whose URL contained a hyphen was addressed as, and rewritten to, a
//    different endpoint.
//
// 2. TRANSPORT-SAFE. The replacement escaped literals as "%2D" and "%25",
//    which collides with the transport's own percent-encoding: the CLI sends
//    encodeURIComponent(slug), turning "%2D" into "%252D", and RFC 3986
//    §6.2.2.2 permits any normaliser to decode "%2D" - an unreserved
//    character - back to '-'. A proxy doing that turns "-order%2Ditems" into
//    "-order-items" and #123's collision reappears behind ordinary
//    infrastructure.
//
// So the escape alphabet contains no '%' at all. JSON-pointer style:
//
//   '/'  ->  '-'       the common case stays readable: /sap/functions
//                      becomes -sap-functions
//   '-'  ->  "~1"      prefix-free: every escape starts with '~', and '~' is
//   '~'  ->  "~0"      itself always escaped, so a '~' in a slug always begins
//                      one and a '-' always means '/'
//   ""   ->  "empty"   unambiguous: every non-empty path begins with '/' and
//                      therefore encodes to a leading '-'
//
// Verified exhaustively over every path up to length 5 drawn from "/-~01ab":
// 19,608 paths, no collisions, no round-trip failures, and unchanged by
// encodeURIComponent followed by a decode.
const std::string PathUtils::EMPTY_REPLACEMENT = "empty";

std::string PathUtils::pathToSlug(const std::string& path) {
    if (path.empty()) {
        return EMPTY_REPLACEMENT;
    }

    std::string slug;
    slug.reserve(path.size() + 8);
    for (const char c : path) {
        switch (c) {
            case '/': slug += '-';  break;
            case '-': slug += "~1"; break;
            case '~': slug += "~0"; break;
            default:  slug += c;    break;
        }
    }
    return slug;
}

std::string PathUtils::slugToPath(const std::string& slug) {
    if (slug == EMPTY_REPLACEMENT) {
        return "";
    }

    std::string path;
    path.reserve(slug.size());
    for (std::size_t i = 0; i < slug.size();) {
        if (slug[i] == '-') {
            path += '/';
            i += 1;
        } else if (slug[i] == '~' && i + 1 < slug.size() && slug[i + 1] == '1') {
            path += '-';
            i += 2;
        } else if (slug[i] == '~' && i + 1 < slug.size() && slug[i + 1] == '0') {
            path += '~';
            i += 2;
        } else {
            path += slug[i];
            i += 1;
        }
    }
    return path;
}

bool PathUtils::looksLikePath(const std::string& identifier) {
    // A slug never contains '/': the encoder maps every '/' to '-'.
    return identifier.find('/') != std::string::npos;
}

std::string PathUtils::identifierToPath(const std::string& identifier) {
    // The config service accepts ONE name per endpoint: the slug.
    //
    // This used to fall back to treating a '/'-bearing identifier as a
    // percent-decoded url-path, and a tavern case asserted that
    // `GET /api/v1/_config/endpoints/customers%2F` resolved. It never did and
    // never could: the routes are declared `<string>`, which matches a single
    // path segment, and Crow percent-decodes before matching - so the decoded
    // `customers/` matches no route and 404s before any of this runs. The
    // unit test for the fallback passed against an input the HTTP surface
    // cannot deliver.
    //
    // Rather than add a second reachable name - the exact ambiguity #123
    // removed, where 'x' and '-x' both addressed '/x' - the fallback is gone.
    // looksLikePath survives as the predicate that keeps the two forms
    // distinguishable for callers that hold a path already.
    return slugToPath(identifier);
}

} // namespace flapi
