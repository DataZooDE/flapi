/**
 * Arrow IPC Configuration Schema
 *
 * Defines configuration structures and parsing for Arrow IPC streaming.
 * Supports:
 * - Global configuration in flapi.yaml
 * - Endpoint-level overrides
 * - Request-level parameter handling
 */

#pragma once

#include <string>
#include <optional>
#include <algorithm>
#include <cctype>

namespace flapi {

/**
 * Arrow compression configuration.
 */
struct ArrowCompressionConfig {
    std::string codec;          // "", "lz4", "zstd"
    int level = 0;              // 0 = default, 1-22 for zstd

    bool hasCompression() const {
        return !codec.empty();
    }
};

/**
 * Arrow resource limits.
 */
struct ArrowLimitsConfig {
    size_t maxMemoryPerRequest = 256 * 1024 * 1024;  // 256 MB per request
    size_t maxMemoryGlobal = 2ULL * 1024 * 1024 * 1024;  // 2 GB total
    int maxConcurrentStreams = 10;                    // Concurrent Arrow responses
    int maxBatchesPerStream = 10000;                  // Prevent infinite streams
    size_t maxRowsPerStream = 10000000;               // Soft limit with warning
    int streamTimeoutSeconds = 600;                   // 10 minute maximum
};

/**
 * Arrow fallback behavior configuration.
 */
struct ArrowFallbackConfig {
    std::string onUnsupportedType = "error";         // error | omit_column | json
    std::string onMemoryExhaustion = "error";        // error | json
};

/**
 * Arrow security configuration.
 */
struct ArrowSecurityConfig {
    int maxSchemaDepth = 64;                         // Nested type limit
    bool allowExtensionTypes = false;                // Disable for security
};

/**
 * Default Arrow serialization settings.
 */
struct ArrowDefaultsConfig {
    size_t batchSize = 8192;                         // Rows per batch
    ArrowCompressionConfig compression;
};

/**
 * Global Arrow configuration (from flapi.yaml).
 */
struct ArrowGlobalConfig {
    bool enabled = true;                             // Master switch
    ArrowDefaultsConfig defaults;
    ArrowLimitsConfig limits;
    ArrowFallbackConfig fallback;
    ArrowSecurityConfig security;
};

/**
 * Endpoint-level Arrow configuration (from endpoint yaml).
 */
struct ArrowEndpointConfig {
    std::optional<bool> enabled;                     // Override global enable
    std::optional<size_t> batchSize;                 // Override batch size
    std::optional<ArrowCompressionConfig> compression;  // Override compression
};

/**
 * Request-level Arrow parameters.
 */
struct ArrowRequestParams {
    std::optional<size_t> batchSize;                 // ?arrow_batch_size=N
    std::optional<std::string> codec;                // From Accept header codec param
};

/**
 * Merged Arrow configuration for a specific request.
 * Combines global, endpoint, and request-level settings.
 */
struct ArrowEffectiveConfig {
    bool enabled = true;
    size_t batchSize = 8192;
    ArrowCompressionConfig compression;
    ArrowLimitsConfig limits;
    ArrowFallbackConfig fallback;
    ArrowSecurityConfig security;

    /**
     * Create effective config by merging global, endpoint, and request settings.
     * Priority: request > endpoint > global
     */
    static ArrowEffectiveConfig merge(
        const ArrowGlobalConfig& global,
        const ArrowEndpointConfig& endpoint,
        const ArrowRequestParams& request
    ) {
        ArrowEffectiveConfig effective;

        // Start with global config
        effective.enabled = global.enabled;
        effective.batchSize = global.defaults.batchSize;
        effective.compression = global.defaults.compression;
        effective.limits = global.limits;
        effective.fallback = global.fallback;
        effective.security = global.security;

        // Apply endpoint overrides
        if (endpoint.enabled.has_value()) {
            effective.enabled = endpoint.enabled.value();
        }
        if (endpoint.batchSize.has_value()) {
            effective.batchSize = endpoint.batchSize.value();
        }
        if (endpoint.compression.has_value()) {
            effective.compression = endpoint.compression.value();
        }

        // Apply request overrides
        if (request.batchSize.has_value()) {
            effective.batchSize = request.batchSize.value();
        }
        if (request.codec.has_value()) {
            effective.compression.codec = request.codec.value();
        }

        // Clamp batch size to limits
        if (effective.batchSize == 0) {
            effective.batchSize = global.defaults.batchSize;
        }
        if (effective.batchSize > 1000000) {
            effective.batchSize = 1000000;  // Cap at 1M rows per batch
        }

        return effective;
    }
};

/**
 * Helper to parse codec string and validate.
 */
inline std::string parseAndValidateCodec(const std::string& codec) {
    std::string lower = codec;
    std::transform(lower.begin(), lower.end(), lower.begin(),
        [](unsigned char c) { return std::tolower(c); });

    if (lower == "zstd" || lower == "lz4" || lower.empty()) {
        return lower;
    }
    // Invalid codec - return empty (no compression)
    return "";
}

/**
 * Helper to parse compression level and clamp to valid range.
 */
inline int parseAndClampCompressionLevel(int level, const std::string& codec) {
    if (codec == "zstd") {
        // ZSTD levels: 1-22, default 3
        if (level <= 0) return 3;
        if (level > 22) return 22;
        return level;
    } else if (codec == "lz4") {
        // LZ4 doesn't have levels in our implementation
        return 0;
    }
    return 0;
}

} // namespace flapi
