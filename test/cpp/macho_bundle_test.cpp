#include <catch2/catch_test_macros.hpp>
#include "macho_bundle.hpp"

#include <array>
#include <cstdint>
#include <cstring>
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
