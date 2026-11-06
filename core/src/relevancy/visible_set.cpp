// SPDX-License-Identifier: Apache-2.0
#include "tessera/net/relevancy/visible_set.hpp"

#include <algorithm>
#include <cassert>

namespace tessera::net {

const VisibleEntry* VisibleSet::Find(uint64_t raw) const noexcept {
    const auto it =
        std::lower_bound(entries_.begin(), entries_.end(), raw,
                         [](const VisibleEntry& entry, uint64_t value) { return entry.raw < value; });
    return it != entries_.end() && it->raw == raw ? &*it : nullptr;
}

void VisibleSet::Apply(std::span<const uint64_t> leaving, std::span<const VisibleEntry> entering) {
    if (leaving.empty() && entering.empty()) {
        return;
    }
    scratch_.clear();
    scratch_.reserve(entries_.size() + entering.size());
    size_t next_leaving = 0;
    size_t next_entering = 0;
    for (const VisibleEntry& current : entries_) {
        while (next_entering < entering.size() && entering[next_entering].raw < current.raw) {
            scratch_.push_back(entering[next_entering++]);
        }
        assert(next_entering == entering.size() || entering[next_entering].raw != current.raw);
        if (next_leaving < leaving.size() && leaving[next_leaving] == current.raw) {
            ++next_leaving;
            continue;
        }
        scratch_.push_back(current);
    }
    while (next_entering < entering.size()) {
        scratch_.push_back(entering[next_entering++]);
    }
    assert(next_leaving == leaving.size());
    entries_.swap(scratch_);
}

} // namespace tessera::net
