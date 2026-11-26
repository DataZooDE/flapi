#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace flapi {

// A Mustache syntax problem, located in the template source.
struct MustacheSyntaxError {
    std::size_t line = 1;    // 1-based
    std::size_t column = 1;  // 1-based
    std::string message;
};

// Checks a SQL template BEFORE it reaches crow::mustache::compile.
//
// The bundled crow mustache parser does no balance checking of its own: a
// stray `{{/x}}` calls back() on an empty vector (SEGFAULT on render) and an
// unclosed `{{#x}}` leaves the block's jump target at 0, so rendering it with
// x absent loops forever while growing the output without bound. Neither may
// be reachable from configuration, so every template is held to this check
// first. It mirrors crow's own tokenisation (`{{` ... `}}`, spaces trimmed)
// and is deliberately at least as strict, so a template it accepts compiles.
//
// Rejected: unbalanced or crossed sections (`#`/`^` vs `/`, names must match),
// `{{{` not closed by `}}}`, an unterminated `{{`, empty tags and section names,
// partials (`{{> x}}`: crow loads them from a cwd-relative "templates/"
// directory unrelated to template.path, and a self-including one recurses until
// the stack overflows) and set-delimiter tags (`{{= =}}`), which no flAPI
// template uses and whose crow parsing is index arithmetic we do not mirror.
std::optional<MustacheSyntaxError> CheckMustacheSyntax(const std::string& body);

// "<label>:<line>:<column>: <message>", or "line N, column M: ..." without a label.
std::string FormatMustacheSyntaxError(const MustacheSyntaxError& error, const std::string& label);

// Throws std::invalid_argument("Invalid template <label>:<line>:<col>: ...")
// when CheckMustacheSyntax reports a problem.
void RequireValidMustacheSyntax(const std::string& body, const std::string& label);

// A variable or section name used by a template, located in the source.
struct MustacheVariable {
    std::string name;        // as written, spaces trimmed: "params.id"
    std::size_t line = 1;    // 1-based
    std::size_t column = 1;  // 1-based
    std::string detail;      // FindUnquotedAuthVariables: where the tag sits
    std::size_t offset = 0;  // FindUnquotedAuthVariables: byte offset in the text
};

// A placeholder rendered in place of an auth.* value (see NewAuthMarkerPrefix).
struct AuthMarker {
    std::string marker;  // prefix + one letter + 'z'
    std::string name;    // "auth.username"
    std::string value;   // what replaces it once the SQL is checked (already SQL-quoted)
};

// Every variable (`{{x}}`, `{{{x}}}`, `{{&x}}`) and section (`{{#x}}`, `{{^x}}`)
// name in `body`, in source order. Closing tags, comments, partials and
// delimiter tags name nothing. Meant for a body CheckMustacheSyntax accepted; a
// malformed tag ends the scan instead of being reported.
std::vector<MustacheVariable> CollectMustacheVariables(const std::string& body);

// Every auth.* value tag (auth.username, auth.email, auth.roles, auth.groups, auth.type,
// and the same names resolved inside a `{{#auth}}` section or as `{{.}}`)
// that is NOT inside a plain top-level single-quoted SQL literal: unquoted, in
// a comment, an E'...' escape string, a "..." identifier, a $$ string, a
// literal holding a doubled quote (SQL text), or a literal inside a call of a
// SQL-executing function (query(), *_query, *_execute, ...). `detail` says
// where the tag sits. A lint over the template source, for load and
// --validate-config. Meant for a body CheckMustacheSyntax accepted.
std::vector<MustacheVariable> FindUnquotedAuthVariables(const std::string& body);

// A fresh random marker prefix (letters and digits only).
std::string NewAuthMarkerPrefix();

// The same rules over RENDERED SQL, for each occurrence of a marker: where
// DuckDB will see it, with every Mustache tag already resolved.
std::vector<MustacheVariable> FindUnsafeAuthMarkers(const std::string& sql, const std::string& prefix,
                                                    const std::vector<AuthMarker>& markers);

// Replaces every marker with its value, in one pass.
std::string SubstituteAuthMarkers(const std::string& sql, const std::string& prefix,
                                  const std::vector<AuthMarker>& markers);

// "Unsafe use of auth.* in <label>:<line>:<col>: ..." for one problem.
std::string FormatAuthPlacementProblem(const MustacheVariable& problem, const std::string& label);

// The source lint, then a sample render with markers for every auth.* value:
// the formatted first problem, or nullopt.
std::optional<std::string> CheckAuthPlacement(const std::string& body, const std::string& label);

// CheckAuthPlacement for a template file (local disk or the embedded bundle);
// nullopt when it is fine, missing, malformed or remote.
std::optional<std::string> CheckAuthPlacementInTemplateFile(const std::string& path);

// The template's text from the local disk or the embedded bundle; nullopt when
// it does not exist, is unreadable, or is remote (s3://, https://...).
std::optional<std::string> ReadTemplateText(const std::string& path);

// Reads a template from the local disk or the embedded bundle and checks it.
// Returns the formatted error, or nullopt when the template is fine, does not
// exist (a missing template is reported elsewhere), or is remote (s3://,
// https://...: not fetched at load; the render-time check still applies).
std::optional<std::string> CheckMustacheTemplateFile(const std::string& path);

} // namespace flapi
