// SPDX-License-Identifier: Apache-2.0
// The allocation counter: replaces the global allocation functions of the test program so that a test can
// count what some code allocates (see allocation_counter.hpp), and tests the counter itself.
#include "allocation_counter.hpp"
#include "test_support.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

namespace {

std::atomic<bool> g_counting{false};
std::atomic<uint64_t> g_count{0};
std::atomic<uint64_t> g_bytes{0};
std::atomic<int64_t> g_live{0};

// Each block starts with what it needs to be counted when it is given back. 16 bytes keep what follows
// aligned for any ordinary type.
struct Header {
    std::size_t size;
    std::size_t counted; // allocated while a scope was alive
};
constexpr std::size_t kHeader = 16;
static_assert(sizeof(Header) <= kHeader, "the header must fit in front of the block");

void* Allocate(std::size_t size) {
    const bool counted = g_counting.load(std::memory_order_relaxed);
    void* raw = std::malloc(size + kHeader);
    if (raw == nullptr) {
        throw std::bad_alloc();
    }
    *static_cast<Header*>(raw) = Header{size, counted ? 1u : 0u};
    if (counted) {
        g_count.fetch_add(1, std::memory_order_relaxed);
        g_bytes.fetch_add(size, std::memory_order_relaxed);
        g_live.fetch_add(static_cast<int64_t>(size), std::memory_order_relaxed);
    }
    return static_cast<char*>(raw) + kHeader;
}

void* AllocateNoThrow(std::size_t size) noexcept {
    try {
        return Allocate(size);
    } catch (...) {
        return nullptr;
    }
}

void Free(void* memory) noexcept {
    if (memory == nullptr) {
        return;
    }
    void* raw = static_cast<char*>(memory) - kHeader;
    const Header header = *static_cast<Header*>(raw);
    if (header.counted != 0 && g_counting.load(std::memory_order_relaxed)) {
        g_live.fetch_sub(static_cast<int64_t>(header.size), std::memory_order_relaxed);
    }
    std::free(raw);
}

} // namespace

void* operator new(std::size_t size) {
    return Allocate(size);
}
void* operator new[](std::size_t size) {
    return Allocate(size);
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    return AllocateNoThrow(size);
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    return AllocateNoThrow(size);
}
void operator delete(void* memory) noexcept {
    Free(memory);
}
void operator delete[](void* memory) noexcept {
    Free(memory);
}
void operator delete(void* memory, std::size_t) noexcept {
    Free(memory);
}
void operator delete[](void* memory, std::size_t) noexcept {
    Free(memory);
}
void operator delete(void* memory, const std::nothrow_t&) noexcept {
    Free(memory);
}
void operator delete[](void* memory, const std::nothrow_t&) noexcept {
    Free(memory);
}

namespace allocation_test {

AllocationScope::AllocationScope() {
    g_count.store(0, std::memory_order_relaxed);
    g_bytes.store(0, std::memory_order_relaxed);
    g_live.store(0, std::memory_order_relaxed);
    g_counting.store(true, std::memory_order_relaxed);
}

AllocationScope::~AllocationScope() {
    g_counting.store(false, std::memory_order_relaxed);
}

uint64_t AllocationScope::Count() const {
    return g_count.load(std::memory_order_relaxed);
}

uint64_t AllocationScope::Bytes() const {
    return g_bytes.load(std::memory_order_relaxed);
}

uint64_t AllocationScope::LiveBytes() const {
    const int64_t live = g_live.load(std::memory_order_relaxed);
    return live > 0 ? static_cast<uint64_t>(live) : 0;
}

} // namespace allocation_test

namespace {

// A place the pointers go to, so that a compiler cannot see the allocations as unused and leave them out.
std::atomic<void*> g_sink{nullptr};

void Allocate64() {
    int* memory = new int[64];
    g_sink.store(memory);
    delete[] memory;
}

} // namespace

TESSERA_TEST(the_allocation_counter_counts_only_inside_a_scope) {
    using allocation_test::AllocationScope;
    Allocate64(); // before any scope: not counted anywhere
    {
        AllocationScope scope;
        TESSERA_CHECK(scope.Count() == 0 && scope.Bytes() == 0);
        Allocate64();
        TESSERA_CHECK(scope.Count() == 1);
        TESSERA_CHECK(scope.Bytes() == 64 * sizeof(int));
        char* text = new char[100];
        g_sink.store(text);
        TESSERA_CHECK(scope.Count() == 2);
        TESSERA_CHECK(scope.Bytes() == 64 * sizeof(int) + 100);
        TESSERA_CHECK(scope.LiveBytes() == 100); // the first was given back
        delete[] text;
        TESSERA_CHECK(scope.LiveBytes() == 0);
        TESSERA_CHECK(scope.Bytes() == 64 * sizeof(int) + 100); // what was asked stays asked
    }
    Allocate64(); // after the scope: not counted
    AllocationScope later;
    TESSERA_CHECK(later.Count() == 0); // a new scope starts from zero
    Allocate64();
    TESSERA_CHECK(later.Count() == 1);
}
