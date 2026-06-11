#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace flapi {

// Result of probing the memory available to this process. `bytes` is the
// detected ceiling (cgroup limit or host physical memory, whichever is
// smaller); `source` names where the value came from so it can be logged.
// On failure `bytes == 0` and `source == "unknown"`.
struct MemoryDetection {
    uint64_t bytes = 0;
    std::string source = "unknown"; // cgroup-v2|cgroup-v1|meminfo|sysconf|sysctl|win-global|unknown
};

// Result of probing the CPU quota available to this process. `cores` is
// clamped to at least 1 when a positive value is detected.
struct CpuDetection {
    uint32_t cores = 0;
    std::string source = "unknown"; // cgroup-v2|cgroup-v1|hardware|unknown
};

// --- Pure parsers (no filesystem; unit-tested directly) ------------------

// cgroup v2 `memory.max`. A literal "max" means unlimited -> nullopt.
std::optional<uint64_t> ParseCgroupV2MemoryMax(const std::string& content);

// cgroup v1 `memory.limit_in_bytes`. Absurdly large sentinel values
// (PAGE_COUNTER_MAX, 0x7FFFFFFFFFFFF000, etc.) mean unlimited -> nullopt.
std::optional<uint64_t> ParseCgroupV1MemoryLimit(const std::string& content);

// cgroup v2 `cpu.max` == "<quota> <period>". "max ..." means unlimited ->
// nullopt. Otherwise effective cores = ceil(quota / period), clamped >= 1.
std::optional<uint32_t> ParseCgroupV2CpuMax(const std::string& content);

// cgroup v1 `cpu.cfs_quota_us` / `cpu.cfs_period_us`. quota -1 means
// unlimited -> nullopt. Otherwise ceil(quota / period), clamped >= 1.
std::optional<uint32_t> ParseCgroupV1Cpu(const std::string& quota, const std::string& period);

// --- Composing detectors (probe filesystem; `root` is injectable for tests) ---

MemoryDetection DetectAvailableMemoryBytes(const std::string& root = "/");
CpuDetection DetectAvailableCpuCount(const std::string& root = "/");

} // namespace flapi
