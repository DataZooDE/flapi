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
};

// Every variable (`{{x}}`, `{{{x}}}`, `{{&x}}`) and section (`{{#x}}`, `{{^x}}`)
// name in `body`, in source order. Closing tags, comments, partials and
// delimiter tags name nothing. Meant for a body CheckMustacheSyntax accepted; a
// malformed tag ends the scan instead of being reported.
std::vector<MustacheVariable> CollectMustacheVariables(const std::string& body);

// The template's text from the local disk or the embedded bundle; nullopt when
// it does not exist, is unreadable, or is remote (s3://, https://...).
std::optional<std::string> ReadTemplateText(const std::string& path);

// Reads a template from the local disk or the embedded bundle and checks it.
// Returns the formatted error, or nullopt when the template is fine, does not
// exist (a missing template is reported elsewhere), or is remote (s3://,
// https://...: not fetched at load; the render-time check still applies).
std::optional<std::string> CheckMustacheTemplateFile(const std::string& path);

} // namespace flapi
