// SPDX-License-Identifier: Apache-2.0
// Compares the Tessera Net entity table with a general-purpose entity-component-system (EnTT), on one
// thread, with one million entities. The EnTT side uses its default registry and views.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <random>
#include <vector>

#include <entt/entt.hpp>

#include "tessera/net/entity_table.hpp"

using namespace tessera::net;
using Clock = std::chrono::steady_clock;

namespace {

constexpr uint32_t kEntities = 1'000'000;
constexpr int kRuns = 5;
constexpr int kPasses = 20;

struct Columns {
    float px[kPageSlots], py[kPageSlots], pz[kPageSlots];
    float vx[kPageSlots], vy[kPageSlots], vz[kPageSlots];
    float health[kPageSlots];
};
using Table = EntityTable<Columns>;

struct Position { float x, y, z; };
struct Velocity { float x, y, z; };
struct Health { float v; };
struct NetState { bool dirty; };
struct DirtyTag {};

double Ms(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

double Median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

std::vector<uint32_t> Permutation(uint32_t n, uint32_t seed) {
    std::vector<uint32_t> v(n);
    std::iota(v.begin(), v.end(), 0u);
    std::mt19937 rng(seed);
    std::shuffle(v.begin(), v.end(), rng);
    return v;
}

entt::entity MakeEnttEntity(entt::registry& registry) {
    const entt::entity e = registry.create();
    registry.emplace<Position>(e, 0.f, 0.f, 0.f);
    registry.emplace<Velocity>(e, 1.f, 2.f, 3.f);
    registry.emplace<Health>(e, 100.f);
    registry.emplace<NetState>(e, false);
    return e;
}

}  // namespace

int main() {
    std::vector<uint8_t> out(static_cast<size_t>(kEntities) * 10);
    const uint32_t tenth = kEntities / 10;
    uint64_t sink = 0;

    // Permutations are prepared once so that generating them is not part of any timing.
    std::vector<std::vector<uint32_t>> churn_picks;
    for (int round = 0; round < 10; ++round) {
        churn_picks.push_back(Permutation(kEntities, 100 + static_cast<uint32_t>(round)));
    }

    std::vector<double> table_create, table_destroy, table_churn;
    std::vector<double> entt_create, entt_destroy, entt_churn;
    for (int run = 0; run < kRuns; ++run) {
        {
            Table table(TableConfig{});
            std::vector<EntityId> ids(kEntities, EntityId::Null());
            const auto a = Clock::now();
            for (EntityId& id : ids) id = table.Create();
            const auto b = Clock::now();
            for (const EntityId id : ids) table.Destroy(id);
            const auto c = Clock::now();
            table_create.push_back(Ms(a, b));
            table_destroy.push_back(Ms(b, c));

            for (EntityId& id : ids) id = table.Create();
            const auto d = Clock::now();
            for (int round = 0; round < 10; ++round) {
                const auto& pick = churn_picks[static_cast<size_t>(round)];
                for (uint32_t i = 0; i < tenth; ++i) table.Destroy(ids[pick[i]]);
                for (uint32_t i = 0; i < tenth; ++i) ids[pick[i]] = table.Create();
            }
            table_churn.push_back(Ms(d, Clock::now()));
        }
        {
            entt::registry registry;
            std::vector<entt::entity> entities(kEntities);
            const auto a = Clock::now();
            for (entt::entity& e : entities) e = MakeEnttEntity(registry);
            const auto b = Clock::now();
            for (const entt::entity e : entities) registry.destroy(e);
            const auto c = Clock::now();
            entt_create.push_back(Ms(a, b));
            entt_destroy.push_back(Ms(b, c));

            for (entt::entity& e : entities) e = MakeEnttEntity(registry);
            const auto d = Clock::now();
            for (int round = 0; round < 10; ++round) {
                const auto& pick = churn_picks[static_cast<size_t>(round)];
                for (uint32_t i = 0; i < tenth; ++i) registry.destroy(entities[pick[i]]);
                for (uint32_t i = 0; i < tenth; ++i) entities[pick[i]] = MakeEnttEntity(registry);
            }
            entt_churn.push_back(Ms(d, Clock::now()));
        }
    }

    // Iteration and serialization of changed entities, on a steady population.
    Table table(TableConfig{});
    std::vector<EntityId> ids(kEntities, EntityId::Null());
    for (EntityId& id : ids) id = table.Create();
    entt::registry registry;
    std::vector<entt::entity> entities(kEntities);
    for (entt::entity& e : entities) e = MakeEnttEntity(registry);

    std::vector<double> table_iterate, entt_iterate;
    std::vector<double> table_serialize, entt_flag_serialize, entt_tag_serialize;
    for (int run = 0; run < kRuns; ++run) {
        auto a = Clock::now();
        for (int pass = 0; pass < kPasses; ++pass) {
            for (uint32_t page = 0; page < table.PageCount(); ++page) {
                Columns& c = table.Columns(page);
                table.ForEachAliveInPage(page, [&](uint32_t slot) {
                    c.px[slot] += c.vx[slot] * 0.033f;
                    c.py[slot] += c.vy[slot] * 0.033f;
                    c.pz[slot] += c.vz[slot] * 0.033f;
                });
            }
        }
        auto b = Clock::now();
        for (int pass = 0; pass < kPasses; ++pass) {
            registry.view<Position, const Velocity>().each([](Position& p, const Velocity& v) {
                p.x += v.x * 0.033f;
                p.y += v.y * 0.033f;
                p.z += v.z * 0.033f;
            });
        }
        auto c = Clock::now();
        table_iterate.push_back(Ms(a, b) / kPasses);
        entt_iterate.push_back(Ms(b, c) / kPasses);

        // Mark 10% of the entities as changed, then write id and quantized position for each of them.
        const auto pick = Permutation(kEntities, 500 + static_cast<uint32_t>(run));
        for (uint32_t i = 0; i < tenth; ++i) {
            table.MarkDirty(ids[pick[i]]);
            registry.get<NetState>(entities[pick[i]]).dirty = true;
        }
        const auto s0 = Clock::now();
        size_t n = 0;
        for (uint32_t page = 0; page < table.PageCount(); ++page) {
            const Columns& cols = table.Columns(page);
            table.ConsumeDirtyInPage(page, [&](uint32_t slot) {
                const uint32_t id = page * kPageSlots + slot;
                const int16_t q[3] = {int16_t(cols.px[slot] * 16.0f), int16_t(cols.py[slot] * 16.0f),
                                      int16_t(cols.pz[slot] * 16.0f)};
                std::memcpy(out.data() + n, &id, 4);
                std::memcpy(out.data() + n + 4, q, 6);
                n += 10;
            });
        }
        sink += n;
        const auto s1 = Clock::now();
        n = 0;
        registry.view<NetState, const Position>().each([&](entt::entity e, NetState& state, const Position& p) {
            if (!state.dirty) return;
            state.dirty = false;
            const uint32_t id = entt::to_integral(e);
            const int16_t q[3] = {int16_t(p.x * 16.0f), int16_t(p.y * 16.0f), int16_t(p.z * 16.0f)};
            std::memcpy(out.data() + n, &id, 4);
            std::memcpy(out.data() + n + 4, q, 6);
            n += 10;
        });
        sink += n;
        const auto s2 = Clock::now();
        // Variant: a tag component marks the changed entities. Adding the tag is a structural change.
        for (uint32_t i = 0; i < tenth; ++i) registry.emplace_or_replace<DirtyTag>(entities[pick[i]]);
        n = 0;
        registry.view<DirtyTag, const Position>().each([&](entt::entity e, const Position& p) {
            const uint32_t id = entt::to_integral(e);
            const int16_t q[3] = {int16_t(p.x * 16.0f), int16_t(p.y * 16.0f), int16_t(p.z * 16.0f)};
            std::memcpy(out.data() + n, &id, 4);
            std::memcpy(out.data() + n + 4, q, 6);
            n += 10;
        });
        registry.clear<DirtyTag>();
        sink += n;
        const auto s3 = Clock::now();
        table_serialize.push_back(Ms(s0, s1));
        entt_flag_serialize.push_back(Ms(s1, s2));
        entt_tag_serialize.push_back(Ms(s2, s3));
    }

    std::printf("Entity table vs EnTT, %u entities, one thread, median of %d runs\n\n", kEntities, kRuns);
    std::printf("%-44s %10s %10s\n", "operation (ms)", "table", "EnTT");
    std::printf("%-44s %10.2f %10.2f\n", "create 1 M", Median(table_create), Median(entt_create));
    std::printf("%-44s %10.2f %10.2f\n", "destroy 1 M", Median(table_destroy), Median(entt_destroy));
    std::printf("%-44s %10.2f %10.2f\n", "churn (replace 10%, ten rounds)", Median(table_churn), Median(entt_churn));
    std::printf("%-44s %10.2f %10.2f\n", "iterate 1 M (one pass)", Median(table_iterate), Median(entt_iterate));
    std::printf("%-44s %10.2f %10.2f\n", "write the 10% that changed (dirty flag)", Median(table_serialize), Median(entt_flag_serialize));
    std::printf("%-44s %10s %10.2f\n", "write the 10% that changed (tag component)", "-", Median(entt_tag_serialize));
    std::printf("(sink %llu)\n", static_cast<unsigned long long>(sink));
    return 0;
}
