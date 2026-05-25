#include <catch2/catch_test_macros.hpp>
#include "macho_bundle.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace flapi;

namespace {

// Helpers to build a synthetic 64-bit little-endian Mach-O binary
// with a single LC_SEGMENT_64 containing one named section. This
// lets us unit-test the parser on any platform.

constexpr std::uint32_t kMachOMagic64 = 0xFEEDFACFu;
constexpr std::uint32_t kLcSegment64  = 0x19;
constexpr std::size_t   kHeader64Size = 32;
constexpr std::size_t   kSeg64Size    = 72;
constexpr std::size_t   kSect64Size   = 80;

void WriteU32LE(std::vector<std::uint8_t>& buf, std::size_t off, std::uint32_t v) {
    buf[off]     = static_cast<std::uint8_t>(v & 0xff);
    buf[off + 1] = static_cast<std::uint8_t>((v >> 8) & 0xff);
    buf[off + 2] = static_cast<std::uint8_t>((v >> 16) & 0xff);
    buf[off + 3] = static_cast<std::uint8_t>((v >> 24) & 0xff);
}

void WriteU64LE(std::vector<std::uint8_t>& buf, std::size_t off, std::uint64_t v) {
    WriteU32LE(buf, off,     static_cast<std::uint32_t>(v & 0xffffffffu));
    WriteU32LE(buf, off + 4, static_cast<std::uint32_t>((v >> 32) & 0xffffffffu));
}

void WriteName(std::vector<std::uint8_t>& buf, std::size_t off,
               const std::string& name) {
    for (std::size_t i = 0; i < 16; ++i) {
        buf[off + i] = (i < name.size())
            ? static_cast<std::uint8_t>(name[i])
            : 0u;
    }
}

struct SectionSpec {
    std::string segname;
    std::string sectname;
    std::uint64_t file_offset;
    std::uint64_t size;
};

std::vector<std::uint8_t> BuildMachO64(const std::vector<SectionSpec>& sections) {
    // One LC_SEGMENT_64 per section spec. Real Mach-O groups multiple
    // sections under one segment, but for the parser-under-test that
    // doesn't matter -- each (segname, sectname) pair is checked.
    const std::uint32_t ncmds = static_cast<std::uint32_t>(sections.size());
    const std::uint32_t sizeofcmds = ncmds * static_cast<std::uint32_t>(kSeg64Size + kSect64Size);

    const std::size_t total = kHeader64Size + sizeofcmds;
    std::vector<std::uint8_t> buf(total, 0);

    // mach_header_64
    WriteU32LE(buf, 0, kMachOMagic64);   // magic
    WriteU32LE(buf, 4, 0x0100000Cu);     // cputype = CPU_TYPE_ARM64 (arbitrary)
    WriteU32LE(buf, 8, 0);                // cpusubtype
    WriteU32LE(buf, 12, 2);               // filetype = MH_EXECUTE
    WriteU32LE(buf, 16, ncmds);
    WriteU32LE(buf, 20, sizeofcmds);
    WriteU32LE(buf, 24, 0);               // flags
    WriteU32LE(buf, 28, 0);               // reserved

    std::size_t cursor = kHeader64Size;
    for (const auto& s : sections) {
        // segment_command_64
        WriteU32LE(buf, cursor + 0, kLcSegment64);
        WriteU32LE(buf, cursor + 4, static_cast<std::uint32_t>(kSeg64Size + kSect64Size));
        WriteName(buf, cursor + 8, s.segname);  // segname[16]
        WriteU64LE(buf, cursor + 24, 0);         // vmaddr
        WriteU64LE(buf, cursor + 32, 0);         // vmsize
        WriteU64LE(buf, cursor + 40, 0);         // fileoff
        WriteU64LE(buf, cursor + 48, 0);         // filesize
        WriteU32LE(buf, cursor + 56, 0);         // maxprot
        WriteU32LE(buf, cursor + 60, 0);         // initprot
        WriteU32LE(buf, cursor + 64, 1);         // nsects
        WriteU32LE(buf, cursor + 68, 0);         // flags

        // section_64 (immediately after the segment_command_64)
        const std::size_t sect_cursor = cursor + kSeg64Size;
        WriteName(buf, sect_cursor + 0,  s.sectname);  // sectname[16]
        WriteName(buf, sect_cursor + 16, s.segname);   // segname[16]
        WriteU64LE(buf, sect_cursor + 32, 0);          // addr
        WriteU64LE(buf, sect_cursor + 40, s.size);     // size
        WriteU32LE(buf, sect_cursor + 48, static_cast<std::uint32_t>(s.file_offset));
        WriteU32LE(buf, sect_cursor + 52, 0);          // align
        WriteU32LE(buf, sect_cursor + 56, 0);          // reloff
        WriteU32LE(buf, sect_cursor + 60, 0);          // nreloc
        WriteU32LE(buf, sect_cursor + 64, 0);          // flags
        // reserved1/2/3 already zero

        cursor += kSeg64Size + kSect64Size;
    }

    return buf;
}

// --- Fat / universal helpers (issue #66) -------------------------------------

constexpr std::uint32_t kFatMagic     = 0xCAFEBABEu;
constexpr std::size_t   kFatHeaderSize = 8;
constexpr std::size_t   kFatArchSize   = 20;  // 32-bit fat_arch
constexpr std::uint32_t kCpuTypeX86_64 = 0x01000007u;
constexpr std::uint32_t kCpuTypeArm64  = 0x0100000Cu;
constexpr std::uint32_t kCpuTypePPC    = 0x00000012u;

void WriteU32BE(std::vector<std::uint8_t>& buf, std::size_t off, std::uint32_t v) {
    buf[off]     = static_cast<std::uint8_t>((v >> 24) & 0xff);
    buf[off + 1] = static_cast<std::uint8_t>((v >> 16) & 0xff);
    buf[off + 2] = static_cast<std::uint8_t>((v >> 8) & 0xff);
    buf[off + 3] = static_cast<std::uint8_t>(v & 0xff);
}

struct FatSliceSpec {
    std::uint32_t cputype;
    std::vector<std::uint8_t> bytes;  // thin Mach-O contents for this slice
};

// Wraps thin Mach-O bytes with a 32-bit fat header. Slices are placed
// at 4 KiB-aligned offsets after the fat_arch table; the wrapper
// pads with zeros between slices so absolute offsets are stable and
// reproducible across runs.
std::vector<std::uint8_t> BuildFatMachO(const std::vector<FatSliceSpec>& slices) {
    constexpr std::uint64_t kAlign = 4096;
    auto align_up = [](std::uint64_t v, std::uint64_t a) {
        return (v + a - 1) & ~(a - 1);
    };

    const std::uint32_t nfat_arch = static_cast<std::uint32_t>(slices.size());
    std::vector<std::uint64_t> slice_offsets;
    slice_offsets.reserve(slices.size());

    std::uint64_t cursor = align_up(kFatHeaderSize + nfat_arch * kFatArchSize, kAlign);
    for (const auto& s : slices) {
        slice_offsets.push_back(cursor);
        cursor = align_up(cursor + s.bytes.size(), kAlign);
    }
    const std::uint64_t total = cursor;

    std::vector<std::uint8_t> buf(static_cast<std::size_t>(total), 0);
    WriteU32BE(buf, 0, kFatMagic);
    WriteU32BE(buf, 4, nfat_arch);

    for (std::uint32_t i = 0; i < nfat_arch; ++i) {
        const std::size_t arch_off =
            kFatHeaderSize + static_cast<std::size_t>(i) * kFatArchSize;
        WriteU32BE(buf, arch_off + 0,  slices[i].cputype);
        WriteU32BE(buf, arch_off + 4,  0);  // cpusubtype
        WriteU32BE(buf, arch_off + 8,  static_cast<std::uint32_t>(slice_offsets[i]));
        WriteU32BE(buf, arch_off + 12, static_cast<std::uint32_t>(slices[i].bytes.size()));
        WriteU32BE(buf, arch_off + 16, 12);  // align (2^12 == 4 KiB)
    }
    for (std::uint32_t i = 0; i < nfat_arch; ++i) {
        std::memcpy(buf.data() + slice_offsets[i],
                    slices[i].bytes.data(),
                    slices[i].bytes.size());
    }
    return buf;
}

}  // namespace

TEST_CASE("IsMachOMagic recognises 64-bit and fat magics", "[macho_bundle]") {
    std::uint8_t mh64[4] = {0xCF, 0xFA, 0xED, 0xFE};  // little-endian 0xFEEDFACF
    std::uint8_t fat[4]  = {0xBE, 0xBA, 0xFE, 0xCA};
    std::uint8_t elf[4]  = {0x7F, 'E', 'L', 'F'};

    REQUIRE(IsMachOMagic(mh64));
    REQUIRE(IsMachOMagic(fat));
    REQUIRE_FALSE(IsMachOMagic(elf));
}

TEST_CASE("LocateFlapiSection finds __FLAPI/__bundle in synthetic Mach-O",
          "[macho_bundle]") {
    auto buf = BuildMachO64({
        {"__TEXT",   "__text",   0x1000, 0x4000},
        {"__FLAPI",  "__bundle", 0x100000, 16u * 1024u * 1024u},
        {"__DATA",   "__data",   0x200000, 0x8000},
    });

    auto loc = LocateFlapiSectionInBuffer(buf);
    REQUIRE(loc.has_value());
    REQUIRE(loc->file_offset == 0x100000);
    REQUIRE(loc->size == 16u * 1024u * 1024u);
}

TEST_CASE("LocateFlapiSection returns nullopt when section is absent",
          "[macho_bundle]") {
    auto buf = BuildMachO64({
        {"__TEXT", "__text", 0x1000, 0x4000},
        {"__DATA", "__data", 0x10000, 0x2000},
    });
    REQUIRE_FALSE(LocateFlapiSectionInBuffer(buf).has_value());
}

TEST_CASE("LocateFlapiSection rejects non-Mach-O input", "[macho_bundle]") {
    std::vector<std::uint8_t> not_macho(256, 0);
    not_macho[0] = 0x7F;  // \x7FELF -- not Mach-O
    not_macho[1] = 'E';
    not_macho[2] = 'L';
    not_macho[3] = 'F';
    REQUIRE_FALSE(LocateFlapiSectionInBuffer(not_macho).has_value());
}

TEST_CASE("LocateFlapiSection ignores a section that matches by name in the wrong segment",
          "[macho_bundle]") {
    // __bundle in some other segment must NOT match.
    auto buf = BuildMachO64({
        {"__OTHER", "__bundle", 0x1000, 0x100},
    });
    REQUIRE_FALSE(LocateFlapiSectionInBuffer(buf).has_value());
}

TEST_CASE("LocateFlapiSection handles short buffer without crashing",
          "[macho_bundle]") {
    // mach_header_64 is 32 bytes; anything shorter must be rejected.
    std::vector<std::uint8_t> tiny(16, 0);
    REQUIRE_FALSE(LocateFlapiSectionInBuffer(tiny).has_value());

    // Header present but truncated load commands.
    auto buf = BuildMachO64({{"__FLAPI", "__bundle", 0x1000, 0x100}});
    buf.resize(buf.size() - 32);  // chop the last section_64
    REQUIRE_FALSE(LocateFlapiSectionInBuffer(buf).has_value());
}

// --- Fat / universal binary cases (issue #66) --------------------------------

TEST_CASE("LocateFlapiSection finds __FLAPI/__bundle in a single-slice fat Mach-O",
          "[macho_bundle][fat]") {
    constexpr std::uint64_t kSectOffWithinSlice = 0x100000;
    auto slice_bytes = BuildMachO64({
        {"__TEXT",  "__text",   0x1000, 0x4000},
        {"__FLAPI", "__bundle", kSectOffWithinSlice, 16u * 1024u * 1024u},
    });

    auto fat = BuildFatMachO({
        {kCpuTypeArm64, slice_bytes},
    });

    auto loc = LocateFlapiSectionInBuffer(fat);
    REQUIRE(loc.has_value());

    // The section's offset within the slice is recorded as
    // kSectOffWithinSlice; the slice itself sits at the first
    // 4 KiB-aligned offset past the fat header + 1 fat_arch entry,
    // which is 4096. Absolute file_offset must add the two.
    constexpr std::uint64_t kSliceFatOffset = 4096;
    REQUIRE(loc->file_offset == kSliceFatOffset + kSectOffWithinSlice);
    // Width unchanged by the fat wrapper.
    REQUIRE(loc->size == 16ull * 1024ull * 1024ull);
}

TEST_CASE("LocateFlapiSection picks the host-arch slice in a two-slice fat Mach-O",
          "[macho_bundle][fat]") {
    constexpr std::uint64_t kArm64SectOff   = 0x100000;
    constexpr std::uint64_t kX86_64SectOff  = 0x200000;
    constexpr std::uint64_t kSliceSize      = 16u * 1024u * 1024u;

    auto arm64_slice = BuildMachO64({
        {"__TEXT",  "__text",   0x1000, 0x4000},
        {"__FLAPI", "__bundle", kArm64SectOff, kSliceSize},
    });
    auto x86_64_slice = BuildMachO64({
        {"__TEXT",  "__text",   0x1000, 0x4000},
        {"__FLAPI", "__bundle", kX86_64SectOff, kSliceSize},
    });

    auto fat = BuildFatMachO({
        {kCpuTypeArm64,  arm64_slice},
        {kCpuTypeX86_64, x86_64_slice},
    });

    auto loc = LocateFlapiSectionInBuffer(fat);
    REQUIRE(loc.has_value());

    // First slice is 4 KiB-aligned past the fat header + 2 fat_arch
    // entries (8 + 40 = 48 bytes) = 4096. Second slice is placed at
    // the next 4 KiB-aligned offset after the first slice's bytes;
    // BuildMachO64 emits 32 + 2*(72+80) = 336 bytes for a 2-section
    // thin Mach-O, so the second slice lands at align_up(4096+336,
    // 4096) = 8192.
    [[maybe_unused]] constexpr std::uint64_t kArm64SliceFatOffset  = 4096;
    [[maybe_unused]] constexpr std::uint64_t kX86_64SliceFatOffset = 8192;

#if defined(__aarch64__) || defined(__arm64__)
    constexpr std::uint64_t expected_abs = kArm64SliceFatOffset + kArm64SectOff;
#elif defined(__x86_64__) || defined(_M_X64)
    constexpr std::uint64_t expected_abs = kX86_64SliceFatOffset + kX86_64SectOff;
#else
    constexpr std::uint64_t expected_abs = kArm64SliceFatOffset + kArm64SectOff;
#endif
    REQUIRE(loc->file_offset == expected_abs);
    REQUIRE(loc->size == kSliceSize);
}

TEST_CASE("LocateFlapiSection falls back to the first slice when no arch matches",
          "[macho_bundle][fat]") {
    constexpr std::uint64_t kFirstSectOff = 0x100000;
    auto first_slice = BuildMachO64({
        {"__FLAPI", "__bundle", kFirstSectOff, 0x80000},
    });
    auto second_slice = BuildMachO64({
        {"__FLAPI", "__bundle", 0x200000, 0x80000},
    });
    // Two PPC slices: never matches kPreferredCpuType on the platforms
    // we compile for, so the parser must take the first one.
    auto fat = BuildFatMachO({
        {kCpuTypePPC, first_slice},
        {kCpuTypePPC, second_slice},
    });

    auto loc = LocateFlapiSectionInBuffer(fat);
    REQUIRE(loc.has_value());
    // First slice's fat offset = 4 KiB-aligned past the 8-byte fat
    // header + 2 * 20-byte fat_arch records (= 48) = 4096. The
    // section is recorded at kFirstSectOff inside that slice, so the
    // absolute offset is the sum -- proving the parser picked the
    // first slice as the fallback rather than peeking at the second.
    constexpr std::uint64_t kFirstSliceFatOffset = 4096;
    REQUIRE(loc->file_offset == kFirstSliceFatOffset + kFirstSectOff);
    REQUIRE(loc->size == 0x80000);
}

TEST_CASE("OverwriteFlapiSection round-trips inside a fat slice",
          "[macho_bundle][fat]") {
    // Pick a section large enough to hold our payload, with at least
    // some intra-slice offset so the absolute-offset path is the only
    // way the seek lands correctly.
    constexpr std::uint64_t kSectOff  = 0x10000;
    constexpr std::uint64_t kSectSize = 4096;
    auto slice_bytes = BuildMachO64({
        {"__FLAPI", "__bundle", kSectOff, kSectSize},
    });
    // Extend the slice byte vector so the section's declared extent
    // (kSectOff + kSectSize) actually exists inside the slice's
    // storage when we ship it into the fat wrapper.
    slice_bytes.resize(static_cast<std::size_t>(kSectOff + kSectSize), 0);

    auto fat = BuildFatMachO({
        {kCpuTypeArm64, slice_bytes},
    });

    // Write the fat fixture to a temp file so we can exercise the
    // file-on-disk path of OverwriteFlapiSection + LocateFlapiSection.
    auto tmp = std::filesystem::temp_directory_path() /
               ("macho_fat_roundtrip_" +
                std::to_string(reinterpret_cast<std::uintptr_t>(&fat)) + ".bin");
    {
        std::ofstream f(tmp, std::ios::binary);
        f.write(reinterpret_cast<const char*>(fat.data()),
                static_cast<std::streamsize>(fat.size()));
    }

    auto loc = LocateFlapiSection(tmp);
    REQUIRE(loc.has_value());

    const std::vector<std::uint8_t> payload = {'f', 'l', 'a', 'p', 'i', '!', '!'};
    OverwriteFlapiSection(tmp, *loc, payload);

    // Verify the bytes landed at the absolute offset the locator
    // returned -- proves the slice base was applied before the seek.
    std::ifstream in(tmp, std::ios::binary);
    in.seekg(static_cast<std::streamoff>(loc->file_offset), std::ios::beg);
    std::vector<std::uint8_t> got(payload.size(), 0);
    in.read(reinterpret_cast<char*>(got.data()),
            static_cast<std::streamsize>(got.size()));
    REQUIRE(got == payload);

    // And the slack right after the payload was zero-padded, so a
    // re-locate sees the same section unchanged.
    auto loc2 = LocateFlapiSection(tmp);
    REQUIRE(loc2.has_value());
    REQUIRE(loc2->file_offset == loc->file_offset);
    REQUIRE(loc2->size == loc->size);

    std::error_code ec;
    std::filesystem::remove(tmp, ec);
}

TEST_CASE("CodesignBinary is a benign no-op on non-Darwin builds",
          "[macho_bundle]") {
#ifndef __APPLE__
    // Path doesn't need to exist; the function shouldn't even look at it.
    auto r = CodesignBinary("/nope/does/not/exist");
    REQUIRE(r.exit_code == 0);
    REQUIRE(r.identity == "n/a");
#else
    SUCCEED("skipped: this test only runs on non-Darwin builds");
#endif
}
