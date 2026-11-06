// SPDX-License-Identifier: Apache-2.0
#include "tessera/net/relevancy/visible_set.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <vector>

using tessera::net::VisibleEntry;
using tessera::net::VisibleSet;

namespace {

bool StrictlyAscending(const VisibleSet& set) {
    const auto entries = set.Entries();
    for (size_t i = 1; i < entries.size(); ++i) {
        if (!(entries[i - 1].raw < entries[i].raw)) {
            return false;
        }
    }
    return true;
}

} // namespace

TESSERA_TEST(empty_visible_set_finds_nothing) {
    const VisibleSet set;
    TESSERA_CHECK(set.Size() == 0);
    TESSERA_CHECK(set.Find(10) == nullptr);
    TESSERA_CHECK(!set.Contains(10));
    TESSERA_CHECK(set.Entries().empty());
}

TESSERA_TEST(apply_adds_entries_in_order_and_find_returns_their_ids) {
    VisibleSet set;
    const VisibleEntry entering[] = {{10, 7}, {20, 3}, {30, 9}};
    set.Apply({}, entering);
    TESSERA_CHECK(set.Size() == 3);
    TESSERA_CHECK(set.Entries()[0].raw == 10 && set.Entries()[1].raw == 20 && set.Entries()[2].raw == 30);
    TESSERA_CHECK(set.Find(20) != nullptr && set.Find(20)->compact_id == 3);
    TESSERA_CHECK(set.Find(10)->compact_id == 7 && set.Find(30)->compact_id == 9);
    TESSERA_CHECK(set.Find(25) == nullptr);
    TESSERA_CHECK(set.Contains(30) && !set.Contains(31));
}

TESSERA_TEST(apply_removes_and_adds_in_one_call) {
    VisibleSet set;
    const VisibleEntry first[] = {{10, 0}, {20, 1}, {30, 2}};
    set.Apply({}, first);
    const uint64_t leaving[] = {20};
    const VisibleEntry entering[] = {{15, 5}, {40, 6}};
    set.Apply(leaving, entering);
    TESSERA_CHECK(set.Size() == 4);
    TESSERA_CHECK(set.Entries()[0].raw == 10 && set.Entries()[1].raw == 15);
    TESSERA_CHECK(set.Entries()[2].raw == 30 && set.Entries()[3].raw == 40);
    TESSERA_CHECK(!set.Contains(20));
    TESSERA_CHECK(set.Find(15)->compact_id == 5 && set.Find(40)->compact_id == 6);
    TESSERA_CHECK(set.Find(30)->compact_id == 2);
}

TESSERA_TEST(apply_with_empty_arguments_changes_nothing) {
    VisibleSet set;
    const VisibleEntry first[] = {{10, 0}, {20, 1}};
    set.Apply({}, first);
    set.Apply({}, {});
    TESSERA_CHECK(set.Size() == 2 && set.Contains(10) && set.Contains(20));
}

TESSERA_TEST(apply_can_remove_everything_and_refill) {
    VisibleSet set;
    const VisibleEntry first[] = {{10, 0}, {20, 1}};
    set.Apply({}, first);
    const uint64_t leaving[] = {10, 20};
    set.Apply(leaving, {});
    TESSERA_CHECK(set.Size() == 0);
    const VisibleEntry second[] = {{5, 2}};
    set.Apply({}, second);
    TESSERA_CHECK(set.Size() == 1 && set.Find(5)->compact_id == 2);
}

TESSERA_TEST(large_apply_keeps_the_order) {
    VisibleSet set;
    std::vector<VisibleEntry> first;
    for (uint32_t i = 0; i < 2000; ++i) {
        first.push_back({static_cast<uint64_t>(i) * 10, i});
    }
    set.Apply({}, first);
    std::vector<uint64_t> leaving;
    for (uint32_t i = 0; i < 2000; i += 3) {
        leaving.push_back(static_cast<uint64_t>(i) * 10);
    }
    std::vector<VisibleEntry> entering;
    for (uint32_t i = 0; i < 500; ++i) {
        entering.push_back({static_cast<uint64_t>(i) * 10 + 5, 5000 + i});
    }
    set.Apply(leaving, entering);
    TESSERA_CHECK(set.Size() == 2000 - leaving.size() + 500);
    TESSERA_CHECK(StrictlyAscending(set));
    TESSERA_CHECK(!set.Contains(0) && set.Contains(10) && set.Contains(15));
    TESSERA_CHECK(set.Find(15)->compact_id == 5001);
}

TESSERA_TEST(clear_empties_the_visible_set) {
    VisibleSet set;
    const VisibleEntry first[] = {{10, 0}};
    set.Apply({}, first);
    set.Clear();
    TESSERA_CHECK(set.Size() == 0 && !set.Contains(10));
}
