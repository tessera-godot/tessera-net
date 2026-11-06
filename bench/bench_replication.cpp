// SPDX-License-Identifier: Apache-2.0
// Replicated state benchmark: how fast records are written and read, and how large they are.
#include "bench_replication.hpp"

#include "bench_report.hpp"
#include "tessera/net/replication/bit_stream.hpp"

#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

namespace {

using tessera::net::BitWriter;

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

} // namespace

void CheckReplication() {
    double bits = 0;
    const double write_ns = MeasureRecordWriteNs(&bits);
    // The goal of 10 million updates per second per core leaves 100 ns for everything; writing may take half.
    bench::Report("replication write of one position record", write_ns, "ns", 50.0);
    bench::Info("replication bytes of one position record", bits / 8.0, "B");
}
