#pragma once

#include "mcp_types.hpp"
#include <crow.h>
#include <string>
#include <vector>

namespace flapi {

class MCPClientCapabilitiesDetector {
public:
    // Detect client capabilities from initialize request
    static MCPClientCapabilities detectFromInitialize(const crow::json::wvalue& params);

    // Check specific capabilities
    static bool supportsSampling(const MCPClientCapabilities& caps);
    static bool supportsRoots(const MCPClientCapabilities& caps);
    static bool supportsLogging(const MCPClientCapabilities& caps);

    // Get supported protocols
    static std::vector<std::string> getSupportedProtocols(const MCPClientCapabilities& caps);

private:
    // Parse capabilities from JSON
    static std::vector<std::string> extractSupportedProtocols(const crow::json::wvalue& capabilities);
    static bool extractBooleanCapability(const crow::json::wvalue& capabilities,
                                       const std::string& capability_name);
};

} // namespace flapi
