#include <catch2/catch_test_macros.hpp>
#include "system_resources.hpp"

#include <filesystem>
#include <fstream>
#include <string>

using namespace flapi;

namespace {

constexpr uint64_t kGiB = 1024ull * 1024ull * 1024ull;
constexpr uint64_t kMiB = 1024ull * 1024ull;

// Write `content` to `<root>/<relpath>`, creating parent dirs.
void writeUnder(const std::filesystem::path& root, const std::string& relpath,
                const std::string& content) {
    std::filesystem::path full = root / relpath;
    std::filesystem::create_directories(full.parent_path());
    std::ofstream file(full);
    file << content;
}

std::filesystem::path makeTempRoot(const std::string& name) {
    std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("flapi_sysres_" + name);
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    return root;
}

} // namespace

TEST_CASE("ParseCgroupV2MemoryMax", "[system_resources]") {
    SECTION("numeric value") {
        auto v = ParseCgroupV2MemoryMax("2147483648\n");
        REQUIRE(v.has_value());
        REQUIRE(*v == 2ull * kGiB);
    }
    SECTION("literal max is unlimited") {
        REQUIRE_FALSE(ParseCgroupV2MemoryMax("max\n").has_value());
    }
    SECTION("garbage is unlimited/ignored") {
        REQUIRE_FALSE(ParseCgroupV2MemoryMax("").has_value());
        REQUIRE_FALSE(ParseCgroupV2MemoryMax("not-a-number").has_value());
    }
}

TEST_CASE("ParseCgroupV1MemoryLimit", "[system_resources]") {
    SECTION("normal value") {
        auto v = ParseCgroupV1MemoryLimit("536870912\n"); // 512 MiB
        REQUIRE(v.has_value());
        REQUIRE(*v == 512ull * kMiB);
    }
    SECTION("PAGE_COUNTER_MAX sentinel is unlimited") {
        REQUIRE_FALSE(ParseCgroupV1MemoryLimit("9223372036854771712").has_value());
    }
    SECTION("0x7FFF... sentinel is unlimited") {
        REQUIRE_FALSE(ParseCgroupV1MemoryLimit("9223372036854775807").has_value());
    }
}

TEST_CASE("ParseCgroupV2CpuMax", "[system_resources]") {
    SECTION("exact multiple") {
        auto v = ParseCgroupV2CpuMax("200000 100000");
        REQUIRE(v.has_value());
        REQUIRE(*v == 2);
    }
    SECTION("rounds up") {
        auto v = ParseCgroupV2CpuMax("150000 100000");
        REQUIRE(v.has_value());
        REQUIRE(*v == 2);
    }
    SECTION("sub-core rounds up to 1") {
        auto v = ParseCgroupV2CpuMax("50000 100000");
        REQUIRE(v.has_value());
        REQUIRE(*v == 1);
    }
    SECTION("max is unlimited") {
        REQUIRE_FALSE(ParseCgroupV2CpuMax("max 100000").has_value());
    }
}

TEST_CASE("ParseCgroupV1Cpu", "[system_resources]") {
    SECTION("two cores") {
        auto v = ParseCgroupV1Cpu("200000", "100000");
        REQUIRE(v.has_value());
        REQUIRE(*v == 2);
    }
    SECTION("quota -1 is unlimited") {
        REQUIRE_FALSE(ParseCgroupV1Cpu("-1", "100000").has_value());
    }
    SECTION("rounds up") {
        auto v = ParseCgroupV1Cpu("250000", "100000");
        REQUIRE(v.has_value());
        REQUIRE(*v == 3);
    }
}

TEST_CASE("DetectAvailableMemoryBytes honors cgroup v2", "[system_resources]") {
    auto root = makeTempRoot("memv2");
    // 512 MiB -- comfortably below any real host, so min(cgroup,host)==cgroup.
    writeUnder(root, "sys/fs/cgroup/memory.max", "536870912\n");

    auto det = DetectAvailableMemoryBytes(root.string());
    REQUIRE(det.source == "cgroup-v2");
    REQUIRE(det.bytes == 512ull * kMiB);

    std::filesystem::remove_all(root);
}

TEST_CASE("DetectAvailableMemoryBytes falls through unlimited cgroup v2", "[system_resources]") {
    auto root = makeTempRoot("memv2max");
    writeUnder(root, "sys/fs/cgroup/memory.max", "max\n");
    // No v1, no meminfo under this root: detector should report a host value or
    // unknown, but never the bogus cgroup value.
    auto det = DetectAvailableMemoryBytes(root.string());
    REQUIRE(det.source != "cgroup-v2");
    std::filesystem::remove_all(root);
}

TEST_CASE("DetectAvailableCpuCount honors cgroup v2", "[system_resources]") {
    auto root = makeTempRoot("cpuv2");
    writeUnder(root, "sys/fs/cgroup/cpu.max", "200000 100000");
    auto det = DetectAvailableCpuCount(root.string());
    REQUIRE(det.source == "cgroup-v2");
    REQUIRE(det.cores == 2);
    std::filesystem::remove_all(root);
}
