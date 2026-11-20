#pragma once

#include <optional>
#include <string>

#include <yaml-cpp/yaml.h>

namespace flapi {

// Applies the difference between `base` and `merged` to the YAML TEXT
// `previous`, leaving everything that did not change - comments, blank lines,
// key order, flow-style values, quoting - exactly as the author wrote it.
//
// `base` must be YAML::Load(previous) and `merged` the document that should
// result. YAML::Emitter drops every comment, so re-emitting `merged` makes a
// configuration-service update destroy the documentation in an endpoint file.
//
// Works on block-style maps by key, recursing into a map whose entries changed
// and replacing an entry wholesale only where its value changed and is not a
// map (a sequence, a scalar). An inline `# comment` after a changed single-line
// scalar is kept. A key present in `merged` but not in the text is appended at
// the end of its map.
//
// The result is verified: it is parsed again and must equal `merged`. Returns
// nullopt - the caller falls back to a plain re-emit - when the layout is
// outside what a line-based patch handles (document markers, quoted or complex
// keys, a key dropped by the update, multi-document input) or when the
// verification fails. It never returns text that does not mean `merged`.
std::optional<std::string> PatchYamlPreservingComments(const std::string& previous, const YAML::Node& base,
                                                       const YAML::Node& merged);

}  // namespace flapi
