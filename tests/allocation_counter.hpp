// SPDX-License-Identifier: Apache-2.0
// Counts the memory allocations of the test program while a scope is alive, so that a test can say that some
// code allocates nothing. The counting itself lives in test_allocation_counter.cpp, which replaces the global
// allocation functions of the test program; outside a scope they only pass the request on.
#pragma once

#include <cstdint>

namespace allocation_test {

class AllocationScope {
public:
    AllocationScope();
    ~AllocationScope();
    AllocationScope(const AllocationScope&) = delete;
    AllocationScope& operator=(const AllocationScope&) = delete;

    /// How many allocations were made since the scope began, by any thread.
    uint64_t Count() const;
    /// How many bytes they asked for in all.
    uint64_t Bytes() const;
    /// How many of those bytes are still held: what was asked for in the scope, less what the scope has
    /// given back.
    uint64_t LiveBytes() const;
};

} // namespace allocation_test
