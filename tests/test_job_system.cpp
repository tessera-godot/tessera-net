// SPDX-License-Identifier: Apache-2.0
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "tessera/net/job_system.hpp"
#include "test_support.hpp"

using tessera::net::ThreadPoolJobSystem;

TESSERA_TEST(parallel_for_covers_every_index_exactly_once) {
    ThreadPoolJobSystem jobs(4);
    constexpr uint32_t kCount = 1'000'003;
    std::vector<std::atomic<uint8_t>> hits(kCount);
    jobs.ParallelFor(kCount, 1000, [&](uint32_t begin, uint32_t end, uint32_t) {
        for (uint32_t i = begin; i < end; ++i) {
            hits[i].fetch_add(1, std::memory_order_relaxed);
        }
    });
    uint32_t wrong = 0;
    for (const auto& h : hits) {
        if (h.load() != 1) {
            ++wrong;
        }
    }
    TESSERA_CHECK(wrong == 0);
}

TESSERA_TEST(parallel_for_zero_count_does_not_call_body) {
    ThreadPoolJobSystem jobs(4);
    std::atomic<int> calls{0};
    jobs.ParallelFor(0, 16, [&](uint32_t, uint32_t, uint32_t) { calls.fetch_add(1); });
    TESSERA_CHECK(calls.load() == 0);
}

TESSERA_TEST(parallel_for_grain_larger_than_count_runs_one_block) {
    ThreadPoolJobSystem jobs(4);
    std::atomic<int> calls{0};
    std::atomic<uint32_t> covered{0};
    jobs.ParallelFor(10, 1000, [&](uint32_t begin, uint32_t end, uint32_t) {
        calls.fetch_add(1);
        covered.fetch_add(end - begin);
    });
    TESSERA_CHECK(calls.load() == 1);
    TESSERA_CHECK(covered.load() == 10);
}

TESSERA_TEST(single_worker_pool_runs_on_caller_thread) {
    ThreadPoolJobSystem jobs(1);
    TESSERA_CHECK(jobs.WorkerCount() == 1);
    const std::thread::id caller = std::this_thread::get_id();
    bool all_on_caller = true;
    bool all_worker_zero = true;
    jobs.ParallelFor(100, 7, [&](uint32_t, uint32_t, uint32_t worker) {
        all_on_caller = all_on_caller && std::this_thread::get_id() == caller;
        all_worker_zero = all_worker_zero && worker == 0;
    });
    TESSERA_CHECK(all_on_caller);
    TESSERA_CHECK(all_worker_zero);
}

TESSERA_TEST(zero_workers_is_treated_as_one) {
    ThreadPoolJobSystem jobs(0);
    TESSERA_CHECK(jobs.WorkerCount() == 1);
}

TESSERA_TEST(worker_index_is_within_range) {
    ThreadPoolJobSystem jobs(4);
    std::atomic<uint32_t> max_worker{0};
    jobs.ParallelFor(100'000, 10, [&](uint32_t, uint32_t, uint32_t worker) {
        uint32_t seen = max_worker.load();
        while (worker > seen && !max_worker.compare_exchange_weak(seen, worker)) {
        }
    });
    TESSERA_CHECK(max_worker.load() < jobs.WorkerCount());
}

TESSERA_TEST(parallel_for_can_be_called_repeatedly) {
    ThreadPoolJobSystem jobs(4);
    std::atomic<uint64_t> total{0};
    for (int round = 0; round < 100; ++round) {
        jobs.ParallelFor(1000, 50, [&](uint32_t begin, uint32_t end, uint32_t) { total.fetch_add(end - begin); });
    }
    TESSERA_CHECK(total.load() == 100u * 1000u);
}

TESSERA_TEST(pool_destructs_cleanly_when_idle) {
    int constructed = 0;
    for (int i = 0; i < 20; ++i) {
        ThreadPoolJobSystem jobs(4);
        constructed += jobs.WorkerCount() == 4 ? 1 : 0;
    }
    TESSERA_CHECK(constructed == 20);
}
