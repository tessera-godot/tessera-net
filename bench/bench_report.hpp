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

/// A straight line y = intercept + slope * x fitted to points by least squares.
struct LineFit {
    double intercept;
    double slope;
    double r2; // share of the variation in y that the line explains
};

/// With fewer than two points, or with every x equal, there is no slope to find: the slope is 0 and the r2 is
/// 0, and the intercept is the mean of y: 0 for no points and the y value for one point. With every y
/// equal the line is exact and r2 is 1.
inline LineFit FitLine(const std::vector<double>& x, const std::vector<double>& y) {
    const size_t n = std::min(x.size(), y.size());
    if (n == 0) {
        return LineFit{0.0, 0.0, 0.0};
    }
    double mean_x = 0.0, mean_y = 0.0;
    for (size_t i = 0; i < n; ++i) {
        mean_x += x[i];
        mean_y += y[i];
    }
    mean_x /= static_cast<double>(n);
    mean_y /= static_cast<double>(n);
    double sxx = 0.0, sxy = 0.0, syy = 0.0;
    for (size_t i = 0; i < n; ++i) {
        sxx += (x[i] - mean_x) * (x[i] - mean_x);
        sxy += (x[i] - mean_x) * (y[i] - mean_y);
        syy += (y[i] - mean_y) * (y[i] - mean_y);
    }
    if (n < 2 || sxx == 0.0) {
        return LineFit{mean_y, 0.0, 0.0};
    }
    const double slope = sxy / sxx;
    const double intercept = mean_y - slope * mean_x;
    if (syy == 0.0) {
        return LineFit{intercept, slope, 1.0};
    }
    double residual = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double error = y[i] - (intercept + slope * x[i]);
        residual += error * error;
    }
    return LineFit{intercept, slope, 1.0 - residual / syy};
}

} // namespace bench
