// SPDX-License-Identifier: Apache-2.0
// Shared helpers for the acceptance benchmarks: timing, medians and criterion reporting.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

namespace bench {

using Clock = std::chrono::steady_clock;

inline double Ms(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

inline double Median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

/// How many criteria failed so far.
inline int& Failures() {
    static int failures = 0;
    return failures;
}

inline void Report(const char* name, double value, const char* unit, double limit,
                   bool higher_is_better = false) {
    const bool pass = higher_is_better ? value >= limit : value <= limit;
    if (!pass) {
        ++Failures();
    }
    std::printf("%-46s %12.2f %-6s limit %s%-10.2f %s\n", name, value, unit,
                higher_is_better ? ">= " : "<= ", limit, pass ? "PASS" : "FAIL");
}

/// A measurement with no pass/fail limit.
inline void Info(const char* name, double value, const char* unit) {
    std::printf("%-46s %12.2f %-6s (information only)\n", name, value, unit);
}

inline void ReportBool(const char* name, bool pass) {
    if (!pass) {
        ++Failures();
    }
    std::printf("%-46s %12s %-6s %-22s %s\n", name, pass ? "equal" : "DIFFERENT", "", "",
                pass ? "PASS" : "FAIL");
}

} // namespace bench
