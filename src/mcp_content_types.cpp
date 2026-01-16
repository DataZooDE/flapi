#include "mcp_content_types.hpp"
#include <sstream>
#include <algorithm>
#include <cctype>

namespace flapi::mcp {

// Base64 encoding/decoding helpers
static const std::string BASE64_CHARS =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string ContentBuilder::encodeBase64(const std::vector<uint8_t>& data) {
    std::string encoded;
    int i = 0;
    unsigned char char_array_3[3];
    unsigned char char_array_4[4];

    for (size_t n = 0; n < data.size(); n++) {
        char_array_3[i++] = data[n];
        if (i == 3) {
            char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
            char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
            char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
            char_array_4[3] = char_array_3[2] & 0x3f;

            for (i = 0; i < 4; i++)
                encoded += BASE64_CHARS[char_array_4[i]];
            i = 0;
        }
    }

    if (i > 0) {
        for (int j = i; j < 3; j++)
            char_array_3[j] = '\0';

        char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
        char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
        char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);

        for (int j = 0; j <= i; j++)
            encoded += BASE64_CHARS[char_array_4[j]];

        while (i++ < 3)
            encoded += '=';
    }

    return encoded;
}

std::vector<uint8_t> ContentBuilder::decodeBase64(const std::string& encoded) {
    std::vector<uint8_t> decoded;
    std::vector<int> T(256, -1);
    for (int i = 0; i < 64; i++) T[BASE64_CHARS[i]] = i;

    int val = 0;
    int valb = -6;
    for (unsigned char c : encoded) {
        if (T[c] == -1) break;
        val = (val << 6) + T[c];
        valb += 6;
        if (valb >= 0) {
            decoded.push_back(char((val >> valb) & 0xFF));
            valb -= 8;
        }
    }

    return decoded;
}

std::string ContentBuilder::detectMimeType(const std::string& filename) {
    // Find file extension
    size_t dot_pos = filename.find_last_of('.');
    if (dot_pos == std::string::npos) {
        return "application/octet-stream";  // Default for unknown
    }

    std::string ext = filename.substr(dot_pos + 1);
    // Convert to lowercase
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    // Image MIME types
    if (ext == "png") return "image/png";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "gif") return "image/gif";
    if (ext == "webp") return "image/webp";
    if (ext == "svg") return "image/svg+xml";
    if (ext == "bmp") return "image/bmp";

    // Audio MIME types
    if (ext == "wav") return "audio/wav";
    if (ext == "mp3") return "audio/mpeg";
    if (ext == "ogg" || ext == "oga") return "audio/ogg";
    if (ext == "m4a") return "audio/mp4";
    if (ext == "aac") return "audio/aac";
    if (ext == "flac") return "audio/flac";

    // Video MIME types
    if (ext == "mp4") return "video/mp4";
    if (ext == "webm") return "video/webm";
    if (ext == "mov") return "video/quicktime";
    if (ext == "avi") return "video/x-msvideo";

    // Document MIME types
    if (ext == "pdf") return "application/pdf";
    if (ext == "json") return "application/json";
    if (ext == "xml") return "application/xml";
    if (ext == "csv") return "text/csv";
    if (ext == "txt") return "text/plain";
    if (ext == "html" || ext == "htm") return "text/html";

    return "application/octet-stream";
}

// TextContent implementation
crow::json::wvalue TextContent::toJson() const {
    crow::json::wvalue result;
    result["type"] = type;
    result["text"] = text;
    if (mime_type) {
        result["mimeType"] = mime_type.value();
    }
    return result;
}

// ImageContent implementation
crow::json::wvalue ImageContent::toJson() const {
    crow::json::wvalue result;
    result["type"] = type;
    result["data"] = data;
    result["mimeType"] = mime_type;
    return result;
}

// AudioContent implementation
crow::json::wvalue AudioContent::toJson() const {
    crow::json::wvalue result;
    result["type"] = type;
    result["data"] = data;
    result["mimeType"] = mime_type;
    return result;
}

// ResourceContent implementation
crow::json::wvalue ResourceContent::toJson() const {
    crow::json::wvalue result;
    result["type"] = type;
    result["resource"]["uri"] = uri;
    result["resource"]["mimeType"] = mime_type;
    if (text) {
        result["resource"]["text"] = text.value();
    }
    return result;
}

// EmbeddedFileContent implementation
crow::json::wvalue EmbeddedFileContent::toJson() const {
    crow::json::wvalue result;
    result["type"] = type;
    result["resource"]["uri"] = uri;
    result["resource"]["mimeType"] = mime_type;
    result["resource"]["text"] = text;
    return result;
}

// ContentBlock implementation
ContentBlock::ContentBlock(const TextContent& content)
    : type_(ContentType::Text), content_json_(content.toJson()) {}

ContentBlock::ContentBlock(const ImageContent& content)
    : type_(ContentType::Image), content_json_(content.toJson()) {}

ContentBlock::ContentBlock(const AudioContent& content)
    : type_(ContentType::Audio), content_json_(content.toJson()) {}

ContentBlock::ContentBlock(const ResourceContent& content)
    : type_(ContentType::Resource), content_json_(content.toJson()) {}

ContentBlock::ContentBlock(const EmbeddedFileContent& content)
    : type_(ContentType::EmbeddedFile), content_json_(content.toJson()) {}

crow::json::wvalue ContentBlock::toJson() const {
    return content_json_;
}

// ContentBuilder implementation
ContentBlock ContentBuilder::createTextContent(const std::string& text,
                                               const std::optional<std::string>& mime_type) {
    TextContent content;
    content.text = text;
    content.mime_type = mime_type;
    return ContentBlock(content);
}

ContentBlock ContentBuilder::createImageContent(const std::string& base64_data,
                                                const std::string& mime_type) {
    ImageContent content;
    content.data = base64_data;
    content.mime_type = mime_type;
    return ContentBlock(content);
}

ContentBlock ContentBuilder::createAudioContent(const std::string& base64_data,
                                                const std::string& mime_type) {
    AudioContent content;
    content.data = base64_data;
    content.mime_type = mime_type;
    return ContentBlock(content);
}

ContentBlock ContentBuilder::createResourceContent(const std::string& uri,
                                                   const std::string& mime_type,
                                                   const std::optional<std::string>& text) {
    ResourceContent content;
    content.uri = uri;
    content.mime_type = mime_type;
    content.text = text;
    return ContentBlock(content);
}

ContentBlock ContentBuilder::createEmbeddedFileContent(const std::string& uri,
                                                      const std::string& mime_type,
                                                      const std::string& content) {
    EmbeddedFileContent file_content;
    file_content.uri = uri;
    file_content.mime_type = mime_type;
    file_content.text = content;
    return ContentBlock(file_content);
}

// ContentResponse implementation
ContentResponse::ContentResponse() : content_array_(crow::json::wvalue::list()) {}

void ContentResponse::addContent(const ContentBlock& content) {
    content_array_[content_count_++] = content.toJson();
}

void ContentResponse::addText(const std::string& text) {
    addContent(ContentBuilder::createTextContent(text));
}

void ContentResponse::addImage(const std::string& base64_data, const std::string& mime_type) {
    addContent(ContentBuilder::createImageContent(base64_data, mime_type));
}

crow::json::wvalue ContentResponse::toJson() const {
    crow::json::wvalue result;
    result["content"] = crow::json::wvalue(content_array_);
    return result;
}

} // namespace flapi::mcp
