#include "include/mcp_client_capabilities.hpp"
#include <algorithm>

namespace flapi {

MCPClientCapabilities MCPClientCapabilitiesDetector::detectFromInitialize(const crow::json::wvalue& params) {
    MCPClientCapabilities capabilities;

    if (params.t() != crow::json::type::Object) {
        return capabilities;
    }

    // Extract capabilities from the params
    if (params.count("capabilities") > 0) {
        auto caps_obj = params["capabilities"];

        if (caps_obj.t() == crow::json::type::Object) {
            // Extract sampling capability
            capabilities.supports_sampling = extractBooleanCapability(caps_obj, "sampling");

            // Extract roots capability
            capabilities.supports_roots = extractBooleanCapability(caps_obj, "roots");

            // Extract supported protocols
            capabilities.supported_protocols = extractSupportedProtocols(caps_obj);
        }
    }

    return capabilities;
}

bool MCPClientCapabilitiesDetector::supportsSampling(const MCPClientCapabilities& caps) {
    return caps.supports_sampling;
}

bool MCPClientCapabilitiesDetector::supportsRoots(const MCPClientCapabilities& caps) {
    return caps.supports_roots;
}

bool MCPClientCapabilitiesDetector::supportsLogging(const MCPClientCapabilities& caps) {
    return caps.supports_logging;
}

std::vector<std::string> MCPClientCapabilitiesDetector::getSupportedProtocols(const MCPClientCapabilities& caps) {
    return caps.supported_protocols;
}

std::vector<std::string> MCPClientCapabilitiesDetector::extractSupportedProtocols(const crow::json::wvalue& capabilities) {
    std::vector<std::string> protocols;

    if (capabilities.t() != crow::json::type::Object) {
        return protocols;
    }

    // Look for protocols in various capability objects
    for (const auto& key : capabilities.keys()) {
        auto cap_value = capabilities[key];

        if (cap_value.t() == crow::json::type::Object) {
            // Check if this capability object has supported protocols
            if (cap_value.count("supportedProtocols") > 0) {
                auto protocols_value = cap_value["supportedProtocols"];

                if (protocols_value.t() == crow::json::type::List) {
                    for (size_t i = 0; i < protocols_value.size(); ++i) {
                        auto protocol = protocols_value[i];
                        if (protocol.t() == crow::json::type::String) {
                            protocols.push_back(protocol.dump());
                        }
                    }
                }
            }
        }
    }

    return protocols;
}

bool MCPClientCapabilitiesDetector::extractBooleanCapability(const crow::json::wvalue& capabilities,
                                                            const std::string& capability_name) {
    if (capabilities.count(capability_name) == 0) {
        return false;
    }

    auto cap_value = capabilities[capability_name];
    return cap_value.t() == crow::json::type::True;
}

} // namespace flapi
