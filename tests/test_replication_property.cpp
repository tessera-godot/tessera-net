// SPDX-License-Identifier: Apache-2.0
// Replication against a naive model with a bad network: packets are lost, duplicated and delayed, and so are
// the confirmations. Visibility events travel on a reliable ordered channel (with a delay of their own).
#include "replication_test_support.hpp"
#include "tessera/net/job_system.hpp"
#include "tessera/net/replication/receiver_state.hpp"
#include "tessera/net/replication/replication_engine.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <vector>

using namespace tessera::net;
using namespace replication_test;

namespace {

using Table = EntityTable<TestColumns>;
using Words = std::array<uint32_t, 9>;

constexpr uint32_t kIds = 64; // compact ids per connection

struct Params {
    uint32_t seed = 1;
    uint32_t workers = 1;
    uint32_t entities = 60;
    uint32_t connections = 3;
    uint32_t ticks = 120;
    double loss = 0.2;         // chance a data packet is lost
    double duplicate = 0.1;    // chance it is delivered twice
    double ack_loss = 0.2;     // chance a confirmation is lost
    double toggle = 0.05;      // per entity and connection and tick: chance its visibility flips
    double destroy = 0.02;     // per tick: chance an entity is destroyed (and another one created)
    uint32_t settle_every = 0; // if set, stop and check convergence after every this many ticks
    bool few_values =
        false; // values flip between a couple of states, so an entity often returns to an old one
};

struct EventPacket {
    uint64_t deliver_at;
    uint64_t tick;
    std::vector<VisibilityEvent> events;
};

struct DataPacket {
    uint64_t deliver_at;
    uint32_t connection;
    uint64_t tick;
    std::vector<uint8_t> bytes;
};

struct Ack {
    uint64_t deliver_at;
    uint32_t connection;
    uint64_t tick;
};

struct ConnectionModel {
    std::set<uint32_t> free_ids;
    std::vector<uint32_t> released_last_tick;
    std::map<uint64_t, uint32_t> visible; // entity raw -> compact id, as the server sees it
    std::deque<EventPacket> events_in_flight;
    uint64_t last_event_delivery = 0;
    std::unique_ptr<ReceiverState> receiver;
    std::array<uint64_t, kIds> client_entity{}; // raw ids; all bits set means none
    std::array<uint64_t, kIds> last_tick{};
};

struct Outcome {
    uint64_t hash = 0;
    bool converged = false;
    bool idle_is_free = false;
    uint64_t applied_ok = 0;
    uint64_t rejected = 0;
    uint64_t complete_records = 0;
    uint64_t partial_records = 0;
};

uint64_t Fnv(uint64_t hash, uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        hash ^= (value >> (8 * i)) & 0xFF;
        hash *= 0x100000001B3ull;
    }
    return hash;
}

class World {
public:
    explicit World(const Params& params)
        : params_(params), jobs_(params.workers), engine_(schema_, MakeConfig(params), jobs_),
          table_(TableConfig{0, kMobType}), random_(params.seed) {
        connections_.resize(params.connections);
        for (uint32_t c = 0; c < params.connections; ++c) {
            ConnectionModel& model = connections_[c];
            for (uint32_t id = 0; id < kIds; ++id) {
                model.free_ids.insert(id);
            }
            model.receiver = std::make_unique<ReceiverState>(schema_, kIds, 32);
            model.client_entity.fill(EntityId::Null().raw());
            engine_.AddConnection(c);
        }
        for (uint32_t i = 0; i < params.entities; ++i) {
            alive_.push_back(SpawnRandom());
        }
    }

    Outcome Run() {
        uint64_t tick = 1;
        bool converged = true;
        if (params_.settle_every > 0) {
            // Check along the way, not only at the end: a client left on a wrong state is usually put right
            // by the next change, so only a quiet stretch with a clean channel shows it.
            for (uint32_t done = 0; done + params_.settle_every <= params_.ticks;
                 done += params_.settle_every) {
                for (uint32_t i = 0; i < params_.settle_every; ++i, ++tick) {
                    Step(tick, true, true);
                }
                for (uint32_t i = 0; i < 20; ++i, ++tick) {
                    Step(tick, false, false);
                }
                converged = CheckConverged() && converged;
            }
        } else {
            for (; tick <= params_.ticks; ++tick) {
                Step(tick, true, true);
            }
        }
        const uint64_t drain_until = tick + 40;
        for (; tick <= drain_until; ++tick) {
            Step(tick, false, false);
        }
        outcome_.converged = CheckConverged() && converged;
        // Everything is confirmed now, so a further tick writes nothing but terminators.
        bool idle = true;
        for (uint32_t extra = 0; extra < 5; ++extra, ++tick) {
            Step(tick, false, false);
            for (uint32_t c = 0; c < params_.connections; ++c) {
                idle = idle && engine_.Output(c).size() == 1;
            }
        }
        outcome_.idle_is_free = idle;
        outcome_.hash = hash_;
        return outcome_;
    }

private:
    static ReplicationConfig MakeConfig(const Params& params) {
        ReplicationConfig config;
        config.max_connections = params.connections;
        config.max_visible_per_connection = kIds;
        return config;
    }

    uint32_t Roll(uint32_t below) {
        return static_cast<uint32_t>(random_() % below);
    }
    bool Chance(double probability) {
        return std::uniform_real_distribution<double>(0.0, 1.0)(random_) < probability;
    }

    EntityId SpawnRandom() {
        const EntityId id = table_.Create();
        Randomize(id, true);
        return id;
    }

    void Randomize(EntityId id, bool everything) {
        const auto where = table_.Find(id);
        TestColumns& c = table_.Columns(where->page);
        const uint32_t slot = where->slot;
        if (params_.few_values) {
            // A pulsing flag, a health that goes 100, 99, 100, an entity that steps back and forth.
            c.x[slot] = Chance(0.5) ? 100.0f : 100.5f;
            c.y[slot] = 0.0f;
            c.z[slot] = 0.0f;
            c.yaw[slot] = Chance(0.5) ? 0.0f : 1.0f;
            c.health[slot] = Chance(0.5) ? 100 : 99;
            c.flags[slot] = Chance(0.5) ? 0 : 1;
            c.state[slot] = static_cast<uint8_t>(Roll(2));
            table_.MarkDirty(id);
            return;
        }
        std::uniform_real_distribution<float> position(-9000.0f, 9000.0f);
        if (everything || Chance(0.6)) {
            c.x[slot] = position(random_);
            c.y[slot] = position(random_);
            c.z[slot] = position(random_);
        }
        if (everything || Chance(0.2)) {
            c.yaw[slot] = std::uniform_real_distribution<float>(-7.0f, 7.0f)(random_);
        }
        if (everything || Chance(0.2)) {
            c.health[slot] = static_cast<int32_t>(Roll(2200)) - 1100; // some are out of range on purpose
        }
        if (everything || Chance(0.2)) {
            c.flags[slot] = static_cast<uint16_t>(Roll(65536));
        }
        if (everything || Chance(0.2)) {
            c.state[slot] = static_cast<uint8_t>(Roll(10));
        }
        table_.MarkDirty(id);
    }

    void Step(uint64_t tick, bool mutate, bool lossy) {
        for (ConnectionModel& model : connections_) { // ids freed last tick can be used again now
            model.free_ids.insert(model.released_last_tick.begin(), model.released_last_tick.end());
            model.released_last_tick.clear();
        }
        std::vector<EntityId> destroyed;
        if (mutate) {
            for (size_t i = 0; i < alive_.size(); ++i) {
                if (Chance(params_.destroy)) {
                    destroyed.push_back(alive_[i]);
                    table_.Destroy(alive_[i]);
                    alive_[i] = SpawnRandom(); // often takes the freed slot with a new generation
                } else if (Chance(0.25)) {
                    Randomize(alive_[i], false);
                }
            }
        }
        for (uint32_t c = 0; c < params_.connections; ++c) {
            BuildEvents(c, tick, destroyed, mutate);
        }
        engine_.Refresh(table_);
        RecordHistory();
        engine_.Update(tick);
        for (uint32_t c = 0; c < params_.connections; ++c) {
            const std::span<const uint8_t> output = engine_.Output(c);
            for (const uint8_t byte : output) {
                hash_ = Fnv(hash_, byte);
            }
            if (lossy && Chance(params_.loss)) {
                continue;
            }
            const std::vector<uint8_t> bytes(output.begin(), output.end());
            data_.push_back(DataPacket{tick + Roll(4), c, tick, bytes});
            if (lossy && Chance(params_.duplicate)) {
                data_.push_back(DataPacket{tick + Roll(4), c, tick, bytes});
            }
        }
        Deliver(tick, lossy);
    }

    void BuildEvents(uint32_t c, uint64_t tick, const std::vector<EntityId>& destroyed, bool mutate) {
        ConnectionModel& model = connections_[c];
        std::vector<VisibilityEvent> gone, left, entered;
        for (const EntityId id : destroyed) {
            const auto it = model.visible.find(id.raw());
            if (it != model.visible.end()) {
                gone.push_back({VisibilityEvent::Kind::Destroy, id, it->second});
                model.released_last_tick.push_back(it->second);
                model.visible.erase(it);
            }
        }
        if (mutate) {
            for (const EntityId id : alive_) {
                const auto it = model.visible.find(id.raw());
                if (it != model.visible.end()) {
                    if (Chance(params_.toggle)) {
                        left.push_back({VisibilityEvent::Kind::Leave, id, it->second});
                        model.released_last_tick.push_back(it->second);
                        model.visible.erase(it);
                    }
                } else if (Chance(params_.toggle * 2) && !model.free_ids.empty()) {
                    const uint32_t compact_id = *model.free_ids.begin(); // the lowest, to force reuse
                    model.free_ids.erase(model.free_ids.begin());
                    model.visible[id.raw()] = compact_id;
                    entered.push_back({VisibilityEvent::Kind::Enter, id, compact_id});
                }
            }
        }
        auto by_id = [](const VisibilityEvent& a, const VisibilityEvent& b) {
            return a.id.raw() < b.id.raw();
        };
        std::sort(gone.begin(), gone.end(), by_id);
        std::sort(left.begin(), left.end(), by_id);
        std::sort(entered.begin(), entered.end(), by_id);
        std::vector<VisibilityEvent> events;
        events.insert(events.end(), gone.begin(), gone.end());
        events.insert(events.end(), left.begin(), left.end());
        events.insert(events.end(), entered.begin(), entered.end());
        engine_.ApplyVisibility(c, events);
        // The reliable channel delivers in order, each batch after the previous one.
        const uint64_t at = std::max(model.last_event_delivery, tick + Roll(3));
        model.last_event_delivery = at;
        model.events_in_flight.push_back(EventPacket{at, tick, events});
    }

    void RecordHistory() {
        for (const EntityId id : alive_) {
            const uint32_t* words = engine_.Store().Find(id);
            if (words == nullptr) {
                continue;
            }
            Words now;
            std::copy(words, words + 9, now.begin());
            std::vector<Words>& list = history_[id.raw()];
            if (list.empty() || list.back() != now) {
                list.push_back(now);
            }
        }
    }

    void Deliver(uint64_t now, bool lossy) {
        for (uint32_t c = 0; c < params_.connections; ++c) {
            ConnectionModel& model = connections_[c];
            while (!model.events_in_flight.empty() && model.events_in_flight.front().deliver_at <= now) {
                const EventPacket& packet = model.events_in_flight.front();
                for (const VisibilityEvent& event : packet.events) {
                    if (event.kind == VisibilityEvent::Kind::Enter) {
                        model.receiver->OnEnter(event.compact_id, event.id, packet.tick);
                        model.client_entity[event.compact_id] = event.id.raw();
                        model.last_tick[event.compact_id] = 0;
                    } else {
                        model.receiver->OnLeave(event.compact_id);
                        model.client_entity[event.compact_id] = EntityId::Null().raw();
                    }
                }
                model.receiver->AdvanceEvents(packet.tick);
                model.events_in_flight.pop_front();
            }
        }
        std::vector<DataPacket> due;
        for (size_t i = 0; i < data_.size();) {
            if (data_[i].deliver_at <= now) {
                due.push_back(std::move(data_[i]));
                data_.erase(data_.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                ++i;
            }
        }
        std::stable_sort(due.begin(), due.end(), [](const DataPacket& a, const DataPacket& b) {
            return a.deliver_at != b.deliver_at ? a.deliver_at < b.deliver_at : a.tick < b.tick;
        });
        std::vector<DataPacket> retry;
        for (const DataPacket& packet : due) {
            ConnectionModel& model = connections_[packet.connection];
            if (lossy && Chance(0.05)) { // a damaged copy must be refused without harm
                ReceiverState copy = *model.receiver;
                std::vector<uint8_t> noisy = packet.bytes;
                for (uint32_t flip = 0, n = 1 + Roll(4); flip < n; ++flip) {
                    noisy[Roll(static_cast<uint32_t>(noisy.size()))] ^= static_cast<uint8_t>(1u << Roll(8));
                }
                copy.Apply(noisy, packet.tick);
            }
            size_t applied = 0;
            const DecodeStatus status = model.receiver->Apply(packet.bytes, packet.tick, &applied);
            if (status == DecodeStatus::Ok) {
                ++outcome_.applied_ok;
                if (!(lossy && Chance(params_.ack_loss))) {
                    acks_.push_back(Ack{now + Roll(3), packet.connection, packet.tick});
                }
            } else if (status == DecodeStatus::EventsMissing) {
                retry.push_back(DataPacket{now + 1, packet.connection, packet.tick, packet.bytes});
            } else {
                ++outcome_.rejected;
            }
        }
        data_.insert(data_.end(), retry.begin(), retry.end());
        CheckInvariants(now);
        for (size_t i = 0; i < acks_.size();) {
            if (acks_[i].deliver_at <= now) {
                engine_.Confirm(acks_[i].connection, acks_[i].tick);
                acks_.erase(acks_.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                ++i;
            }
        }
    }

    // What a client holds is always a state the server had for that entity, and never goes back in time.
    void CheckInvariants(uint64_t) {
        for (uint32_t c = 0; c < params_.connections; ++c) {
            ConnectionModel& model = connections_[c];
            for (uint32_t id = 0; id < kIds; ++id) {
                const uint32_t* words = model.receiver->Current(id);
                if (words == nullptr) {
                    continue;
                }
                const EntityId entity = EntityId::FromRaw(model.client_entity[id]);
                TESSERA_CHECK(!entity.IsNull());
                const std::vector<Words>& list = history_[entity.raw()];
                const Words held = [&] {
                    Words w;
                    std::copy(words, words + 9, w.begin());
                    return w;
                }();
                TESSERA_CHECK(std::find(list.begin(), list.end(), held) != list.end());
                const uint64_t tick = model.receiver->CurrentTick(id);
                TESSERA_CHECK(tick >= model.last_tick[id]);
                model.last_tick[id] = tick;
            }
        }
    }

    bool CheckConverged() {
        bool ok = true;
        for (uint32_t c = 0; c < params_.connections; ++c) {
            ConnectionModel& model = connections_[c];
            for (const auto& [raw, compact_id] : model.visible) {
                const EntityId entity = EntityId::FromRaw(raw);
                const uint32_t* server = engine_.Store().Find(entity);
                const uint32_t* client = model.receiver->Current(compact_id);
                const bool same =
                    server != nullptr && client != nullptr && std::equal(server, server + 9, client);
                TESSERA_CHECK(same);
                ok = ok && same;
                TESSERA_CHECK(engine_.Connection(c)->CompactIdOf(entity) == compact_id);
            }
            for (uint32_t id = 0; id < kIds; ++id) {
                const bool known_to_server = std::any_of(model.visible.begin(), model.visible.end(),
                                                         [&](const auto& pair) { return pair.second == id; });
                if (!known_to_server) {
                    TESSERA_CHECK(model.receiver->Current(id) == nullptr);
                    ok = ok && model.receiver->Current(id) == nullptr;
                }
            }
        }
        return ok;
    }

    Params params_;
    ReplicationSchema schema_ = MakeTestSchema();
    ThreadPoolJobSystem jobs_;
    ReplicationEngine engine_;
    Table table_;
    std::mt19937 random_;
    std::vector<EntityId> alive_;
    std::vector<ConnectionModel> connections_;
    std::vector<DataPacket> data_;
    std::vector<Ack> acks_;
    std::map<uint64_t, std::vector<Words>> history_;
    Outcome outcome_;
    uint64_t hash_ = 0xCBF29CE484222325ull;
};

} // namespace

TESSERA_TEST(lossy_channel_keeps_every_client_on_a_state_the_server_had) {
    uint64_t ok = 0, rejected = 0;
    for (uint32_t seed = 1; seed <= 20; ++seed) {
        Params params;
        params.seed = seed;
        World world(params);
        const Outcome outcome = world.Run();
        TESSERA_CHECK(outcome.converged);
        ok += outcome.applied_ok;
        rejected += outcome.rejected;
    }
    TESSERA_CHECK(ok > 1000);    // the test is not vacuous: plenty of buffers were applied...
    TESSERA_CHECK(rejected > 0); // ...and some were refused (late, stale or for unknown ids)
}

TESSERA_TEST(reused_compact_ids_never_inherit_state) {
    // Few entities, few ids and constant visibility changes: the same numbers are handed out again and again.
    for (uint32_t seed = 100; seed < 110; ++seed) {
        Params params;
        params.seed = seed;
        params.entities = 6;
        params.connections = 2;
        params.ticks = 200;
        params.toggle = 0.4;
        params.destroy = 0.05;
        World world(params);
        TESSERA_CHECK(world.Run().converged);
    }
}

TESSERA_TEST(clean_channel_converges_immediately_and_stopped_entities_cost_nothing) {
    Params params;
    params.loss = 0.0;
    params.duplicate = 0.0;
    params.ack_loss = 0.0;
    World world(params);
    const Outcome outcome = world.Run();
    TESSERA_CHECK(outcome.converged && outcome.idle_is_free);
}

TESSERA_TEST(stopped_entities_cost_nothing_after_a_lossy_run) {
    for (uint32_t seed = 200; seed < 205; ++seed) {
        Params params;
        params.seed = seed;
        World world(params);
        const Outcome outcome = world.Run();
        TESSERA_CHECK(outcome.converged && outcome.idle_is_free);
    }
}

TESSERA_TEST(bytes_are_identical_with_1_and_8_workers_in_a_random_world) {
    for (uint32_t seed = 300; seed < 305; ++seed) {
        Params one;
        one.seed = seed;
        one.workers = 1;
        Params eight = one;
        eight.workers = 8;
        World a(one);
        World b(eight);
        TESSERA_CHECK(a.Run().hash == b.Run().hash);
    }
}

TESSERA_TEST(destroyed_slot_reuse_sends_a_complete_record) {
    const ReplicationSchema schema = MakeTestSchema();
    ThreadPoolJobSystem jobs(1);
    ReplicationConfig config;
    config.max_connections = 1;
    config.max_visible_per_connection = 16;
    ReplicationEngine engine(schema, config, jobs);
    ReceiverState receiver(schema, 16, 32);
    receiver.AdvanceEvents(~0ull); // events are applied right here, in order
    Table table{TableConfig{0, kMobType}};
    engine.AddConnection(0);
    const EntityId first = table.Create();
    table.Columns(0).x[first.index()] = 5.0f;
    table.MarkDirty(first);
    engine.Refresh(table);
    VisibilityEvent enter{VisibilityEvent::Kind::Enter, first, 0};
    engine.ApplyVisibility(0, std::span<const VisibilityEvent>(&enter, 1));
    receiver.OnEnter(0, first, 1);
    engine.Update(1);
    TESSERA_CHECK(receiver.Apply(engine.Output(0), 1) == DecodeStatus::Ok);
    engine.Confirm(0, 1);

    TESSERA_CHECK(table.Destroy(first));
    const VisibilityEvent destroy{VisibilityEvent::Kind::Destroy, first, 0};
    engine.ApplyVisibility(0, std::span<const VisibilityEvent>(&destroy, 1));
    receiver.OnLeave(0);
    engine.Update(2);
    TESSERA_CHECK(engine.LastStats().records == 0); // nothing is written for the destroyed entity

    const EntityId second = table.Create(); // the same slot, another generation
    TESSERA_CHECK(second.index() == first.index() && second.generation() != first.generation());
    table.Columns(0).x[second.index()] = 99.0f;
    table.MarkDirty(second);
    engine.Refresh(table);
    VisibilityEvent enter_again{VisibilityEvent::Kind::Enter, second, 0};
    engine.ApplyVisibility(0, std::span<const VisibilityEvent>(&enter_again, 1));
    receiver.OnEnter(0, second, 3);
    engine.Update(3);
    TESSERA_CHECK(engine.LastStats().records == 1 && engine.LastStats().complete_records == 1);
    TESSERA_CHECK(receiver.Apply(engine.Output(0), 3) == DecodeStatus::Ok);
    const uint32_t* stored = engine.Store().Find(second);
    TESSERA_CHECK(stored != nullptr && std::equal(stored, stored + 9, receiver.Current(0)));
    TESSERA_CHECK(engine.Store().Find(first) == nullptr);
}

TESSERA_TEST(states_that_return_to_earlier_values_converge_over_a_lossy_channel) {
    // Nine entities and few values: the same states come back again and again, which is where a shortcut that
    // trusts the newest send alone would leave a client on an intermediate state.
    uint64_t ok = 0;
    for (uint32_t seed = 400; seed < 420; ++seed) {
        Params params;
        params.seed = seed;
        params.entities = 9;
        params.connections = 2;
        params.ticks = 240;
        params.settle_every = 12;
        params.loss = 0.6; // heavy loss: every copy of the right state in a stretch can be lost
        params.few_values = true;
        World world(params);
        const Outcome outcome = world.Run();
        TESSERA_CHECK(outcome.converged && outcome.idle_is_free);
        ok += outcome.applied_ok;
    }
    TESSERA_CHECK(ok > 500);
}
