// SPDX-License-Identifier: Apache-2.0
#include "tessera/net/replication/subscription_index.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

using namespace tessera::net;

namespace {

using Pair = std::pair<uint32_t, uint32_t>; // connection, compact id

std::vector<Pair> Subscribers(const SubscriptionIndex& index, EntityId entity) {
    std::vector<Pair> found;
    index.ForEach(entity, [&](uint32_t connection, uint32_t compact_id) {
        found.emplace_back(connection, compact_id);
    });
    std::sort(found.begin(), found.end());
    return found;
}

EntityId Id(uint32_t type, uint32_t index, uint32_t generation = 0) {
    return EntityId::Make(0, type, index, generation);
}

} // namespace

TESSERA_TEST(for_each_visits_every_subscriber_of_an_entity) {
    SubscriptionIndex index;
    const EntityId entity = Id(1, 7);
    index.Add(entity, 3, 30);
    index.Add(entity, 0, 5);
    index.Add(entity, 9, 90);
    index.Add(Id(1, 8), 1, 1); // another entity
    TESSERA_CHECK((Subscribers(index, entity) == std::vector<Pair>{{0, 5}, {3, 30}, {9, 90}}));
    TESSERA_CHECK((Subscribers(index, Id(1, 8)) == std::vector<Pair>{{1, 1}}));
    TESSERA_CHECK(index.Count() == 4);
}

TESSERA_TEST(entities_nobody_subscribed_to_are_not_visited) {
    SubscriptionIndex index;
    index.Add(Id(1, 7), 0, 0);
    TESSERA_CHECK(Subscribers(index, Id(1, 6)).empty());       // same page, never added
    TESSERA_CHECK(Subscribers(index, Id(1, 900'000)).empty()); // a page that does not exist
    TESSERA_CHECK(Subscribers(index, Id(200, 7)).empty());     // a type that does not exist
}

TESSERA_TEST(remove_unlinks_only_that_connection) {
    SubscriptionIndex index;
    const EntityId entity = Id(2, 100);
    for (uint32_t c = 0; c < 5; ++c) {
        index.Add(entity, c, 10 + c);
    }
    TESSERA_CHECK(index.Remove(entity, 2)); // the middle of the list
    TESSERA_CHECK(index.Remove(entity, 4)); // the first one added, the end of the list
    TESSERA_CHECK(index.Remove(entity, 0)); // the most recent
    TESSERA_CHECK((Subscribers(index, entity) == std::vector<Pair>{{1, 11}, {3, 13}}));
    TESSERA_CHECK(index.Count() == 2);
}

TESSERA_TEST(remove_reports_whether_the_subscription_existed) {
    SubscriptionIndex index;
    const EntityId entity = Id(2, 100);
    index.Add(entity, 1, 1);
    TESSERA_CHECK(!index.Remove(entity, 2));     // another connection
    TESSERA_CHECK(!index.Remove(Id(2, 101), 1)); // another entity
    TESSERA_CHECK(!index.Remove(Id(9, 100), 1)); // a type that was never used
    TESSERA_CHECK(index.Remove(entity, 1));
    TESSERA_CHECK(!index.Remove(entity, 1)); // already gone
    TESSERA_CHECK(index.Count() == 0);
}

TESSERA_TEST(a_stale_generation_is_neither_visited_nor_removed) {
    SubscriptionIndex index;
    const EntityId old_id = Id(1, 7, 0);
    const EntityId new_id = Id(1, 7, 1); // the same slot, reused
    index.Add(old_id, 0, 4);
    TESSERA_CHECK(
        Subscribers(index, new_id).empty()); // a change to the new entity must not reach the old viewers
    TESSERA_CHECK(!index.Remove(new_id, 0));
    index.Add(new_id, 0, 9);
    TESSERA_CHECK((Subscribers(index, old_id) == std::vector<Pair>{{0, 4}}));
    TESSERA_CHECK((Subscribers(index, new_id) == std::vector<Pair>{{0, 9}}));
    TESSERA_CHECK(index.Remove(old_id, 0));
    TESSERA_CHECK((Subscribers(index, new_id) == std::vector<Pair>{{0, 9}}));
}

TESSERA_TEST(entities_across_pages_and_types_are_kept_apart) {
    SubscriptionIndex index;
    const EntityId a = Id(0, 5), b = Id(0, 5000), c = Id(3, 5); // two pages of type 0, and another type
    index.Add(a, 1, 1);
    index.Add(b, 2, 2);
    index.Add(c, 3, 3);
    TESSERA_CHECK((Subscribers(index, a) == std::vector<Pair>{{1, 1}}));
    TESSERA_CHECK((Subscribers(index, b) == std::vector<Pair>{{2, 2}}));
    TESSERA_CHECK((Subscribers(index, c) == std::vector<Pair>{{3, 3}}));
}

TESSERA_TEST(freed_nodes_are_reused_so_churn_does_not_grow_memory) {
    SubscriptionIndex index;
    for (uint32_t i = 0; i < 1000; ++i) {
        index.Add(Id(1, i), i % 8, i);
    }
    const size_t before = index.MemoryBytes();
    for (int round = 0; round < 50; ++round) {
        for (uint32_t i = 0; i < 1000; ++i) {
            TESSERA_CHECK(index.Remove(Id(1, i), i % 8));
        }
        for (uint32_t i = 0; i < 1000; ++i) {
            index.Add(Id(1, i), i % 8, i);
        }
    }
    TESSERA_CHECK(index.MemoryBytes() == before);
    TESSERA_CHECK(index.Count() == 1000);
}
