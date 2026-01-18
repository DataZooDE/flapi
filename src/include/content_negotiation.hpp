#pragma once

#include <string>
#include <vector>
#include <map>
#include <optional>
#include <algorithm>
#include <sstream>
#include <cctype>

namespace flapi {

/**
 * Represents a parsed media type from Accept header.
 * e.g., "application/vnd.apache.arrow.stream;codec=zstd;q=0.9"
 */
struct MediaType {
    std::string type;           // e.g., "application"
    std::string subtype;        // e.g., "vnd.apache.arrow.stream"
    double quality = 1.0;       // q parameter, default 1.0
    std::map<std::string, std::string> parameters;  // Other params like codec

    std::string fullType() const {
        return type + "/" + subtype;
    }

    bool isArrowStream() const {
        return fullType() == "application/vnd.apache.arrow.stream";
    }

    bool isJson() const {
        return fullType() == "application/json";
    }

    bool isCsv() const {
        return fullType() == "text/csv";
    }

    bool isWildcard() const {
        return type == "*" && subtype == "*";
    }
};

/**
 * Response format preferences for an endpoint.
 */
struct ResponseFormatConfig {
    std::vector<std::string> formats;  // Supported formats: json, arrow, csv
    std::string defaultFormat = "json";
    bool arrowEnabled = false;
};

/**
 * Selected response format after content negotiation.
 */
enum class ResponseFormat {
    JSON,
    ARROW_STREAM,
    CSV,
    UNSUPPORTED
};

/**
 * Content negotiation result.
 */
struct NegotiationResult {
    ResponseFormat format = ResponseFormat::UNSUPPORTED;
    std::string codec;  // For Arrow: lz4, zstd, or empty for none
    std::string errorMessage;
};

namespace detail {

inline std::string trim(const std::string& str) {
    size_t start = 0;
    size_t end = str.length();
    while (start < end && std::isspace(static_cast<unsigned char>(str[start]))) {
        ++start;
    }
    while (end > start && std::isspace(static_cast<unsigned char>(str[end - 1]))) {
        --end;
    }
    return str.substr(start, end - start);
}

inline std::string toLower(const std::string& str) {
    std::string result = str;
    std::transform(result.begin(), result.end(), result.begin(),
        [](unsigned char c) { return std::tolower(c); });
    return result;
}

inline std::vector<std::string> split(const std::string& str, char delim) {
    std::vector<std::string> tokens;
    std::stringstream ss(str);
    std::string token;
    while (std::getline(ss, token, delim)) {
        tokens.push_back(token);
    }
    return tokens;
}

} // namespace detail

/**
 * Parse Accept header according to RFC 7231.
 * Returns media types sorted by quality value (highest first).
 */
inline std::vector<MediaType> parseAcceptHeader(const std::string& header) {
    std::vector<MediaType> result;

    std::string trimmed = detail::trim(header);
    if (trimmed.empty()) {
        return result;
    }

    // Split by comma to get individual media type entries
    auto entries = detail::split(trimmed, ',');

    for (auto& entry : entries) {
        entry = detail::trim(entry);
        if (entry.empty()) {
            continue;
        }

        MediaType mediaType;

        // Split by semicolon to separate media type from parameters
        auto parts = detail::split(entry, ';');
        if (parts.empty()) {
            continue;
        }

        // Parse media type (type/subtype)
        std::string typeSubtype = detail::trim(parts[0]);
        auto slashPos = typeSubtype.find('/');
        if (slashPos == std::string::npos) {
            // Invalid media type without slash - treat as wildcard subtype
            mediaType.type = detail::toLower(typeSubtype);
            mediaType.subtype = "*";
            // Skip invalid entries
            continue;
        }

        mediaType.type = detail::toLower(detail::trim(typeSubtype.substr(0, slashPos)));
        mediaType.subtype = detail::toLower(detail::trim(typeSubtype.substr(slashPos + 1)));

        // Parse parameters
        for (size_t i = 1; i < parts.size(); ++i) {
            std::string param = detail::trim(parts[i]);
            auto eqPos = param.find('=');
            if (eqPos == std::string::npos) {
                continue;
            }

            std::string key = detail::toLower(detail::trim(param.substr(0, eqPos)));
            std::string value = detail::trim(param.substr(eqPos + 1));

            if (key == "q") {
                // Parse quality value
                try {
                    mediaType.quality = std::stod(value);
                    // Clamp to valid range
                    if (mediaType.quality < 0.0) mediaType.quality = 0.0;
                    if (mediaType.quality > 1.0) mediaType.quality = 1.0;
                } catch (...) {
                    // Invalid quality value, use default
                    mediaType.quality = 1.0;
                }
            } else {
                mediaType.parameters[key] = value;
            }
        }

        result.push_back(mediaType);
    }

    // Sort by quality value (descending), stable to preserve order for equal quality
    std::stable_sort(result.begin(), result.end(),
        [](const MediaType& a, const MediaType& b) {
            return a.quality > b.quality;
        });

    return result;
}

/**
 * Convert format string to ResponseFormat enum.
 */
inline ResponseFormat stringToFormat(const std::string& format) {
    std::string lower = detail::toLower(format);
    if (lower == "json") {
        return ResponseFormat::JSON;
    } else if (lower == "arrow") {
        return ResponseFormat::ARROW_STREAM;
    } else if (lower == "csv") {
        return ResponseFormat::CSV;
    }
    return ResponseFormat::UNSUPPORTED;
}

/**
 * Check if a format is supported by the endpoint configuration.
 */
inline bool isFormatSupported(const std::string& format, const ResponseFormatConfig& config) {
    std::string lower = detail::toLower(format);
    for (const auto& supported : config.formats) {
        if (detail::toLower(supported) == lower) {
            return true;
        }
    }
    return false;
}

/**
 * Check if a media type is acceptable (q > 0).
 */
inline bool isAcceptable(const MediaType& mediaType) {
    return mediaType.quality > 0.0;
}

/**
 * Perform content negotiation.
 *
 * Priority:
 * 1. Query parameter override (?format=arrow)
 * 2. HTTP Accept header with quality values
 * 3. Endpoint-level default format configuration
 */
inline NegotiationResult negotiateContentType(
    const std::string& acceptHeader,
    const std::string& formatQueryParam,
    const ResponseFormatConfig& endpointConfig
) {
    NegotiationResult result;

    // Priority 1: Query parameter override
    if (!formatQueryParam.empty()) {
        std::string lower = detail::toLower(formatQueryParam);

        // Check if format is supported
        if (!isFormatSupported(lower, endpointConfig)) {
            result.format = ResponseFormat::UNSUPPORTED;
            result.errorMessage = "Format '" + formatQueryParam + "' is not supported by this endpoint";
            return result;
        }

        // Check Arrow-specific requirements
        if (lower == "arrow" && !endpointConfig.arrowEnabled) {
            result.format = ResponseFormat::UNSUPPORTED;
            result.errorMessage = "Arrow format is not enabled for this endpoint";
            return result;
        }

        result.format = stringToFormat(lower);
        return result;
    }

    // Priority 2: Accept header
    auto mediaTypes = parseAcceptHeader(acceptHeader);

    for (const auto& mt : mediaTypes) {
        // Skip if not acceptable (q=0)
        if (!isAcceptable(mt)) {
            continue;
        }

        // Check Arrow stream
        if (mt.isArrowStream()) {
            if (endpointConfig.arrowEnabled && isFormatSupported("arrow", endpointConfig)) {
                result.format = ResponseFormat::ARROW_STREAM;
                // Extract codec if specified
                auto codecIt = mt.parameters.find("codec");
                if (codecIt != mt.parameters.end()) {
                    std::string codec = detail::toLower(codecIt->second);
                    // Only accept valid codecs
                    if (codec == "lz4" || codec == "zstd") {
                        result.codec = codec;
                    }
                    // Invalid codec silently ignored (no compression)
                }
                return result;
            }
            // Arrow requested but not enabled, continue to next preference
            continue;
        }

        // Check JSON
        if (mt.isJson()) {
            if (isFormatSupported("json", endpointConfig)) {
                result.format = ResponseFormat::JSON;
                return result;
            }
            continue;
        }

        // Check CSV
        if (mt.isCsv()) {
            if (isFormatSupported("csv", endpointConfig)) {
                result.format = ResponseFormat::CSV;
                return result;
            }
            continue;
        }

        // Check wildcard
        if (mt.isWildcard()) {
            // Use endpoint default
            std::string defaultFmt = detail::toLower(endpointConfig.defaultFormat);
            if (defaultFmt == "arrow" && endpointConfig.arrowEnabled) {
                result.format = ResponseFormat::ARROW_STREAM;
            } else if (defaultFmt == "csv") {
                result.format = ResponseFormat::CSV;
            } else {
                result.format = ResponseFormat::JSON;
            }
            return result;
        }
    }

    // Priority 3: No Accept header or no match - use endpoint default
    if (mediaTypes.empty()) {
        std::string defaultFmt = detail::toLower(endpointConfig.defaultFormat);
        if (defaultFmt == "arrow" && endpointConfig.arrowEnabled) {
            result.format = ResponseFormat::ARROW_STREAM;
        } else if (defaultFmt == "csv" && isFormatSupported("csv", endpointConfig)) {
            result.format = ResponseFormat::CSV;
        } else if (isFormatSupported("json", endpointConfig)) {
            result.format = ResponseFormat::JSON;
        } else {
            result.format = ResponseFormat::UNSUPPORTED;
            result.errorMessage = "No supported response format available";
        }
        return result;
    }

    // No acceptable match found
    result.format = ResponseFormat::UNSUPPORTED;
    result.errorMessage = "No acceptable response format found";
    return result;
}

} // namespace flapi
