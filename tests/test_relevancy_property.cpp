// SPDX-License-Identifier: Apache-2.0
// Compares the relevancy engine with a deliberately naive model that checks every entity against every
// anchor, and checks that the events do not depend on how many worker threads run the update. The model
// takes the anchors from the engine (`Anchors`), so it checks the visibility rule itself and not how the
// engine decides when to re-evaluate; separate checks make sure an anchor never lags an observer by the
// tolerance or more and that events come out in the promised order.
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
constexpr float kTolerance = 4.0f;

RelevancyConfig PropertyConfig(uint32_t max_connections, uint32_t reanchor_cap) {
    RelevancyConfig config = MakeConfig();
    config.max_connections = max_connections;
    config.observer_tolerance = kTolerance;
    config.max_reanchors_per_tick = reanchor_cap;
    config.types[kSpatialType].hysteresis_margin = 10.0f;
    config.types[kFarType].hysteresis_margin = 0.0f;
    config.types[kTypeSpatialWide] = TypeRelevancy{RelevancyMode::Spatial, 60.0f, 20.0f};
    config.types[kTypeSpatialMedium] = TypeRelevancy{RelevancyMode::Spatial, 200.0f, 5.0f};
    return config;
}

struct World {
    std::vector<EntityId> ids;
    std::vector<std::array<float, 3>> position;
    std::vector<uint32_t> owner; // kOwnerNone when nobody owns it
    std::vector<bool> alive;
};

struct GroupModel {
    GroupId id = 0;
    GroupKind kind = GroupKind::Inclusion;
    std::set<uint64_t> members;
    bool default_allowed = false;
    std::map<ConnectionId, bool> status; // exceptions to the default
};

bool Allowed(const GroupModel& group, ConnectionId connection) {
    const auto it = group.status.find(connection);
    return it != group.status.end() ? it->second : group.default_allowed;
}

uint32_t RandomIndex(std::mt19937& rng, uint32_t count) {
    return static_cast<uint32_t>(rng() % count);
}

float RandomIn(std::mt19937& rng, float low, float high) {
    return std::uniform_real_distribution<float>(low, high)(rng);
}

uint32_t PickAlive(std::mt19937& rng, const World& world) {
    for (;;) {
        const uint32_t i = RandomIndex(rng, static_cast<uint32_t>(world.ids.size()));
        if (world.alive[i]) {
            return i;
        }
    }
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
        world.ids.push_back(id);
        world.position.push_back(p);
        world.owner.push_back(type == kOwnedType ? RandomIndex(rng, connection_count) : kOwnerNone);
        world.alive.push_back(true);
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

/// What a connection should see, by checking every living entity against every anchor.
std::set<uint64_t> Expected(const RelevancyConfig& config, const World& world,
                            const std::vector<GroupModel>& groups, ConnectionId connection,
                            const std::vector<Observer>& anchors,
                            const std::set<uint64_t>& previously_visible) {
    std::set<uint64_t> wanted;
    for (size_t i = 0; i < world.ids.size(); ++i) {
        if (!world.alive[i]) {
            continue;
        }
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
            for (const Observer& a : anchors) {
                const double dx = static_cast<double>(world.position[i][0]) - a.x;
                const double dy = static_cast<double>(world.position[i][1]) - a.y;
                const double dz = static_cast<double>(world.position[i][2]) - a.z;
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
            for (const uint64_t raw : group.members) {
                wanted.insert(raw);
            }
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

/// Destroy events first, then Leave, then Enter, each in ascending id order.
bool EventsAreOrdered(const ConnectionEvents& events) {
    int last_rank = 0;
    uint64_t last_raw = 0;
    bool first = true;
    for (const VisibilityEvent& e : events.events) {
        const int rank = e.kind == Kind::Destroy ? 0 : (e.kind == Kind::Leave ? 1 : 2);
        if (!first && (rank < last_rank || (rank == last_rank && e.id.raw() <= last_raw))) {
            return false;
        }
        first = false;
        last_rank = rank;
        last_raw = e.id.raw();
    }
    return true;
}

bool AnchorsFollowObservers(std::span<const Observer> anchors, const std::vector<Observer>& observers) {
    if (anchors.size() != observers.size()) {
        return false;
    }
    for (size_t i = 0; i < observers.size(); ++i) {
        const double dx = static_cast<double>(observers[i].x) - anchors[i].x;
        const double dy = static_cast<double>(observers[i].y) - anchors[i].y;
        const double dz = static_cast<double>(observers[i].z) - anchors[i].z;
        if (!(dx * dx + dy * dy + dz * dz < static_cast<double>(kTolerance) * kTolerance)) {
            return false;
        }
    }
    return true;
}

/// Moves each observer a little most of the time and a lot now and then.
void JitterObservers(std::mt19937& rng, std::vector<Observer>& observers) {
    const bool big = rng() % 10 < 3;
    const float spread = big ? 30.0f : 1.5f;
    for (Observer& o : observers) {
        o.x += RandomIn(rng, -spread, spread);
        o.z += RandomIn(rng, -spread, spread);
    }
}

std::vector<Observer> RandomObservers(std::mt19937& rng, uint32_t max_count) {
    std::vector<Observer> observers;
    const uint32_t count = 1 + RandomIndex(rng, max_count);
    for (uint32_t k = 0; k < count; ++k) {
        observers.push_back(Observer{RandomIn(rng, -400.0f, 400.0f), RandomIn(rng, -40.0f, 40.0f),
                                     RandomIn(rng, -400.0f, 400.0f)});
    }
    return observers;
}

} // namespace

TESSERA_TEST(random_worlds_match_the_naive_model) {
    constexpr uint32_t kWorlds = 20;
    constexpr uint32_t kConnections = 6;
    constexpr uint32_t kTicks = 40;
    for (uint32_t seed = 1; seed <= kWorlds; ++seed) {
        std::mt19937 rng(seed);
        const RelevancyConfig config = PropertyConfig(16, 0);
        Fixture f(config);
        World world;
        std::vector<GroupModel> groups;
        BuildWorld(f, rng, 2000, kConnections, world, groups);

        std::vector<bool> active(kConnections, false);
        std::vector<std::vector<Observer>> observers(kConnections);
        std::vector<std::set<uint64_t>> shadow(kConnections);
        for (ConnectionId c = 0; c < kConnections; ++c) {
            f.engine.AddConnection(c);
            active[c] = true;
            observers[c] = RandomObservers(rng, 3);
            f.engine.SetObservers(c, observers[c]);
        }
        // Owners were chosen before the connections existed, so give them to the engine now.
        for (size_t i = 0; i < world.ids.size(); ++i) {
            if (world.owner[i] != kOwnerNone) {
                f.engine.SetOwner(world.ids[i], world.owner[i]);
            }
        }

        for (uint64_t tick = 0; tick < kTicks; ++tick) {
            // Entities move, some a little and some a lot.
            std::vector<PendingMove> moves;
            for (int m = 0; m < 100; ++m) {
                const uint32_t i = PickAlive(rng, world);
                const float spread = rng() % 2 == 0 ? 1.0f : 40.0f;
                for (float& coordinate : world.position[i]) {
                    coordinate += RandomIn(rng, -spread, spread);
                }
                moves.push_back(PendingMove{world.ids[i], world.position[i][0], world.position[i][1],
                                            world.position[i][2]});
            }
            f.grid.ApplyMoves(moves);

            // The game changes groups, owners and the population now and then.
            if (rng() % 2 == 0) {
                GroupModel& g = groups[RandomIndex(rng, static_cast<uint32_t>(groups.size()))];
                const ConnectionId c = RandomIndex(rng, kConnections);
                const bool allowed = rng() % 2 == 0;
                f.groups.SetStatus(g.id, c, allowed);
                g.status[c] = allowed;
            }
            if (rng() % 10 == 0) {
                GroupModel& g = groups[RandomIndex(rng, static_cast<uint32_t>(groups.size()))];
                const bool allowed = rng() % 2 == 0;
                f.groups.SetStatusForAll(g.id, allowed);
                g.default_allowed = allowed;
                g.status.clear();
            }
            if (rng() % 5 == 0) {
                GroupModel& g = groups[RandomIndex(rng, static_cast<uint32_t>(groups.size()))];
                const uint32_t i = PickAlive(rng, world);
                if (g.members.count(world.ids[i].raw()) != 0) {
                    f.groups.RemoveMember(g.id, world.ids[i]);
                    g.members.erase(world.ids[i].raw());
                } else {
                    f.groups.AddMember(g.id, world.ids[i]);
                    g.members.insert(world.ids[i].raw());
                }
            }
            if (rng() % 3 == 0) {
                const uint32_t i = PickAlive(rng, world);
                const ConnectionId c = RandomIndex(rng, kConnections);
                if (f.engine.SetOwner(world.ids[i], c)) {
                    world.owner[i] = c;
                }
            }
            if (rng() % 5 == 0) {
                const uint32_t i = PickAlive(rng, world);
                f.grid.Remove(world.ids[i]);
                f.engine.OnEntityDestroyed(world.ids[i]);
                world.alive[i] = false;
                world.owner[i] = kOwnerNone;
                for (GroupModel& g : groups) {
                    g.members.erase(world.ids[i].raw());
                }
            }
            if (rng() % 5 == 0) { // a new entity appears
                const uint32_t spawn_types[] = {kSpatialType,       kFarType,    kTypeSpatialWide,
                                                kTypeSpatialMedium, kGlobalType, kOwnedType};
                const EntityId id =
                    MakeId(spawn_types[RandomIndex(rng, 6)], static_cast<uint32_t>(world.ids.size()));
                const std::array<float, 3> p{RandomIn(rng, -500.0f, 500.0f), RandomIn(rng, -50.0f, 50.0f),
                                             RandomIn(rng, -500.0f, 500.0f)};
                f.grid.Insert(id, p[0], p[1], p[2]);
                f.engine.OnEntityCreated(id);
                world.ids.push_back(id);
                world.position.push_back(p);
                world.owner.push_back(kOwnerNone);
                world.alive.push_back(true);
            }
            if (rng() % 20 == 0) {
                const ConnectionId c = RandomIndex(rng, kConnections);
                f.engine.RemoveConnection(c);
                shadow[c].clear();
                for (size_t i = 0; i < world.ids.size(); ++i) {
                    if (world.owner[i] == c) {
                        world.owner[i] = kOwnerNone;
                    }
                }
                for (GroupModel& g : groups) {
                    g.status.erase(c);
                }
                f.engine.AddConnection(c);
                observers[c] = RandomObservers(rng, 3);
            }

            for (ConnectionId c = 0; c < kConnections; ++c) {
                JitterObservers(rng, observers[c]);
                f.engine.SetObservers(c, observers[c]);
            }
            f.engine.Update(tick);

            for (ConnectionId c = 0; c < kConnections; ++c) {
                const std::span<const Observer> reported = f.engine.Anchors(c);
                const std::vector<Observer> anchors(reported.begin(), reported.end());
                const std::set<uint64_t> expected = Expected(config, world, groups, c, anchors, shadow[c]);
                TESSERA_CHECK(EventsAreOrdered(f.engine.Events(c)));
                TESSERA_CHECK(AnchorsFollowObservers(reported, observers[c]));
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

/// Runs a fixed scenario and returns a hash of every event of every connection, in order. A cap of 8
/// re-anchors per tick is on, so the deferral rules take part in the comparison.
uint64_t RunScenarioHash(uint32_t workers) {
    constexpr uint32_t kConnections = 40;
    std::mt19937 rng(777);
    const RelevancyConfig config = PropertyConfig(64, 8);
    Fixture f(config, workers);
    World world;
    std::vector<GroupModel> groups;
    BuildWorld(f, rng, 3000, kConnections, world, groups);
    std::vector<std::vector<Observer>> observers(kConnections);
    for (ConnectionId c = 0; c < kConnections; ++c) {
        f.engine.AddConnection(c);
        observers[c] = RandomObservers(rng, 1);
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
            const uint32_t i = PickAlive(rng, world);
            const float spread = rng() % 2 == 0 ? 1.0f : 30.0f;
            for (float& coordinate : world.position[i]) {
                coordinate += RandomIn(rng, -spread, spread);
            }
            moves.push_back(
                PendingMove{world.ids[i], world.position[i][0], world.position[i][1], world.position[i][2]});
        }
        f.grid.ApplyMoves(moves);
        for (ConnectionId c = 0; c < kConnections; ++c) {
            JitterObservers(rng, observers[c]);
            f.engine.SetObservers(c, observers[c]);
        }
        if (tick == 50) { // a change to the groups
            f.groups.SetStatusForAll(groups[2].id, true);
        }
        if (tick == 100) { // a destruction in the middle of the run
            const uint32_t i = PickAlive(rng, world);
            f.grid.Remove(world.ids[i]);
            f.engine.OnEntityDestroyed(world.ids[i]);
            world.alive[i] = false;
        }
        if (tick == 150) { // a connection leaves and comes back
            f.engine.RemoveConnection(3);
            f.engine.AddConnection(3);
            observers[3] = RandomObservers(rng, 1);
            f.engine.SetObservers(3, observers[3]);
        }
        if (tick == 200) { // an owner changes
            for (size_t i = 0; i < world.ids.size(); ++i) {
                if (world.alive[i] && world.ids[i].type() == kOwnedType) {
                    f.engine.SetOwner(world.ids[i], 5);
                    break;
                }
            }
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
