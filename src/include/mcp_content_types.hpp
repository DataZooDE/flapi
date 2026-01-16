#pragma once

#include <crow.h>
#include <string>
#include <vector>
#include <memory>
#include <optional>

namespace flapi::mcp {

/**
 * Content Type Enum
 * Represents the different types of content that can be returned in MCP responses
 */
enum class ContentType {
    Text,          // Plain text or HTML
    Image,         // Base64-encoded image data
    Audio,         // Base64-encoded audio data
    Resource,      // External resource reference
    EmbeddedFile   // Embedded file content
};

/**
 * Text Content
 * Represents plain text or HTML content
 */
struct TextContent {
    std::string type = "text";
    std::string text;
    std::optional<std::string> mime_type;  // Optional MIME type like "text/html"
    std::optional<std::string> annotations;

    crow::json::wvalue toJson() const;
};

/**
 * Image Content
 * Represents base64-encoded image data
 */
struct ImageContent {
    std::string type = "image";
    std::string data;  // Base64-encoded image data
    std::string mime_type;  // e.g., "image/png", "image/jpeg", "image/gif"

    crow::json::wvalue toJson() const;
};

/**
 * Audio Content
 * Represents base64-encoded audio data
 */
struct AudioContent {
    std::string type = "audio";
    std::string data;  // Base64-encoded audio data
    std::string mime_type;  // e.g., "audio/wav", "audio/mp3", "audio/ogg"

    crow::json::wvalue toJson() const;
};

/**
 * Resource Content
 * Represents a reference to an external resource or embedded resource
 */
struct ResourceContent {
    std::string type = "resource";
    std::string uri;  // e.g., "flapi://resource_name" or "http://example.com/data"
    std::string mime_type;  // MIME type of the resource
    std::optional<std::string> text;  // Optional embedded text content

    crow::json::wvalue toJson() const;
};

/**
 * Embedded File Content
 * Represents embedded file data
 */
struct EmbeddedFileContent {
    std::string type = "resource";
    std::string uri;  // Resource identifier
    std::string mime_type;  // MIME type
    std::string text;  // File contents

    crow::json::wvalue toJson() const;
};

/**
 * Generic Content Block
 * Represents any MCP content type as a variant
 */
class ContentBlock {
public:
    ContentBlock() = default;
    explicit ContentBlock(const TextContent& content);
    explicit ContentBlock(const ImageContent& content);
    explicit ContentBlock(const AudioContent& content);
    explicit ContentBlock(const ResourceContent& content);
    explicit ContentBlock(const EmbeddedFileContent& content);

    crow::json::wvalue toJson() const;

    ContentType getType() const { return type_; }

private:
    ContentType type_;
    crow::json::wvalue content_json_;
};

/**
 * Content Builder
 * Utility class for constructing content blocks
 */
class ContentBuilder {
public:
    /**
     * Create text content
     */
    static ContentBlock createTextContent(const std::string& text,
                                          const std::optional<std::string>& mime_type = std::nullopt);

    /**
     * Create image content from base64-encoded data
     */
    static ContentBlock createImageContent(const std::string& base64_data,
                                           const std::string& mime_type);

    /**
     * Create audio content from base64-encoded data
     */
    static ContentBlock createAudioContent(const std::string& base64_data,
                                           const std::string& mime_type);

    /**
     * Create resource reference
     */
    static ContentBlock createResourceContent(const std::string& uri,
                                              const std::string& mime_type,
                                              const std::optional<std::string>& text = std::nullopt);

    /**
     * Create embedded file content
     */
    static ContentBlock createEmbeddedFileContent(const std::string& uri,
                                                  const std::string& mime_type,
                                                  const std::string& content);

    /**
     * Detect MIME type from file extension
     */
    static std::string detectMimeType(const std::string& filename);

    /**
     * Encode binary data to base64
     */
    static std::string encodeBase64(const std::vector<uint8_t>& data);

    /**
     * Decode base64 data
     */
    static std::vector<uint8_t> decodeBase64(const std::string& encoded);
};

/**
 * Content Response Helper
 * Utility for building tool/resource response objects with content arrays
 */
class ContentResponse {
public:
    ContentResponse();

    /**
     * Add a content block to the response
     */
    void addContent(const ContentBlock& content);

    /**
     * Add text content directly
     */
    void addText(const std::string& text);

    /**
     * Add image content directly
     */
    void addImage(const std::string& base64_data, const std::string& mime_type);

    /**
     * Get the JSON response object
     */
    crow::json::wvalue toJson() const;

private:
    crow::json::wvalue content_array_;
    size_t content_count_ = 0;
};

} // namespace flapi::mcp
