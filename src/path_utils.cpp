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
    // A slug never contains '/': the encoder maps every '/' to '-'. So an
    // identifier containing one cannot be a slug, and is the percent-decoded
    // url-path form that the config service also accepts.
    //
    // This is an explicit contract rather than a fallback. An earlier version
    // made slugToPath prepend a leading '/' whenever the decode did not start
    // with one, which made 'x' and '-x' both address '/x' - every endpoint had
    // two names again, the exact ambiguity #123 set out to remove.
    return identifier.find('/') != std::string::npos;
}

std::string PathUtils::identifierToPath(const std::string& identifier) {
    if (!looksLikePath(identifier)) {
        return slugToPath(identifier);
    }
    // Already a path. Normalise only the leading slash, which a client may or
    // may not have included.
    if (identifier.empty() || identifier.front() == '/') {
        return identifier;
    }
    return "/" + identifier;
}

} // namespace flapi
