// SPDX-License-Identifier: Apache-2.0
// What interpolation costs on the client with 1,000 and 10,000 visible entities: the time to take in one
// tick of states, to read every position, and to advance the clock, and the memory held for each entity.
// Build with -DTESSERA_BUILD_BENCH=ON and run in Release. Exits with code 1 if a limit is passed.
#include "bench_report.hpp"
#include "tessera/net/entity_table.hpp"
#include "tessera/net/interpolator.hpp"
#include "tessera/net/replication/property_codecs.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <vector>

using namespace tessera::net;
using bench::Clock;
using bench::Median;

namespace {

// A player of the example server: a position and a 64-bit number, 5 words in all.
struct PlayerColumns {
    float x[kPageSlots];
    float y[kPageSlots];
    float z[kPageSlots];
    int64_t score[kPageSlots];
};

ReplicationSchema MakeSchema() {
    ReplicationSchema schema;
    schema.AddType(0, {PropertyDescriptor::Position(TESSERA_COLUMN(PlayerColumns, x),
                                                    TESSERA_COLUMN(PlayerColumns, y),
                                                    TESSERA_COLUMN(PlayerColumns, z), 0.01f, 2048.0f),
                       PropertyDescriptor::Integer(TESSERA_COLUMN(PlayerColumns, score), true,
                                                   std::numeric_limits<int64_t>::min(),
                                                   std::numeric_limits<int64_t>::max())});
    return schema;
}

double Micros(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::micro>(b - a).count();
}

void Measure(const ReplicationSchema& schema, uint32_t count) {
    const double scale = static_cast<double>(count) / 10000.0; // the limits are for 10,000, a tenth for 1,000
    Interpolator interpolator(schema);
    InterpolationConfig config; // the defaults: 2 ticks
    interpolator.Configure(config, nullptr);
    interpolator.Reset(count);
    for (uint32_t id = 0; id < count; ++id) {
        interpolator.OnEnter(id, 0);
    }
    const uint32_t words = schema.Find(0)->words;

    // The states of every entity: each tick it has moved 0.27 m (27 steps) along x.
    std::vector<uint32_t> states(static_cast<size_t>(count) * words);
    {
        auto columns = std::make_unique<PlayerColumns>();
        columns->x[0] = 100.0f;
        columns->y[0] = 1.0f;
        columns->z[0] = 100.0f;
        columns->score[0] = 7;
        for (const PropertyDescriptor& property : schema.Find(0)->properties) {
            QuantizeProperty(property, reinterpret_cast<const std::byte*>(columns.get()), 0,
                             states.data() + property.word_offset);
        }
        for (uint32_t id = 1; id < count; ++id) {
            std::copy(states.begin(), states.begin() + words,
                      states.begin() + static_cast<size_t>(id) * words);
        }
    }
    auto feed = [&](uint64_t tick) {
        for (uint32_t id = 0; id < count; ++id) {
            uint32_t* state = states.data() + static_cast<size_t>(id) * words;
            state[0] += 27; // the position moved: the first word is the x of the entity
            interpolator.OnRecord(id, tick, state);
        }
        interpolator.OnBuffer(tick);
    };

    uint64_t tick = 0;
    for (; tick < 20; ++tick) {
        feed(tick);
        interpolator.Advance(1.0);
    }

    std::vector<double> feed_times;
    std::vector<double> read_times;
    double sink = 0.0;
    for (int round = 0; round < 60; ++round, ++tick) {
        const auto start = Clock::now();
        feed(tick);
        const auto fed = Clock::now();
        interpolator.Advance(1.0);
        double out[3];
        const auto before_reading = Clock::now();
        for (uint32_t id = 0; id < count; ++id) {
            if (interpolator.Read(id, 0, out)) {
                sink += out[0];
            }
        }
        const auto read = Clock::now();
        feed_times.push_back(Micros(start, fed));
        read_times.push_back(Micros(before_reading, read));
    }

    std::vector<double> advance_times;
    for (int round = 0; round < 41; ++round) {
        constexpr int kCalls = 100000;
        const auto start = Clock::now();
        for (int call = 0; call < kCalls; ++call) {
            interpolator.Advance(0.0001);
        }
        advance_times.push_back(Micros(start, Clock::now()) / kCalls);
    }

    char label[96];
    const double feed_tick = Median(feed_times);
    const double read_all = Median(read_times);
    std::snprintf(label, sizeof(label), "%u entities: take in one tick of states", count);
    bench::Report(label, feed_tick, "us", 1000.0 * scale);
    std::snprintf(label, sizeof(label), "%u entities: per state taken in", count);
    bench::Info(label, feed_tick * 1000.0 / count, "ns");
    std::snprintf(label, sizeof(label), "%u entities: read every position", count);
    bench::Report(label, read_all, "us", 500.0 * scale);
    std::snprintf(label, sizeof(label), "%u entities: per position read", count);
    bench::Info(label, read_all * 1000.0 / count, "ns");
    std::snprintf(label, sizeof(label), "%u entities: advance the clock", count);
    bench::Report(label, Median(advance_times) * 1000.0, "ns", 10000.0);
    std::snprintf(label, sizeof(label), "%u entities: memory for each", count);
    bench::Report(label, static_cast<double>(interpolator.MemoryBytes()) / count, "B", 384.0);
    if (sink < 0.0) {
        std::printf("%f\n", sink); // so that the reads are not optimized away
    }
}

} // namespace

int main() {
    const ReplicationSchema schema = MakeSchema();
    Measure(schema, 1000);
    Measure(schema, 10000);
    std::printf("%s\n", bench::Failures() == 0 ? "all limits met" : "a limit was passed");
    return bench::Failures() == 0 ? 0 : 1;
}
