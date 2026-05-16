#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace flapi {

struct DescriptionIssue {
    std::string code;
    std::string message;
};

class MCPDescriptionScanner {
public:
    // Soft upper bound on description length. Longer descriptions are flagged
    // because they are typically a sign of accidental file-paste or an
    // attempt to drown out the user prompt by occupying the model's context.
    static constexpr std::size_t kMaxDescriptionLength = 2048;

    std::vector<DescriptionIssue> scan(const std::string& description) const;
};

} // namespace flapi
