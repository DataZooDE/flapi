#include "embedded_archive_file_provider.hpp"

#include <algorithm>
#include <regex>
#include <stdexcept>
#include <string>

namespace flapi {

namespace {

constexpr const char* kFileScheme = "file://";

std::string NormaliseKey(const std::string& path) {
    std::string s = path;

    // Strip file:// prefix.
    if (s.rfind(kFileScheme, 0) == 0) {
        s.erase(0, std::string(kFileScheme).size());
    }

    // Strip leading "./" repeatedly.
    while (s.size() >= 2 && s[0] == '.' && s[1] == '/') {
        s.erase(0, 2);
    }

    // Strip a single leading slash. Embedded entries are always relative
    // to the bundle root, never absolute.
    if (!s.empty() && s.front() == '/') {
        s.erase(0, 1);
    }

    return s;
}

std::string NormaliseDirectory(const std::string& directory) {
    std::string s = NormaliseKey(directory);
    while (!s.empty() && s.back() == '/') {
        s.pop_back();
    }
    return s;
}

std::regex GlobToRegex(const std::string& pattern) {
    std::string out;
    out.reserve(pattern.size() * 2);
    for (char c : pattern) {
        switch (c) {
            case '*':
                out += ".*";
                break;
            case '?':
                out += '.';
                break;
            case '.': case '+': case '^': case '$':
            case '(': case ')': case '[': case ']':
            case '{': case '}': case '|': case '\\':
                out += '\\';
                out += c;
                break;
            default:
                out += c;
                break;
        }
    }
    return std::regex(out, std::regex::icase);
}

}  // namespace

EmbeddedArchiveFileProvider::EmbeddedArchiveFileProvider(
        std::shared_ptr<const ArchiveEntries> entries)
    : _entries(std::move(entries)) {
    if (_entries == nullptr) {
        throw std::invalid_argument(
            "EmbeddedArchiveFileProvider requires non-null ArchiveEntries");
    }
}

std::string EmbeddedArchiveFileProvider::ReadFile(const std::string& path) {
    const auto key = NormaliseKey(path);
    auto it = _entries->find(key);
    if (it == _entries->end()) {
        throw FileOperationError("Embedded bundle has no entry: " + path);
    }
    const auto& data = it->second;
    return std::string(reinterpret_cast<const char*>(data.data()), data.size());
}

bool EmbeddedArchiveFileProvider::FileExists(const std::string& path) {
    const auto key = NormaliseKey(path);
    return _entries->find(key) != _entries->end();
}

std::vector<std::string> EmbeddedArchiveFileProvider::ListFiles(
        const std::string& directory, const std::string& pattern) {
    const auto dir = NormaliseDirectory(directory);
    const std::string prefix = dir.empty() ? "" : dir + "/";

    std::vector<std::string> result;
    std::regex name_re = GlobToRegex(pattern);

    for (const auto& [name, _data] : *_entries) {
        if (!prefix.empty()) {
            if (name.rfind(prefix, 0) != 0) {
                continue;
            }
        }
        const auto remainder = name.substr(prefix.size());
        // Skip nested subdirectory entries -- ListFiles is non-recursive
        // to mirror LocalFileProvider's directory_iterator semantics.
        if (remainder.find('/') != std::string::npos) {
            continue;
        }
        if (remainder.empty()) {
            continue;
        }
        if (std::regex_match(remainder, name_re)) {
            result.push_back(name);
        }
    }

    std::sort(result.begin(), result.end());
    return result;
}

bool EmbeddedArchiveFileProvider::IsRemotePath(const std::string& /*path*/) const {
    return false;
}

}  // namespace flapi
