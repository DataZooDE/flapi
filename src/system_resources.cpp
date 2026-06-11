#include "system_resources.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <thread>

#if defined(__linux__)
#include <unistd.h>
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/types.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace flapi {

namespace {

std::string trim(const std::string& s) {
    size_t begin = 0;
    size_t end = s.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(s[begin]))) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return s.substr(begin, end - begin);
}

// Read an entire file under `root`. Returns nullopt if it does not exist.
std::optional<std::string> readFile(const std::string& root, const std::string& relpath) {
    std::string path = root;
    if (!path.empty() && path.back() == '/' && !relpath.empty() && relpath.front() == '/') {
        path.pop_back();
    } else if (!path.empty() && path.back() != '/' && !relpath.empty() && relpath.front() != '/') {
        path += '/';
    }
    path += relpath;

    std::ifstream file(path);
    if (!file.is_open()) {
        return std::nullopt;
    }
    std::stringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

// Any cgroup memory limit at or above this is treated as "unlimited". This
// covers PAGE_COUNTER_MAX and the various INT64/page-aligned sentinels used by
// cgroup v1, and guards against absurd values exceeding physical RAM.
constexpr uint64_t kUnlimitedThreshold = 0x7000000000000000ull;

uint64_t hostPhysicalBytes(const std::string& root) {
    // Prefer /proc/meminfo MemTotal (works with an injected root in tests);
    // fall back to sysconf on the real system.
    if (auto meminfo = readFile(root, "proc/meminfo")) {
        std::istringstream iss(*meminfo);
        std::string line;
        while (std::getline(iss, line)) {
            if (line.rfind("MemTotal:", 0) == 0) {
                std::istringstream ls(line.substr(std::string("MemTotal:").size()));
                uint64_t kb = 0;
                std::string unit;
                ls >> kb >> unit;
                if (kb > 0) {
                    return kb * 1024ull;
                }
            }
        }
    }
#if defined(__linux__)
    long pages = sysconf(_SC_PHYS_PAGES);
    long page_size = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && page_size > 0) {
        return static_cast<uint64_t>(pages) * static_cast<uint64_t>(page_size);
    }
#endif
    return 0;
}

} // namespace

std::optional<uint64_t> ParseCgroupV2MemoryMax(const std::string& content) {
    std::string s = trim(content);
    if (s.empty() || s == "max") {
        return std::nullopt;
    }
    try {
        size_t consumed = 0;
        uint64_t value = std::stoull(s, &consumed);
        if (consumed != s.size() || value == 0 || value >= kUnlimitedThreshold) {
            return std::nullopt;
        }
        return value;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<uint64_t> ParseCgroupV1MemoryLimit(const std::string& content) {
    std::string s = trim(content);
    if (s.empty()) {
        return std::nullopt;
    }
    try {
        size_t consumed = 0;
        uint64_t value = std::stoull(s, &consumed);
        if (consumed != s.size() || value == 0 || value >= kUnlimitedThreshold) {
            return std::nullopt;
        }
        return value;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

namespace {

std::optional<uint32_t> coresFromQuotaPeriod(int64_t quota, int64_t period) {
    if (quota <= 0 || period <= 0) {
        return std::nullopt;
    }
    // ceil(quota / period), clamped to >= 1.
    int64_t cores = (quota + period - 1) / period;
    if (cores < 1) {
        cores = 1;
    }
    return static_cast<uint32_t>(cores);
}

} // namespace

std::optional<uint32_t> ParseCgroupV2CpuMax(const std::string& content) {
    std::istringstream iss(trim(content));
    std::string quota_str;
    int64_t period = 0;
    if (!(iss >> quota_str >> period)) {
        return std::nullopt;
    }
    if (quota_str == "max") {
        return std::nullopt;
    }
    try {
        int64_t quota = std::stoll(quota_str);
        return coresFromQuotaPeriod(quota, period);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<uint32_t> ParseCgroupV1Cpu(const std::string& quota, const std::string& period) {
    try {
        int64_t q = std::stoll(trim(quota));
        int64_t p = std::stoll(trim(period));
        if (q < 0) { // -1 means unlimited
            return std::nullopt;
        }
        return coresFromQuotaPeriod(q, p);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

MemoryDetection DetectAvailableMemoryBytes(const std::string& root) {
    MemoryDetection result;
    uint64_t host = hostPhysicalBytes(root);

    auto bound = [&](uint64_t cgroup_value, const std::string& source) -> bool {
        uint64_t value = cgroup_value;
        if (host > 0) {
            value = std::min(value, host); // never exceed physical RAM
        }
        result.bytes = value;
        result.source = source;
        return true;
    };

#if defined(__linux__) || defined(FLAPI_FORCE_CGROUP_PROBE)
    // cgroup v2
    if (auto content = readFile(root, "sys/fs/cgroup/memory.max")) {
        if (auto v = ParseCgroupV2MemoryMax(*content)) {
            bound(*v, "cgroup-v2");
            return result;
        }
        // memory.max == "max": honor memory.high soft limit if present.
        if (auto high = readFile(root, "sys/fs/cgroup/memory.high")) {
            if (auto hv = ParseCgroupV2MemoryMax(*high)) {
                bound(*hv, "cgroup-v2");
                return result;
            }
        }
    }
    // cgroup v1
    if (auto content = readFile(root, "sys/fs/cgroup/memory/memory.limit_in_bytes")) {
        if (auto v = ParseCgroupV1MemoryLimit(*content)) {
            bound(*v, "cgroup-v1");
            return result;
        }
    }
#endif

    if (host > 0) {
        result.bytes = host;
        result.source = "meminfo";
        return result;
    }

#if defined(__APPLE__)
    {
        uint64_t memsize = 0;
        size_t len = sizeof(memsize);
        if (sysctlbyname("hw.memsize", &memsize, &len, nullptr, 0) == 0 && memsize > 0) {
            result.bytes = memsize;
            result.source = "sysctl";
            return result;
        }
    }
#elif defined(_WIN32)
    {
        MEMORYSTATUSEX status;
        status.dwLength = sizeof(status);
        if (GlobalMemoryStatusEx(&status) && status.ullTotalPhys > 0) {
            result.bytes = status.ullTotalPhys;
            result.source = "win-global";
            return result;
        }
    }
#endif

    return result; // bytes == 0, source == "unknown"
}

CpuDetection DetectAvailableCpuCount(const std::string& root) {
    CpuDetection result;
    uint32_t hw = std::thread::hardware_concurrency();

    auto finalize = [&](uint32_t cores, const std::string& source) {
        if (hw > 0) {
            cores = std::min(cores, hw);
        }
        if (cores < 1) {
            cores = 1;
        }
        result.cores = cores;
        result.source = source;
    };

#if defined(__linux__) || defined(FLAPI_FORCE_CGROUP_PROBE)
    // cgroup v2
    if (auto content = readFile(root, "sys/fs/cgroup/cpu.max")) {
        if (auto v = ParseCgroupV2CpuMax(*content)) {
            finalize(*v, "cgroup-v2");
            return result;
        }
    }
    // cgroup v1
    {
        auto quota = readFile(root, "sys/fs/cgroup/cpu/cpu.cfs_quota_us");
        auto period = readFile(root, "sys/fs/cgroup/cpu/cpu.cfs_period_us");
        if (quota && period) {
            if (auto v = ParseCgroupV1Cpu(*quota, *period)) {
                finalize(*v, "cgroup-v1");
                return result;
            }
        }
    }
#endif

    if (hw > 0) {
        result.cores = hw;
        result.source = "hardware";
        return result;
    }

    result.cores = 1;
    result.source = "hardware";
    return result;
}

} // namespace flapi
