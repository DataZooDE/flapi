#include "path_utils.hpp"

#include <cctype>
#include <string>

namespace flapi {

// A URL path is addressed in the config service as
// /api/v1/_config/endpoints/{slug}, so the slug has to survive a round trip.
// The previous codec did not: it replaced every internal '/' with '-', every
// non-alphanumeric with '-', collapsed runs of '-' and stripped the edges, so
// "/a-b" and "/a/b" both became "a-b" and both decoded to "/a/b". An endpoint
// whose URL contained a hyphen - entirely ordinary - was addressed as, and
// rewritten to, a different endpoint (#123).
//
// This encoding is injective, verified exhaustively over every path up to
// length 5 drawn from "/-%2Dab" (19,608 paths, no collisions, no round-trip
// failures):
//
//   '/'  ->  '-'        so the common case stays readable: /sap/functions
//                       becomes -sap-functions
//   '-'  ->  "%2D"      a literal hyphen is escaped, which is what makes a
//   '%'  ->  "%25"      '-' in a slug unambiguously mean '/'
//   ""   ->  "empty"    unambiguous because every non-empty path begins with
//                       '/' and therefore encodes to a leading '-'
//
// Two earlier attempts failed on exactly one point, worth recording so the
// next person does not repeat them: a one-character escape for '/' cannot
// coexist with a two-character escape for the literal, because the shorter is
// a prefix of the longer. "//x" and "_x" both produced "__x".
const std::string PathUtils::EMPTY_REPLACEMENT = "empty";

namespace {

bool matchesEscape(const std::string& s, std::size_t i, const char* escape) {
    // Case-insensitive on the hex digit, since percent-encoding conventionally is.
    if (i + 2 >= s.size() + 0 && i + 3 > s.size()) {
        return false;
    }
    return s[i] == escape[0] &&
           std::toupper(static_cast<unsigned char>(s[i + 1])) ==
               std::toupper(static_cast<unsigned char>(escape[1])) &&
           std::toupper(static_cast<unsigned char>(s[i + 2])) ==
               std::toupper(static_cast<unsigned char>(escape[2]));
}

}  // namespace

std::string PathUtils::pathToSlug(const std::string& path) {
    if (path.empty()) {
        return EMPTY_REPLACEMENT;
    }

    std::string slug;
    slug.reserve(path.size() + 8);
    for (const char c : path) {
        switch (c) {
            case '/': slug += '-';     break;
            case '-': slug += "%2D";   break;
            case '%': slug += "%25";   break;
            default:  slug += c;       break;
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
        } else if (i + 3 <= slug.size() && matchesEscape(slug, i, "%2D")) {
            path += '-';
            i += 3;
        } else if (i + 3 <= slug.size() && matchesEscape(slug, i, "%25")) {
            path += '%';
            i += 3;
        } else {
            path += slug[i];
            i += 1;
        }
    }

    // Tolerate an identifier that is already a path. The config service also
    // accepts a percent-encoded url-path in this position - the tavern suite
    // addresses endpoints as "northwind%2Fproducts%2F" - which arrives here
    // url-decoded as "northwind/products/" and contains nothing this codec
    // encodes. The old decoder prepended the leading slash unconditionally,
    // because it stripped one when encoding; this one encodes the leading '/'
    // as '-', so a slug produced by pathToSlug already decodes with it.
    //
    // Adding it only when absent keeps both callers working and cannot
    // collide: every slug from pathToSlug for a non-empty path starts with
    // '-', hence decodes to a leading '/'.
    if (!path.empty() && path.front() != '/') {
        path.insert(path.begin(), '/');
    }
    return path;
}

} // namespace flapi
