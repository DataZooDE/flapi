#pragma once

#include <string>
#include <string_view>

namespace flapi {

// Shared credential-key detection for every sink that can persist or export a
// caller-supplied value: the audit log, span attributes and the OpenInference
// payload overlay.
//
// This lives outside CapturePolicy on purpose. The audit log is written even
// when tracing is compiled out, so the redactor it depends on must not live in
// the tracing layer - that coupling is exactly how the audit log ended up
// writing declared `password` fields in cleartext while the tracing path was
// redacting them correctly.

// Lowercase and drop `-` and `_`, so `X-Api-Key`, `api_key` and `apikey` all
// compare equal. HTTP parameter and header names are not reliably normalised by
// anything upstream of us.
std::string normaliseKey(std::string_view key);

// True when the key looks like a credential, under ANY configuration. Matching
// is a substring test against a stem list over the normalised key, not equality:
// `auth_token`, `x-api-key` and `user_password` are all credentials and none of
// them equals a list entry.
//
// The stems are deliberately chosen to be long enough not to swallow ordinary
// data-API field names. `authorization` rather than `auth`, so `author` survives;
// `signature` rather than `sig`, so `design` survives.
bool isCredentialKey(std::string_view key);

// A DuckDB error message about `statement`, a statement that may carry
// credentials (a connection or auth `init:`), safe to log:
//  - DuckDB's `LINE n: ...` excerpt of the statement is dropped;
//  - every string literal, quoted identifier and dollar-quoted body of the
//    statement (4+ bytes) is replaced wherever the message repeats it;
//  - the token after "at or near" (rest of the line) is replaced;
//  - connection-string secrets and URL credentials/queries are masked.
// An apostrophe in the message itself is never taken for a literal.
std::string redactSqlErrorMessage(std::string_view message, std::string_view statement = {});

// `statement` with the body of every string literal and dollar-quoted string
// replaced by `<redacted>`; identifiers and keywords stay. For showing a
// connection `init:` through the config APIs.
std::string redactSqlLiterals(std::string_view statement);

}  // namespace flapi
