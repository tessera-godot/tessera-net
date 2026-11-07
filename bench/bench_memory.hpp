// SPDX-License-Identifier: Apache-2.0
// The memory the process holds in RAM right now, for the benchmarks that report memory per connection.
#pragma once

#include <cstddef>
#include <fstream>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
// psapi.h must come after windows.h.
#include <psapi.h>
#endif

namespace bench {

inline size_t ResidentBytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters{};
    GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters));
    return static_cast<size_t>(counters.WorkingSetSize);
#else
    std::ifstream statm("/proc/self/statm");
    size_t total_pages = 0;
    size_t resident_pages = 0;
    statm >> total_pages >> resident_pages;
    return resident_pages * 4096;
#endif
}

} // namespace bench
