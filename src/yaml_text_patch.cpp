#include "yaml_text_patch.hpp"

#include <cctype>
#include <vector>

namespace flapi {

namespace {

using Lines = std::vector<std::string>;

std::string EmitNode(const YAML::Node& node) {
    YAML::Emitter out;
    out << node;
    return std::string(out.c_str());
}

Lines SplitLines(const std::string& text) {
    Lines lines;
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto nl = text.find('\n', start);
        if (nl == std::string::npos) {
            if (start < text.size()) {
                lines.push_back(text.substr(start));
            }
            break;
        }
        lines.push_back(text.substr(start, nl - start));
        start = nl + 1;
    }
    return lines;
}

std::size_t IndentOf(const std::string& line) {
    const auto idx = line.find_first_not_of(' ');
    return idx == std::string::npos ? line.size() : idx;
}

bool IsBlankOrComment(const std::string& line) {
    const auto idx = line.find_first_not_of(" \t");
    return idx == std::string::npos || line[idx] == '#';
}

// `{key: value}` emitted as block YAML, every line indented by `indent` spaces.
Lines EmitEntry(const std::string& key, const YAML::Node& value, std::size_t indent) {
    YAML::Node single(YAML::NodeType::Map);
    single[key] = YAML::Clone(value);
    Lines lines = SplitLines(EmitNode(single));
    for (auto& line : lines) {
        line.insert(0, indent, ' ');
    }
    return lines;
}

struct Entry {
    std::string key;
    std::size_t first = 0;   // the key line
    std::size_t last = 0;    // last content line (comments/blanks after it are not part of the entry)
};

// The `key:` entries of the block map occupying lines [begin, end) at `indent`.
bool ScanEntries(const Lines& lines, std::size_t begin, std::size_t end, std::size_t indent,
                 std::vector<Entry>& entries) {
    for (std::size_t i = begin; i < end; ++i) {
        const std::string& line = lines[i];
        if (IsBlankOrComment(line)) {
            continue;
        }
        const std::size_t ind = IndentOf(line);
        if (ind < indent || line[ind] == '\t') {
            return false;
        }
        if (ind > indent) {
            if (entries.empty()) {
                return false;
            }
            entries.back().last = i;
            continue;
        }
        // `key:` entry sequence at the key's own indent (`a:\n- x`) belongs to the key.
        if (line[ind] == '-' && (ind + 1 == line.size() || line[ind + 1] == ' ')) {
            if (entries.empty()) {
                return false;
            }
            entries.back().last = i;
            continue;
        }
        std::size_t j = ind;
        while (j < line.size() &&
               (std::isalnum(static_cast<unsigned char>(line[j])) || line[j] == '_' || line[j] == '-' ||
                line[j] == '.')) {
            ++j;
        }
        if (j == ind || j >= line.size() || line[j] != ':' || (j + 1 < line.size() && line[j + 1] != ' ')) {
            return false;   // quoted/complex key, document marker, flow collection, ...
        }
        entries.push_back(Entry{line.substr(ind, j - ind), i, i});
    }
    return !entries.empty();
}

std::optional<Lines> PatchRange(const Lines& lines, std::size_t begin, std::size_t end, std::size_t indent,
                                const YAML::Node& base, const YAML::Node& merged) {
    if (!base.IsMap() || !merged.IsMap()) {
        return std::nullopt;
    }
    std::vector<Entry> entries;
    if (!ScanEntries(lines, begin, end, indent, entries)) {
        return std::nullopt;
    }

    Lines out;
    std::size_t pos = begin;
    std::vector<std::string> seen;
    for (const auto& entry : entries) {
        out.insert(out.end(), lines.begin() + static_cast<std::ptrdiff_t>(pos),
                   lines.begin() + static_cast<std::ptrdiff_t>(entry.first));
        pos = entry.last + 1;
        seen.push_back(entry.key);

        const YAML::Node new_value = merged[entry.key];
        if (!new_value.IsDefined() || !base[entry.key].IsDefined()) {
            return std::nullopt;   // a key the update dropped: not a patch
        }
        const YAML::Node old_value = base[entry.key];
        auto copy_entry = [&]() {
            out.insert(out.end(), lines.begin() + static_cast<std::ptrdiff_t>(entry.first),
                       lines.begin() + static_cast<std::ptrdiff_t>(entry.last) + 1);
        };
        if (EmitNode(old_value) == EmitNode(new_value)) {
            copy_entry();
            continue;
        }

        const std::string& key_line = lines[entry.first];
        const std::string rest = key_line.substr(indent + entry.key.size() + 1);
        const auto rest_start = rest.find_first_not_of(' ');
        const bool block_value = rest_start == std::string::npos || rest[rest_start] == '#';

        // A map whose entries changed: patch inside it, keeping its other comments.
        if (old_value.IsMap() && new_value.IsMap() && old_value.size() > 0 && block_value &&
            entry.last > entry.first) {
            std::size_t child_indent = 0;
            for (std::size_t i = entry.first + 1; i <= entry.last; ++i) {
                if (!IsBlankOrComment(lines[i])) {
                    child_indent = IndentOf(lines[i]);
                    break;
                }
            }
            if (child_indent > indent) {
                if (auto inner = PatchRange(lines, entry.first + 1, entry.last + 1, child_indent, old_value,
                                            new_value)) {
                    out.push_back(key_line);
                    out.insert(out.end(), inner->begin(), inner->end());
                    continue;
                }
            }
        }

        // A single-line plain scalar: new value, same inline comment.
        if (old_value.IsScalar() && new_value.IsScalar() && entry.first == entry.last && !block_value) {
            const char lead = rest[rest_start];
            const bool plain = lead != '"' && lead != '\'' && lead != '|' && lead != '>' && lead != '&' &&
                               lead != '*' && lead != '!';
            const auto emitted = EmitEntry(entry.key, new_value, indent);
            if (plain && emitted.size() == 1) {
                const auto comment_at = rest.find(" #");
                std::string patched = emitted.front();
                if (comment_at != std::string::npos) {
                    // keep the author's spacing before the comment
                    std::size_t space_start = comment_at;
                    while (space_start > 0 && rest[space_start - 1] == ' ') {
                        --space_start;
                    }
                    patched += rest.substr(space_start);
                }
                out.push_back(patched);
                continue;
            }
        }

        const auto replaced = EmitEntry(entry.key, new_value, indent);
        out.insert(out.end(), replaced.begin(), replaced.end());
    }
    const std::size_t after_last_entry = out.size();
    out.insert(out.end(), lines.begin() + static_cast<std::ptrdiff_t>(pos),
               lines.begin() + static_cast<std::ptrdiff_t>(end));

    // Keys the text does not have yet go right after the last entry's content.
    Lines additions;
    for (const auto& kv : merged) {
        const std::string key = kv.first.as<std::string>();
        bool known = false;
        for (const auto& s : seen) {
            known = known || s == key;
        }
        if (!known) {
            const auto added = EmitEntry(key, kv.second, indent);
            additions.insert(additions.end(), added.begin(), added.end());
        }
    }
    if (!additions.empty()) {
        // Right after the last entry's own content, ahead of the comments and
        // blank lines that follow it (they introduce whatever comes next).
        out.insert(out.begin() + static_cast<std::ptrdiff_t>(after_last_entry), additions.begin(),
                   additions.end());
    }
    return out;
}

}  // namespace

std::optional<std::string> PatchYamlPreservingComments(const std::string& previous, const YAML::Node& base,
                                                       const YAML::Node& merged) {
    if (previous.find('\r') != std::string::npos || !base.IsMap() || !merged.IsMap()) {
        return std::nullopt;
    }
    const Lines lines = SplitLines(previous);
    try {
        auto patched = PatchRange(lines, 0, lines.size(), 0, base, merged);
        if (!patched) {
            return std::nullopt;
        }
        std::string text;
        for (const auto& line : *patched) {
            text += line;
            text += '\n';
        }
        // Never hand back text that does not mean what the caller asked for.
        if (EmitNode(YAML::Load(text)) != EmitNode(merged)) {
            return std::nullopt;
        }
        return text;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

}  // namespace flapi
