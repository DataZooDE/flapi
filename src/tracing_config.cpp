#include "tracing_config.hpp"

#include <algorithm>

namespace flapi {

CaptureTier parseCaptureTier(const std::string& value, CaptureTier fallback) {
    std::string lowered = value;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lowered == "off")      { return CaptureTier::Off; }
    if (lowered == "metadata") { return CaptureTier::Metadata; }
    if (lowered == "payload")  { return CaptureTier::Payload; }
    return fallback;
}

const char* captureTierName(CaptureTier tier) {
    switch (tier) {
        case CaptureTier::Off:      return "off";
        case CaptureTier::Metadata: return "metadata";
        case CaptureTier::Payload:  return "payload";
    }
    return "metadata";
}

DbProfiling parseDbProfiling(const std::string& value, DbProfiling fallback) {
    std::string lowered = value;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lowered == "off")      { return DbProfiling::Off; }
    if (lowered == "summary")  { return DbProfiling::Summary; }
    if (lowered == "detailed") { return DbProfiling::Detailed; }
    return fallback;
}

const char* dbProfilingName(DbProfiling level) {
    switch (level) {
        case DbProfiling::Off:      return "off";
        case DbProfiling::Summary:  return "summary";
        case DbProfiling::Detailed: return "detailed";
    }
    return "off";
}

}  // namespace flapi
