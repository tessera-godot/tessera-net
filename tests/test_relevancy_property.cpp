// SPDX-License-Identifier: Apache-2.0
// Compares the relevancy engine with a deliberately naive model that checks every entity against every
// observer, and checks that the events do not depend on how many worker threads run the update.
#include "relevancy_test_support.hpp"
#include "test_support.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <map>
#include <random>
#include <set>
#include <vector>

using namespace relevancy_test;
using Kind = tessera::net::VisibilityEvent::Kind;

namespace {

constexpr uint32_t kOwnerNone = 0xFFFFFFFFu;
constexpr uint32_t kTypeSpatialWide = 5;   // range 60, margin 20
constexpr uint32_t kTypeSpatialMedium = 6; // range 200, margin 5

RelevancyConfig PropertyConfig(uint32_t interval, uint32_t max_connections) {
    RelevancyConfig config = MakeConfig(interval);
    config.max_connections = max_connections;
    config.types[kSpatialType].hysteresis_margin = 10.0f;
    config.types[kFarType].hysteresis_margin = 0.0f;
    config.types[kTypeSpatialWide] = TypeRelevancy{RelevancyMode::Spatial, 60.0f, 20.0f};
    config.types[kTypeSpatialMedium] = TypeRelevancy{RelevancyMode::Spatial, 200.0f, 5.0f};
    return config;
}

struct World {
    std::vector<EntityId> ids;
    std::vector<std::array<float, 3>> position;
    std::vector<uint32_t> owner; // kOwnerNone when the type is not owner-only
};

struct GroupModel {
    GroupId id = 0;
    GroupKind kind = GroupKind::Inclusion;
    std::set<uint64_t> members;
    std::map<ConnectionId, bool> status; // exceptions; the default is "not allowed"
};

bool Allowed(const GroupModel& group, ConnectionId connection) {
    const auto it = group.status.find(connection);
    return it != group.status.end() && it->second;
}

uint32_t RandomIndex(std::mt19937& rng, uint32_t count) {
    return static_cast<uint32_t>(rng() % count);
}

float RandomIn(std::mt19937& rng, float low, float high) {
    return std::uniform_real_distribution<float>(low, high)(rng);
}

/// Fills the fixture with entities, owners and groups, and mirrors everything in the model.
void BuildWorld(Fixture& f, std::mt19937& rng, uint32_t entity_count, uint32_t connection_count, World& world,
                std::vector<GroupModel>& groups) {
    const uint32_t types[] = {kSpatialType, kFarType,   kTypeSpatialWide, kTypeSpatialMedium,
                              kGlobalType,  kOwnedType, kNoRangeType};
    for (uint32_t i = 0; i < entity_count; ++i) {
        const uint32_t type = types[RandomIndex(rng, 7)];
        const EntityId id = MakeId(type, i);
        const std::array<float, 3> p{RandomIn(rng, -500.0f, 500.0f), RandomIn(rng, -50.0f, 50.0f),
                                     RandomIn(rng, -500.0f, 500.0f)};
        f.grid.Insert(id, p[0], p[1], p[2]);
        f.engine.OnEntityCreated(id);
        uint32_t owner = kOwnerNone;
        if (type == kOwnedType) {
            owner = RandomIndex(rng, connection_count);
            f.engine.SetOwner(id, owner);
        }
        world.ids.push_back(id);
        world.position.push_back(p);
        world.owner.push_back(owner);
    }
    for (int g = 0; g < 4; ++g) {
        GroupModel model;
        model.kind = g < 2 ? GroupKind::Exclusion : GroupKind::Inclusion;
        model.id = f.groups.CreateGroup(model.kind);
        for (int m = 0; m < 50; ++m) {
            const EntityId member = world.ids[RandomIndex(rng, entity_count)];
            if (f.groups.AddMember(model.id, member)) {
                model.members.insert(member.raw());
            }
        }
        for (ConnectionId c = 0; c < connection_count; ++c) {
            if (rng() % 2 == 0) {
                f.groups.SetStatus(model.id, c, true);
                model.status[c] = true;
            }
        }
        groups.push_back(model);
    }
}

/// What a connection should see, by checking every entity against every observer.
std::set<uint64_t> Expected(const RelevancyConfig& config, const World& world,
                            const std::vector<GroupModel>& groups, ConnectionId connection,
                            const std::vector<Observer>& observers,
                            const std::set<uint64_t>& previously_visible) {
    std::set<uint64_t> wanted;
    for (size_t i = 0; i < world.ids.size(); ++i) {
        const EntityId id = world.ids[i];
        const TypeRelevancy& rule = config.types[id.type()];
        switch (rule.mode) {
        case RelevancyMode::AlwaysRelevant:
            wanted.insert(id.raw());
            break;
        case RelevancyMode::OwnerOnly:
            if (world.owner[i] == connection) {
                wanted.insert(id.raw());
            }
            break;
        case RelevancyMode::Spatial: {
            if (!(rule.range > 0.0f)) {
                break;
            }
            const bool seen = previously_visible.count(id.raw()) != 0;
            const double reach = seen ? static_cast<double>(rule.range + rule.hysteresis_margin)
                                      : static_cast<double>(rule.range);
            for (const Observer& o : observers) {
                const double dx = static_cast<double>(world.position[i][0]) - o.x;
                const double dy = static_cast<double>(world.position[i][1]) - o.y;
                const double dz = static_cast<double>(world.position[i][2]) - o.z;
                if (dx * dx + dy * dy + dz * dz <= reach * reach) {
                    wanted.insert(id.raw());
                    break;
                }
            }
            break;
        }
        }
    }
    for (const GroupModel& group : groups) {
        if (group.kind == GroupKind::Inclusion && Allowed(group, connection)) {
            wanted.insert(group.members.begin(), group.members.end());
        }
    }
    for (const GroupModel& group : groups) {
        if (group.kind == GroupKind::Exclusion && !Allowed(group, connection)) {
            for (const uint64_t raw : group.members) {
                wanted.erase(raw);
            }
        }
    }
    return wanted;
}

void ApplyEvents(const ConnectionEvents& events, std::set<uint64_t>& shadow) {
    for (const VisibilityEvent& e : events.events) {
        if (e.kind == Kind::Enter) {
            shadow.insert(e.id.raw());
        } else {
            shadow.erase(e.id.raw());
        }
    }
}

} // namespace

TESSERA_TEST(random_worlds_match_the_naive_model) {
    constexpr uint32_t kWorlds = 20;
    constexpr uint32_t kConnections = 6;
    for (uint32_t seed = 1; seed <= kWorlds; ++seed) {
        std::mt19937 rng(seed);
        const RelevancyConfig config = PropertyConfig(1, 16);
        Fixture f(config);
        World world;
        std::vector<GroupModel> groups;
        BuildWorld(f, rng, 2000, kConnections, world, groups);

        std::vector<std::vector<Observer>> observers(kConnections);
        std::vector<std::set<uint64_t>> shadow(kConnections);
        for (ConnectionId c = 0; c < kConnections; ++c) {
            f.engine.AddConnection(c);
            const uint32_t count = 1 + RandomIndex(rng, 3);
            for (uint32_t k = 0; k < count; ++k) {
                observers[c].push_back(Observer{RandomIn(rng, -400.0f, 400.0f), RandomIn(rng, -40.0f, 40.0f),
                                                RandomIn(rng, -400.0f, 400.0f)});
            }
            f.engine.SetObservers(c, observers[c]);
        }
        // Owners were assigned before the connections existed, so give them again now.
        for (size_t i = 0; i < world.ids.size(); ++i) {
            if (world.owner[i] != kOwnerNone) {
                f.engine.SetOwner(world.ids[i], world.owner[i]);
            }
        }

        for (uint64_t tick = 0; tick < 30; ++tick) {
            std::vector<PendingMove> moves;
            for (int m = 0; m < 100; ++m) {
                const uint32_t i = RandomIndex(rng, static_cast<uint32_t>(world.ids.size()));
                for (float& coordinate : world.position[i]) {
                    coordinate += RandomIn(rng, -40.0f, 40.0f);
                }
                moves.push_back(PendingMove{world.ids[i], world.position[i][0], world.position[i][1],
                                            world.position[i][2]});
            }
            f.grid.ApplyMoves(moves);
            for (ConnectionId c = 0; c < kConnections; ++c) {
                for (Observer& o : observers[c]) {
                    o.x += RandomIn(rng, -30.0f, 30.0f);
                    o.z += RandomIn(rng, -30.0f, 30.0f);
                }
                f.engine.SetObservers(c, observers[c]);
            }
            f.engine.Update(tick);
            for (ConnectionId c = 0; c < kConnections; ++c) {
                const std::set<uint64_t> expected =
                    Expected(config, world, groups, c, observers[c], shadow[c]);
                ApplyEvents(f.engine.Events(c), shadow[c]);
                f.engine.ClearEvents(c);
                if (shadow[c] != expected) {
                    std::printf("  mismatch: seed %u tick %llu connection %u (engine %zu, model %zu)\n", seed,
                                static_cast<unsigned long long>(tick), c, shadow[c].size(), expected.size());
                }
                TESSERA_CHECK(shadow[c] == expected);
                std::set<uint32_t> compact_ids;
                for (const uint64_t raw : shadow[c]) {
                    const uint32_t compact = f.engine.CompactIdOf(c, EntityId::FromRaw(raw));
                    TESSERA_CHECK(compact != kInvalidCompactId);
                    TESSERA_CHECK(compact_ids.insert(compact).second);
                }
            }
        }
    }
}

namespace {

constexpr uint64_t kFnvOffset = 1469598103934665603ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

void Mix(uint64_t& hash, uint64_t value) {
    hash = (hash ^ value) * kFnvPrime;
}

/// Runs a fixed scenario and returns a hash of every event of every connection, in order.
uint64_t RunScenarioHash(uint32_t workers) {
    constexpr uint32_t kConnections = 40;
    std::mt19937 rng(777);
    const RelevancyConfig config = PropertyConfig(3, 64);
    Fixture f(config, workers);
    World world;
    std::vector<GroupModel> groups;
    BuildWorld(f, rng, 3000, kConnections, world, groups);
    std::vector<std::vector<Observer>> observers(kConnections);
    for (ConnectionId c = 0; c < kConnections; ++c) {
        f.engine.AddConnection(c);
        observers[c].push_back(
            Observer{RandomIn(rng, -400.0f, 400.0f), 0.0f, RandomIn(rng, -400.0f, 400.0f)});
        f.engine.SetObservers(c, observers[c]);
    }
    for (size_t i = 0; i < world.ids.size(); ++i) {
        if (world.owner[i] != kOwnerNone) {
            f.engine.SetOwner(world.ids[i], world.owner[i]);
        }
    }
    uint64_t hash = kFnvOffset;
    for (uint64_t tick = 0; tick < 300; ++tick) {
        std::vector<PendingMove> moves;
        for (int m = 0; m < 150; ++m) {
            const uint32_t i = RandomIndex(rng, static_cast<uint32_t>(world.ids.size()));
            for (float& coordinate : world.position[i]) {
                coordinate += RandomIn(rng, -30.0f, 30.0f);
            }
            moves.push_back(
                PendingMove{world.ids[i], world.position[i][0], world.position[i][1], world.position[i][2]});
        }
        f.grid.ApplyMoves(moves);
        for (ConnectionId c = 0; c < kConnections; ++c) {
            observers[c][0].x += RandomIn(rng, -10.0f, 10.0f);
            observers[c][0].z += RandomIn(rng, -10.0f, 10.0f);
            f.engine.SetObservers(c, observers[c]);
        }
        if (tick == 100) { // a destruction in the middle of the run
            const EntityId victim = world.ids[10];
            f.grid.Remove(victim);
            f.engine.OnEntityDestroyed(victim);
        }
        f.engine.Update(tick);
        for (ConnectionId c = 0; c < kConnections; ++c) {
            for (const VisibilityEvent& e : f.engine.Events(c).events) {
                Mix(hash, static_cast<uint64_t>(e.kind));
                Mix(hash, e.id.raw());
                Mix(hash, e.compact_id);
            }
            Mix(hash, c);
            f.engine.ClearEvents(c);
        }
    }
    return hash;
}

} // namespace

TESSERA_TEST(events_are_identical_with_one_and_eight_workers) {
    const uint64_t single = RunScenarioHash(1);
    const uint64_t parallel = RunScenarioHash(8);
    TESSERA_CHECK(single == parallel);
    TESSERA_CHECK(single != kFnvOffset); // events were actually produced
}
