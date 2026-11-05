// SPDX-License-Identifier: Apache-2.0
#include <cstdint>
#include <vector>

#include "tessera/net/command_buffer.hpp"
#include "tessera/net/entity_table.hpp"
#include "tessera/net/job_system.hpp"
#include "test_support.hpp"

using tessera::net::CommandBuffers;
using tessera::net::EntityId;
using tessera::net::EntityTable;
using tessera::net::StructuralCommand;
using tessera::net::TableConfig;
using tessera::net::ThreadPoolJobSystem;

TESSERA_TEST(apply_order_is_by_key_regardless_of_worker) {
    CommandBuffers<int> buffers(3);
    buffers.Push(2, 5, 50);
    buffers.Push(0, 7, 70);
    buffers.Push(1, 1, 10);
    buffers.Push(0, 7, 71);  // same key on the same worker: enqueue order is kept
    std::vector<int> order;
    const size_t applied = buffers.ApplyAll([&](const int& command) { order.push_back(command); });
    TESSERA_CHECK(applied == 4);
    TESSERA_CHECK((order == std::vector<int>{10, 50, 70, 71}));
}

TESSERA_TEST(apply_order_does_not_depend_on_which_worker_pushed) {
    // The same commands pushed by different workers must be applied in the same order.
    CommandBuffers<int> one_worker(1);
    CommandBuffers<int> many_workers(4);
    for (uint64_t key = 0; key < 100; ++key) {
        one_worker.Push(0, key, static_cast<int>(key));
        many_workers.Push(static_cast<uint32_t>((key * 7) % 4), key, static_cast<int>(key));
    }
    std::vector<int> a;
    std::vector<int> b;
    one_worker.ApplyAll([&](const int& command) { a.push_back(command); });
    many_workers.ApplyAll([&](const int& command) { b.push_back(command); });
    TESSERA_CHECK(a == b);
}

TESSERA_TEST(apply_clears_buffers) {
    CommandBuffers<int> buffers(2);
    buffers.Push(0, 0, 1);
    buffers.Push(1, 1, 2);
    TESSERA_CHECK(buffers.ApplyAll([](const int&) {}) == 2);
    TESSERA_CHECK(buffers.ApplyAll([](const int&) {}) == 0);
    TESSERA_CHECK(buffers.PendingCount() == 0);
}

TESSERA_TEST(pending_count_sums_all_workers) {
    CommandBuffers<int> buffers(3);
    buffers.Push(0, 0, 1);
    buffers.Push(1, 1, 2);
    buffers.Push(1, 2, 3);
    buffers.Push(2, 3, 4);
    TESSERA_CHECK(buffers.PendingCount() == 4);
}

TESSERA_TEST(concurrent_push_from_all_workers_loses_nothing) {
    constexpr uint32_t kWorkers = 4;
    constexpr uint32_t kPerWorker = 100'000;
    ThreadPoolJobSystem jobs(kWorkers);
    CommandBuffers<uint32_t> buffers(kWorkers);
    jobs.ParallelFor(kWorkers * kPerWorker, 1000, [&](uint32_t begin, uint32_t end, uint32_t worker) {
        for (uint32_t i = begin; i < end; ++i) {
            buffers.Push(worker, i, i);
        }
    });
    TESSERA_CHECK(buffers.PendingCount() == kWorkers * kPerWorker);
    TESSERA_CHECK(buffers.ApplyAll([](const uint32_t&) {}) == kWorkers * kPerWorker);
}

TESSERA_TEST(double_destroy_commands_are_applied_as_given) {
    struct Columns {
        float x[tessera::net::kPageSlots];
    };
    EntityTable<Columns> table(TableConfig{});
    const EntityId id = table.Create();
    CommandBuffers<StructuralCommand> buffers(2);
    buffers.Push(0, 0, StructuralCommand{StructuralCommand::Kind::Destroy, 0, id});
    buffers.Push(1, 1, StructuralCommand{StructuralCommand::Kind::Destroy, 0, id});
    int succeeded = 0;
    int failed = 0;
    buffers.ApplyAll([&](const StructuralCommand& command) {
        (table.Destroy(command.id) ? succeeded : failed) += 1;
    });
    TESSERA_CHECK(succeeded == 1);
    TESSERA_CHECK(failed == 1);
    TESSERA_CHECK(table.AliveCount() == 0);
}
