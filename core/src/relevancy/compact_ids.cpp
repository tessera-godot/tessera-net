// SPDX-License-Identifier: Apache-2.0
#include "tessera/net/relevancy/compact_ids.hpp"

#include "tessera/net/relevancy/relevancy_types.hpp"

#include <algorithm>
#include <bit>
#include <stdexcept>

namespace tessera::net {

namespace {

constexpr uint32_t kBitsPerWord = 64;

} // namespace

CompactIdAllocator::CompactIdAllocator(uint32_t capacity) : capacity_(capacity) {
    if (capacity == 0) {
        throw std::invalid_argument("CompactIdAllocator: capacity must be at least 1");
    }
    const uint32_t words = (capacity + kBitsPerWord - 1) / kBitsPerWord;
    used_.assign(words, 0);
    pending_.assign(words, 0);
    // Mark the unused bits of the last word as taken so Acquire never returns them.
    const uint32_t tail = capacity % kBitsPerWord;
    if (tail != 0) {
        used_.back() = ~uint64_t{0} << tail;
    }
}

uint32_t CompactIdAllocator::Acquire() {
    for (uint32_t word = lowest_word_; word < used_.size(); ++word) {
        const uint64_t free_bits = ~used_[word];
        if (free_bits == 0) {
            continue;
        }
        lowest_word_ = word;
        const uint32_t bit = static_cast<uint32_t>(std::countr_zero(free_bits));
        used_[word] |= uint64_t{1} << bit;
        ++in_use_;
        return word * kBitsPerWord + bit;
    }
    lowest_word_ = static_cast<uint32_t>(used_.size());
    return kInvalidCompactId;
}

bool CompactIdAllocator::Release(uint32_t id) {
    if (id >= capacity_) {
        return false;
    }
    const uint32_t word = id / kBitsPerWord;
    const uint64_t mask = uint64_t{1} << (id % kBitsPerWord);
    if ((used_[word] & mask) == 0 || (pending_[word] & mask) != 0) {
        return false;
    }
    pending_[word] |= mask;
    released_.push_back(id);
    return true;
}

void CompactIdAllocator::BeginTick() {
    for (const uint32_t id : released_) {
        const uint32_t word = id / kBitsPerWord;
        const uint64_t mask = uint64_t{1} << (id % kBitsPerWord);
        used_[word] &= ~mask;
        pending_[word] &= ~mask;
        --in_use_;
        lowest_word_ = std::min(lowest_word_, word);
    }
    released_.clear();
}

} // namespace tessera::net
