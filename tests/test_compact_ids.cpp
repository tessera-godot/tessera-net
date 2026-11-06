// SPDX-License-Identifier: Apache-2.0
#include "tessera/net/relevancy/compact_ids.hpp"
#include "tessera/net/relevancy/relevancy_types.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <stdexcept>

using tessera::net::CompactIdAllocator;
using tessera::net::kInvalidCompactId;

TESSERA_TEST(compact_ids_are_handed_out_lowest_first) {
    CompactIdAllocator ids(100);
    TESSERA_CHECK(ids.Acquire() == 0);
    TESSERA_CHECK(ids.Acquire() == 1);
    TESSERA_CHECK(ids.Acquire() == 2);
}

TESSERA_TEST(released_compact_id_returns_only_after_begin_tick) {
    CompactIdAllocator ids(100);
    ids.Acquire();
    ids.Acquire();
    ids.Acquire();
    TESSERA_CHECK(ids.Release(1));
    TESSERA_CHECK(ids.Acquire() == 3);
    ids.BeginTick();
    TESSERA_CHECK(ids.Acquire() == 1);
}

TESSERA_TEST(compact_ids_run_out_and_recover) {
    CompactIdAllocator ids(3);
    ids.Acquire();
    ids.Acquire();
    ids.Acquire();
    TESSERA_CHECK(ids.Acquire() == kInvalidCompactId);
    TESSERA_CHECK(ids.Release(0));
    ids.BeginTick();
    TESSERA_CHECK(ids.Acquire() == 0);
}

TESSERA_TEST(releasing_a_free_or_out_of_range_id_fails) {
    CompactIdAllocator ids(10);
    TESSERA_CHECK(!ids.Release(0));
    TESSERA_CHECK(!ids.Release(10));
    TESSERA_CHECK(!ids.Release(kInvalidCompactId));
}

TESSERA_TEST(second_release_of_the_same_id_fails) {
    CompactIdAllocator ids(10);
    ids.Acquire();
    TESSERA_CHECK(ids.Release(0));
    TESSERA_CHECK(!ids.Release(0));
    ids.BeginTick();
    TESSERA_CHECK(ids.Acquire() == 0);
}

TESSERA_TEST(in_use_counts_ids_waiting_for_the_next_tick) {
    CompactIdAllocator ids(10);
    ids.Acquire();
    ids.Acquire();
    ids.Acquire();
    ids.Release(0);
    TESSERA_CHECK(ids.InUse() == 3);
    ids.BeginTick();
    TESSERA_CHECK(ids.InUse() == 2);
}

TESSERA_TEST(compact_id_allocator_handles_capacity_not_multiple_of_64) {
    CompactIdAllocator ids(70);
    for (uint32_t i = 0; i < 70; ++i) {
        TESSERA_CHECK(ids.Acquire() == i);
    }
    TESSERA_CHECK(ids.Acquire() == kInvalidCompactId);
    TESSERA_CHECK(ids.Capacity() == 70);
}

TESSERA_TEST(compact_id_allocator_rejects_zero_capacity) {
    bool threw = false;
    try {
        CompactIdAllocator ids(0);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    TESSERA_CHECK(threw);
}

TESSERA_TEST(pending_release_counts_ids_waiting_for_the_next_tick) {
    CompactIdAllocator ids(10);
    ids.Acquire();
    ids.Acquire();
    ids.Acquire();
    TESSERA_CHECK(ids.PendingRelease() == 0);
    ids.Release(0);
    ids.Release(2);
    TESSERA_CHECK(ids.PendingRelease() == 2);
    ids.BeginTick();
    TESSERA_CHECK(ids.PendingRelease() == 0);
}
