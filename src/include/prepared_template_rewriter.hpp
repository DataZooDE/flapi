#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "sql_parameter_classifier.hpp"

namespace flapi {

struct RequestFieldConfig;

// W3.1 (PR A): one entry in the binding plan produced by the rewriter.
// `position` is the 0-indexed `?` slot in the rewritten template; the
// caller binds values in that order via DuckDB's prepared-statement API.
struct PreparedBindingSpec {
    std::string field_name;
    SqlParameterType type = SqlParameterType::Varchar;
    std::size_t position = 0;
};

struct PreparedRewriteResult {
    std::string rewritten_template;
    std::vector<PreparedBindingSpec> bindings;
};

// Pure helper. Scans a Mustache template and rewrites occurrences of
// `{{ params.X }}` to `?` for parameters X that the classifier marks
// bindable, recording the binding order. Untouched:
//  - triple-brace `{{{ params.X }}}` (raw substitution; operators
//    migrate these separately)
//  - any `{{ params.X }}` inside a Mustache section block
//    (`{{#X}}...{{/X}}` or `{{^X}}...{{/X}}`)
//  - params whose `RequestFieldConfig` has no typed validator
//  - non-`params.*` references like `{{ conn.X }}` or `{{ env.X }}`
//
// The result is suitable for: render-the-rewritten-template-as-Mustache
// (only structural variation remains), then `duckdb_prepare` + bind
// per the plan.
class PreparedTemplateRewriter {
public:
    PreparedRewriteResult rewrite(const std::string& template_text,
                                  const std::vector<RequestFieldConfig>& request_fields,
                                  const SqlParameterClassifier& classifier) const;
};

} // namespace flapi
