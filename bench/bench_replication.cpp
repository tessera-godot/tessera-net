// SPDX-License-Identifier: Apache-2.0
// Replicated state benchmark: how fast records are written, how large they are and how much memory they take.
// There is no network and no live relevancy here: every connection is given a fixed set of visible entities,
// so the numbers are those of replicated state alone.
#include "bench_replication.hpp"

#include "bench_report.hpp"
#include "tessera/net/entity_table.hpp"
#include "tessera/net/job_system.hpp"
#include "tessera/net/replication/bit_stream.hpp"
#include "tessera/net/replication/record_codec.hpp"
#include "tessera/net/replication/replication_engine.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <random>
#include <vector>

namespace {

using namespace tessera::net;

constexpr uint32_t kEntities = 200'000;
constexpr uint32_t kConnections = 128;
constexpr uint32_t kVisible = 1500;
constexpr uint32_t kMaxVisible = 4096;
constexpr uint32_t kWarmupTicks = 9;
constexpr uint32_t kRuns = 5;
constexpr uint32_t kTicksPerRun = 20;
constexpr uint32_t kConfirmLag = 2; // ticks between a buffer being written and its confirmation coming back

struct BenchColumns {
    float x[kPageSlots];
    float y[kPageSlots];
    float z[kPageSlots];
    float yaw[kPageSlots];
    int32_t health[kPageSlots];
    uint16_t flags[kPageSlots];
    uint8_t state[kPageSlots];
};

ReplicationSchema MakeSchema() {
    ReplicationSchema schema;
    schema.AddType(
        0, {PropertyDescriptor::Position(TESSERA_COLUMN(BenchColumns, x), TESSERA_COLUMN(BenchColumns, y),
                                         TESSERA_COLUMN(BenchColumns, z), 0.01f, 10000.0f),
            PropertyDescriptor::Angle(TESSERA_COLUMN(BenchColumns, yaw), 10),
            PropertyDescriptor::Integer(TESSERA_COLUMN(BenchColumns, health), true, -1000, 1000),
            PropertyDescriptor::Bits(TESSERA_COLUMN(BenchColumns, flags), 16),
            PropertyDescriptor::Enum(TESSERA_COLUMN(BenchColumns, state), 8)});
    return schema;
}

struct Measurement {
    double update_ms = 0;  // ReplicationEngine::Update
    double confirm_ms = 0; // the confirmations of the tick
    double refresh_ms = 0; // quantizing what changed
    double records = 0;    // records written per tick, all connections
    double bytes = 0;      // bytes written per tick, all connections
};

// 128 connections, each seeing 1,500 of 200,000 entities, all of them already known to their players.
class World {
public:
    explicit World(uint32_t workers, uint32_t connections = kConnections)
        : connections_(connections), jobs_(workers), engine_(schema_, MakeConfig(connections), jobs_),
          table_(TableConfig{0, 0}) {
        std::mt19937 random(2024);
        std::uniform_real_distribution<float> position(-5000.0f, 5000.0f);
        ids_.reserve(kEntities);
        for (uint32_t i = 0; i < kEntities; ++i) {
            const EntityId id = table_.Create();
            const auto where = table_.Find(id);
            BenchColumns& columns = table_.Columns(where->page);
            columns.x[where->slot] = position(random);
            columns.y[where->slot] = 0.0f;
            columns.z[where->slot] = position(random);
            columns.yaw[where->slot] = 1.0f;
            columns.health[where->slot] = static_cast<int32_t>(i % 1000);
            columns.flags[where->slot] = static_cast<uint16_t>(i);
            columns.state[where->slot] = static_cast<uint8_t>(i % 8);
            table_.MarkDirty(id);
            ids_.push_back(id);
        }
        engine_.Refresh(table_);
        for (ConnectionId c = 0; c < connections; ++c) {
            engine_.AddConnection(c);
            std::vector<VisibilityEvent> events;
            events.reserve(kVisible);
            for (uint32_t i = 0; i < kVisible; ++i) { // 113 shares no factor with 200,000, so the ids differ
                events.push_back(VisibilityEvent{
                    VisibilityEvent::Kind::Enter,
                    ids_[(static_cast<uint64_t>(c) * 1171 + static_cast<uint64_t>(i) * 113) % kEntities], i});
            }
            engine_.ApplyVisibility(c, events);
        }
        // Send everything once and have it confirmed, so the measured ticks are the usual partial records.
        engine_.Update(tick_);
        ++tick_;
        for (ConnectionId c = 0; c < connections; ++c) {
            engine_.Confirm(c, 1);
        }
    }

    // Moves every `period`-th entity (offset by the tick) by a few centimetres, and flags it as changed.
    void Move(uint32_t period) {
        for (uint32_t i = 0; i < kEntities; ++i) {
            if (period > 1 && (i + tick_) % period != 0) {
                continue;
            }
            const auto where = table_.Find(ids_[i]);
            table_.Columns(where->page).x[where->slot] += 0.2f;
            table_.MarkDirty(ids_[i]);
        }
    }

    // One tick: move `1 / period` of the entities, quantize, write the buffers, confirm the buffers of two
    // ticks ago.
    Measurement Step(uint32_t period) {
        Move(period);
        Measurement m;
        const auto t0 = bench::Clock::now();
        engine_.Refresh(table_);
        const auto t1 = bench::Clock::now();
        engine_.Update(tick_);
        const auto t2 = bench::Clock::now();
        if (tick_ > kConfirmLag) {
            for (ConnectionId c = 0; c < connections_; ++c) {
                engine_.Confirm(c, tick_ - kConfirmLag);
            }
        }
        const auto t3 = bench::Clock::now();
        m.refresh_ms = bench::Ms(t0, t1);
        m.update_ms = bench::Ms(t1, t2);
        m.confirm_ms = bench::Ms(t2, t3);
        m.records = engine_.LastStats().records;
        for (ConnectionId c = 0; c < connections_; ++c) {
            m.bytes += static_cast<double>(engine_.Output(c).size());
        }
        ++tick_;
        return m;
    }

    // The median over kRuns runs of the mean of kTicksPerRun ticks, after some warm-up ticks.
    Measurement Measure(uint32_t period) {
        for (uint32_t i = 0; i < kWarmupTicks; ++i) {
            Step(period);
        }
        std::vector<double> update, confirm, refresh, records, bytes;
        for (uint32_t run = 0; run < kRuns; ++run) {
            Measurement sum;
            for (uint32_t i = 0; i < kTicksPerRun; ++i) {
                const Measurement m = Step(period);
                sum.update_ms += m.update_ms;
                sum.confirm_ms += m.confirm_ms;
                sum.refresh_ms += m.refresh_ms;
                sum.records += m.records;
                sum.bytes += m.bytes;
            }
            update.push_back(sum.update_ms / kTicksPerRun);
            confirm.push_back(sum.confirm_ms / kTicksPerRun);
            refresh.push_back(sum.refresh_ms / kTicksPerRun);
            records.push_back(sum.records / kTicksPerRun);
            bytes.push_back(sum.bytes / kTicksPerRun);
        }
        Measurement result;
        result.update_ms = bench::Median(update);
        result.confirm_ms = bench::Median(confirm);
        result.refresh_ms = bench::Median(refresh);
        result.records = bench::Median(records);
        result.bytes = bench::Median(bytes);
        return result;
    }

    // A fingerprint of every byte written over `ticks` ticks.
    uint64_t Fingerprint(uint32_t ticks, uint32_t period) {
        uint64_t hash = 0xCBF29CE484222325ull;
        for (uint32_t t = 0; t < ticks; ++t) {
            Step(period);
            for (ConnectionId c = 0; c < connections_; ++c) {
                for (const uint8_t byte : engine_.Output(c)) {
                    hash = (hash ^ byte) * 0x100000001B3ull;
                }
            }
        }
        return hash;
    }

    double StoreBytesPerEntity() const {
        return static_cast<double>(engine_.Store().MemoryBytes()) / kEntities;
    }
    double ConnectionBytes() const {
        double total = 0;
        for (ConnectionId c = 0; c < connections_; ++c) {
            total += static_cast<double>(engine_.Connection(c)->MemoryBytes());
        }
        // The index of who sees what is shared, so each connection carries an equal part of it.
        return total / connections_ + static_cast<double>(engine_.SubscriptionBytes()) / connections_;
    }
    size_t IdleOutputBytes() {
        size_t largest = 0;
        for (ConnectionId c = 0; c < connections_; ++c) {
            largest = std::max(largest, engine_.Output(c).size());
        }
        return largest;
    }
    uint32_t IdleRecords() const {
        return engine_.LastStats().records;
    }

private:
    static ReplicationConfig MakeConfig(uint32_t connections) {
        ReplicationConfig config;
        config.max_connections = connections;
        config.max_visible_per_connection = kMaxVisible;
        return config;
    }

    uint32_t connections_;
    ReplicationSchema schema_ = MakeSchema();
    ThreadPoolJobSystem jobs_;
    ReplicationEngine engine_;
    EntityTable<BenchColumns> table_;
    std::vector<EntityId> ids_;
    uint64_t tick_ = 2;
};

// The cost of writing one position record, the common case of replication: an id gap, the "complete" flag,
// the age of the baseline, a 3-bit property mask, the "absolute" flag and three small position steps. Only
// the writing is timed here; comparing states and looking them up come on top (see the full benchmark below).
double MeasureRecordWriteNs(double* bits_per_record) {
    constexpr uint32_t kRecords = 1'000'000;
    constexpr uint32_t kPerBuffer = 1000;
    std::mt19937 random(12345);
    std::uniform_int_distribution<int> step(-30, 30);
    std::vector<int8_t> steps(static_cast<size_t>(kRecords) * 3);
    for (int8_t& s : steps) {
        s = static_cast<int8_t>(step(random));
    }
    std::vector<double> runs;
    uint64_t sink = 0;
    uint64_t bits = 0;
    for (int repeat = 0; repeat < 5; ++repeat) {
        std::vector<uint8_t> buffer;
        buffer.reserve(8192);
        bits = 0;
        const auto start = bench::Clock::now();
        for (uint32_t base = 0; base < kRecords; base += kPerBuffer) {
            buffer.clear();
            BitWriter writer(buffer);
            for (uint32_t i = 0; i < kPerBuffer; ++i) {
                const size_t at = static_cast<size_t>(base + i) * 3;
                writer.WriteVarUInt(1 + (i & 3));
                writer.WriteBool(false);
                writer.WriteVarUInt(1);
                writer.WriteBits(0x1, 3);
                writer.WriteBool(false);
                writer.WriteVarInt(steps[at]);
                writer.WriteVarInt(steps[at + 1]);
                writer.WriteVarInt(steps[at + 2]);
            }
            bits += writer.BitCount();
            writer.Finish();
            sink += buffer.size();
        }
        const auto end = bench::Clock::now();
        runs.push_back(bench::Ms(start, end) * 1.0e6 / kRecords);
    }
    if (sink == 0) {
        std::printf("unexpected empty output\n");
    }
    *bits_per_record = static_cast<double>(bits) / kRecords;
    return bench::Median(runs);
}

// Building and writing one whole position record (`WriteRecord`) with its data already in the processor
// cache: the work of a record without the cost of finding the entity's state in memory.
double MeasureWriteRecordNs() {
    const ReplicationSchema schema = MakeSchema();
    const TypeSchema& type = *schema.Find(0);
    std::vector<uint32_t> bases(1000 * 9), currents(1000 * 9);
    for (int i = 0; i < 1000; ++i) {
        for (int w = 0; w < 9; ++w) {
            bases[i * 9 + w] = 1'000'000 + w;
            currents[i * 9 + w] = bases[i * 9 + w];
        }
        for (int axis = 0; axis < 3; ++axis) {
            currents[i * 9 + axis] += 20; // a move of 20 steps on every axis
        }
    }
    std::vector<uint8_t> buffer;
    buffer.reserve(1 << 16);
    std::vector<double> runs;
    for (int repeat = 0; repeat < 5; ++repeat) {
        const auto start = bench::Clock::now();
        for (int round = 0; round < 1000; ++round) {
            buffer.clear();
            BitWriter writer(buffer);
            for (int i = 0; i < 1000; ++i) {
                WriteRecord(type, 1, 2, &bases[i * 9], &currents[i * 9], writer);
            }
            writer.Finish();
        }
        runs.push_back(bench::Ms(start, bench::Clock::now()) * 1.0e6 / 1.0e6);
    }
    return bench::Median(runs);
}

} // namespace

void CheckReplication() {
    double bits = 0;
    const double write_ns = MeasureRecordWriteNs(&bits);
    bench::Info("replication write of one position record", write_ns, "ns");
    bench::Info("replication bytes of one position record (bits only)", bits / 8.0, "B");
    bench::Info("replication WriteRecord of one position record, data in cache", MeasureWriteRecordNs(),
                "ns");

    std::printf(
        "replication: %u connections, %u of %u entities visible to each, confirmations %u ticks late\n",
        kConnections, kVisible, kEntities, kConfirmLag);

    // Throughput: every visible entity changes every tick, so every connection gets 1,500 records per tick.
    // The goal is 10 million updates per second per core. The first version reaches about 5.8 million, so the
    // limit below is a regression guard at about half of that, not the goal.
    World one(1);
    const Measurement single = one.Measure(1);
    const double updates_per_second_one = single.records / (single.update_ms / 1000.0) / 1.0e6;
    bench::Report("replication updates per second, 1 thread (write only)", updates_per_second_one, "M/s", 2.9,
                  true);
    bench::Info("replication updates per second, 1 thread (with confirmations)",
                single.records / ((single.update_ms + single.confirm_ms) / 1000.0) / 1.0e6, "M/s");
    bench::Info("replication records per tick, every entity changing", single.records, "records");
    bench::Info("replication refresh (quantizing 200,000 entities), 1 thread", single.refresh_ms, "ms");
    bench::Info("replication update, 1 thread, every entity changing", single.update_ms, "ms");
    bench::Info("replication confirmations, 1 thread", single.confirm_ms, "ms");

    World eight(8);
    const Measurement parallel = eight.Measure(1);
    const double efficiency = single.update_ms / (8.0 * parallel.update_ms);
    bench::Info("replication update, 8 workers, every entity changing", parallel.update_ms, "ms");
    // The goal is 70%. Marking what changed runs on one thread, so the first version reaches about 44%; the
    // limit is a regression guard at about half of that.
    bench::Report("replication efficiency with 8 workers", efficiency * 100.0, "%", 22.0, true);

    // Size: only a position changes, so this is the size of a position update (the id and the property mask
    // included, the terminator and the padding of the buffers too).
    bench::Report("replication bytes per position update", single.bytes / single.records, "B", 8.0);

    // The typical case: an entity changes about five times a second (every sixth tick at 30 Hz).
    World typical(1);
    const Measurement usual = typical.Measure(6);
    bench::Info("replication records per tick, 5 Hz updates", usual.records, "records");
    bench::Info("replication update + confirmations, 1 thread, 5 Hz updates",
                usual.update_ms + usual.confirm_ms, "ms");
    bench::Info("replication refresh, 1 thread, 5 Hz updates", usual.refresh_ms, "ms");
    bench::Info("replication bytes per position update, 5 Hz updates", usual.bytes / usual.records, "B");

    // Memory.
    bench::Report("replication memory per entity (stored state)", one.StoreBytesPerEntity(), "B", 128.0);
    bench::Report("replication memory per connection", one.ConnectionBytes() / (1024.0 * 1024.0), "MB", 4.0);

    // Nothing changes and everything is confirmed: a connection gets one byte (the terminator) and no
    // records.
    for (uint32_t i = 0; i < 6; ++i) {
        typical.Step(0xFFFFFFFFu); // no entity matches the period, so nothing moves
    }
    bench::Report("replication bytes of an idle connection per tick",
                  static_cast<double>(typical.IdleOutputBytes()), "B", 1.0);
    bench::Report("replication records of an idle tick", static_cast<double>(typical.IdleRecords()), "rec",
                  0.0);

    // The bytes are the same whatever the number of workers.
    World a(1, 16), b(8, 16);
    bench::ReportBool("replication bytes, 1 vs 8 workers, 20 ticks",
                      a.Fingerprint(20, 6) == b.Fingerprint(20, 6));
}
