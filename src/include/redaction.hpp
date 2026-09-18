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

}  // namespace flapi
