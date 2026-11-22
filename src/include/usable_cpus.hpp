#pragma once

// The CPUs this process may actually run on: the scheduler affinity mask (taskset, cpusets,
// Kubernetes CPU manager) and the cgroup v2 CPU quota (`cpu.max`, i.e. a container's CPU
// limit), whichever is smaller. std::thread::hardware_concurrency() counts every host CPU, so a
// 2-CPU container on a 64-core host sized its pools for 64.
//
// Plain C calls only: it also runs from a static constructor before main.

#include <cstdio>
#include <cstdlib>
#include <thread>
#if defined(__linux__)
#include <sched.h>
#endif

namespace flapi {

inline unsigned UsableCpus() {
    unsigned cpus = std::thread::hardware_concurrency();
#if defined(__linux__)
    cpu_set_t mask;
    CPU_ZERO(&mask);
    if (sched_getaffinity(0, sizeof(mask), &mask) == 0) {
        const int affinity = CPU_COUNT(&mask);
        if (affinity > 0 && (cpus == 0 || static_cast<unsigned>(affinity) < cpus)) {
            cpus = static_cast<unsigned>(affinity);
        }
    }
    if (std::FILE* f = std::fopen("/sys/fs/cgroup/cpu.max", "r")) {
        char quota[32] = {0};
        long long period = 0;
        if (std::fscanf(f, "%31s %lld", quota, &period) == 2 && period > 0 && quota[0] != 'm') {
            const long long q = std::atoll(quota);
            if (q > 0) {
                const unsigned limit = static_cast<unsigned>((q + period - 1) / period);
                if (limit > 0 && (cpus == 0 || limit < cpus)) {
                    cpus = limit;
                }
            }
        }
        std::fclose(f);
    }
#endif
    return cpus == 0 ? 1 : cpus;
}

}  // namespace flapi
