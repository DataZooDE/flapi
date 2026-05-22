#include "pack.hpp"

#include "bundle_locator.hpp"
#include "macho_bundle.hpp"
#include "selfpath.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <ostream>
#include <regex>
#include <vector>

namespace flapi {

namespace {

const std::array<std::regex, 4>& SecretPatterns() {
    static const std::array<std::regex, 4> patterns{
        std::regex(R"((^|/)[^/]*\.env$)"),
        std::regex(R"((^|/)secrets/)"),
        std::regex(R"((^|/)[^/]*\.pem$)"),
        std::regex(R"((^|/)[^/]*\.key$)"),
    };
    return patterns;
}

std::int64_t ResolveSourceDateEpoch(const std::optional<std::int64_t>& explicit_val) {
    if (explicit_val.has_value()) {
        return *explicit_val;
    }
    if (const char* env = std::getenv("SOURCE_DATE_EPOCH")) {
        try {
            return static_cast<std::int64_t>(std::stoll(env));
        } catch (...) {
            // fall through to default
        }
    }
    return 0;
}

std::uint64_t HostByteCount(const std::filesystem::path& self_path) {
    auto file_size = std::filesystem::file_size(self_path);
    if (auto loc = LocateBundle(self_path); loc.has_value()) {
        return loc->offset;
    }
    return static_cast<std::uint64_t>(file_size);
}

void CopyHostPrefix(const std::filesystem::path& src,
                    const std::filesystem::path& dst,
                    std::uint64_t bytes_to_copy) {
    std::ifstream in(src, std::ios::binary);
    if (!in) {
        throw PackError("cannot open self-binary: " + src.string());
    }

    std::ofstream out(dst, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw PackError("cannot create output binary: " + dst.string());
    }

    constexpr std::size_t kChunk = 64 * 1024;
    std::vector<char> buf(kChunk);
    std::uint64_t remaining = bytes_to_copy;
    while (remaining > 0) {
        const auto to_read = static_cast<std::streamsize>(
            std::min<std::uint64_t>(remaining, kChunk));
        in.read(buf.data(), to_read);
        const auto n = in.gcount();
        if (n <= 0) {
            throw PackError("short read from self-binary: " + src.string());
        }
        out.write(buf.data(), n);
        remaining -= static_cast<std::uint64_t>(n);
    }
}

void AppendArchive(const std::filesystem::path& path,
                   const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out) {
        throw PackError("cannot append archive to " + path.string());
    }
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

ArchiveEntries CollectEntries(const std::filesystem::path& in_dir, bool allow_secrets) {
    if (!std::filesystem::exists(in_dir)) {
        throw PackError("input directory does not exist: " + in_dir.string());
    }
    if (!std::filesystem::is_directory(in_dir)) {
        throw PackError("input is not a directory: " + in_dir.string());
    }

    ArchiveEntries entries;
    for (const auto& dir_entry :
         std::filesystem::recursive_directory_iterator(in_dir)) {
        if (!dir_entry.is_regular_file()) {
            continue;
        }
        const auto rel = std::filesystem::relative(dir_entry.path(), in_dir);
        const std::string rel_str = rel.generic_string();  // always forward slashes

        if (!allow_secrets && IsSecretExcluded(rel_str)) {
            throw PackError(
                "refusing to bundle secret-looking file: " + rel_str +
                " (override with --allow-secrets)");
        }

        std::ifstream f(dir_entry.path(), std::ios::binary);
        if (!f) {
            throw PackError("cannot read input file: " + dir_entry.path().string());
        }
        std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(f)),
                                       std::istreambuf_iterator<char>());
        entries.emplace(rel_str, std::move(data));
    }
    return entries;
}

void CopyExecutableBits(const std::filesystem::path& src,
                        const std::filesystem::path& dst) {
    std::error_code ec;
    const auto perms = std::filesystem::status(src, ec).permissions();
    if (ec) {
        return;
    }
    std::filesystem::permissions(dst, perms, ec);
    // best-effort -- ignore errors (filesystem may not support perms)
}

}  // namespace

bool IsSecretExcluded(const std::string& rel_path) {
    for (const auto& re : SecretPatterns()) {
        if (std::regex_search(rel_path, re)) {
            return true;
        }
    }
    return false;
}

PackResult Pack(const std::filesystem::path& in_dir,
                const std::filesystem::path& out_path,
                const PackOptions& options) {
    // Collect (and validate against the secret deny list) BEFORE doing any
    // I/O on the output. Otherwise a rejected `.env` leaves a multi-GB
    // half-written binary sitting in the user's tmp dir.
    auto entries = CollectEntries(in_dir, options.allow_secrets);

    ArchiveWriteOptions write_opts;
    write_opts.source_date_epoch = ResolveSourceDateEpoch(options.source_date_epoch);
    const auto archive = WriteArchive(entries, write_opts);

    const auto host_path = options.host_binary_override.value_or(GetSelfPath());

    PackResult r;
    r.output = out_path;
    r.entry_count = entries.size();
    r.archive_size = archive.size();

    // Try the reserved-segment path on macOS (kReservedSegment mode).
    // The Mach-O parser lives on every platform, so we can probe for
    // the section anywhere -- but in practice only macOS builds emit
    // one. If the host has no section, fall through to the append path.
    bool used_section_path = false;
    if (options.macos_mode == MacOSPackMode::kReservedSegment) {
        if (auto sect = LocateFlapiSection(host_path); sect.has_value()) {
            // Copy the entire host (including the placeholder section),
            // then overwrite the section bytes in place.
            std::error_code ec;
            std::filesystem::copy_file(
                host_path, out_path,
                std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) {
                throw PackError("cannot copy host binary to " + out_path.string());
            }
            CopyExecutableBits(host_path, out_path);
            OverwriteFlapiSection(out_path, *sect, archive);
            used_section_path = true;
        }
    }

    if (!used_section_path) {
        const auto host_bytes = HostByteCount(host_path);
        CopyHostPrefix(host_path, out_path, host_bytes);
        CopyExecutableBits(host_path, out_path);
        AppendArchive(out_path, archive);
    }

    // Re-sign on Darwin. CodesignBinary is a benign no-op on other
    // platforms so we don't need a platform guard here. Note: a
    // codesign failure is reported to the caller via PackError; the
    // append path on macOS still benefits from an ad-hoc re-sign
    // because the trailing-data invalidates the original signature.
    if (options.codesign) {
        auto cs = CodesignBinary(out_path);
        if (cs.exit_code != 0) {
            // Don't crash a Linux build over codesign output. On macOS
            // a non-zero exit means we couldn't make the binary
            // launchable -- surface it.
#ifdef __APPLE__
            throw PackError("codesign failed (exit " +
                             std::to_string(cs.exit_code) + "): " +
                             cs.stderr_tail);
#else
            (void)cs;  // unreachable in practice (we always set exit_code = 0)
#endif
        }
    }

    return r;
}

namespace {

std::vector<std::uint8_t> ReadBundleBytes(const std::filesystem::path& binary,
                                          const BundleLocation& loc) {
    std::ifstream f(binary, std::ios::binary);
    if (!f) {
        throw PackError("cannot open " + binary.string());
    }
    f.seekg(static_cast<std::streamoff>(loc.offset), std::ios::beg);

    std::vector<std::uint8_t> bytes(loc.size);
    f.read(reinterpret_cast<char*>(bytes.data()),
           static_cast<std::streamsize>(loc.size));
    if (static_cast<std::uint64_t>(f.gcount()) != loc.size) {
        throw PackError("short bundle read from " + binary.string());
    }
    return bytes;
}

}  // namespace

int PrintBundleInfoForPath(const std::filesystem::path& binary, std::ostream& os) {
    os << "Binary: " << binary.string() << "\n";

    auto loc = LocateBundle(binary);
    if (!loc.has_value()) {
        os << "Bundle: none (filesystem mode)\n";
        return 1;
    }

    os << "Bundle offset: " << loc->offset << "\n";
    os << "Bundle size:   " << loc->size << " bytes\n";

    auto bytes = ReadBundleBytes(binary, *loc);
    auto entries = ReadArchive(bytes);

    os << "Entries (" << entries.size() << "):\n";
    for (const auto& [name, data] : entries) {
        os << "  " << name << " (" << data.size() << " bytes)\n";
    }
    return 0;
}

int PrintBundleInfo(std::ostream& os) {
    return PrintBundleInfoForPath(GetSelfPath(), os);
}

UnpackResult UnpackBundleFromPath(const std::filesystem::path& binary,
                                  const std::filesystem::path& dst_dir) {
    auto loc = LocateBundle(binary);
    if (!loc.has_value()) {
        throw PackError("no bundle present in " + binary.string());
    }

    auto bytes = ReadBundleBytes(binary, *loc);
    auto entries = ReadArchive(bytes);

    std::error_code ec;
    std::filesystem::create_directories(dst_dir, ec);
    if (ec) {
        throw PackError("cannot create unpack directory: " + dst_dir.string());
    }

    UnpackResult r;
    for (const auto& [name, data] : entries) {
        auto out_path = dst_dir / name;
        std::filesystem::create_directories(out_path.parent_path(), ec);

        std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw PackError("cannot write unpacked file: " + out_path.string());
        }
        out.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
        ++r.entries_written;
    }
    return r;
}

UnpackResult UnpackBundle(const std::filesystem::path& dst_dir) {
    return UnpackBundleFromPath(GetSelfPath(), dst_dir);
}

}  // namespace flapi
