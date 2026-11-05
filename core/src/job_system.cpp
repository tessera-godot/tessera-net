// SPDX-License-Identifier: Apache-2.0
#include "tessera/net/job_system.hpp"

#include <algorithm>

namespace tessera::net {

ThreadPoolJobSystem::ThreadPoolJobSystem(uint32_t worker_count) : worker_count_(std::max(worker_count, 1u)) {
    threads_.reserve(worker_count_ - 1);
    for (uint32_t worker = 1; worker < worker_count_; ++worker) {
        threads_.emplace_back([this, worker] { WorkerLoop(worker); });
    }
}

ThreadPoolJobSystem::~ThreadPoolJobSystem() {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    job_ready_.notify_all();
    for (std::thread& thread : threads_) {
        thread.join();
    }
}

uint32_t ThreadPoolJobSystem::DefaultWorkerCount() noexcept {
    const uint32_t cores = std::thread::hardware_concurrency();
    return cores > 1 ? cores - 1 : 1;
}

uint32_t ThreadPoolJobSystem::WorkerCount() const noexcept {
    return worker_count_;
}

void ThreadPoolJobSystem::ParallelFor(uint32_t count, uint32_t grain, const Body& body) {
    if (count == 0) {
        return;
    }
    grain = std::max(grain, 1u);
    if (threads_.empty() || count <= grain) {
        // Not worth waking the pool: one block, on the calling thread.
        for (uint32_t begin = 0; begin < count; begin += grain) {
            body(begin, std::min(begin + grain, count), 0);
        }
        return;
    }

    {
        const std::lock_guard<std::mutex> lock(mutex_);
        body_ = &body;
        count_ = count;
        grain_ = grain;
        next_block_start_.store(0, std::memory_order_relaxed);
        workers_finished_ = 0;
        ++job_id_;
    }
    job_ready_.notify_all();

    RunBlocks(0);  // the calling thread works too

    std::unique_lock<std::mutex> lock(mutex_);
    job_done_.wait(lock, [this] { return workers_finished_ == threads_.size(); });
    body_ = nullptr;
}

void ThreadPoolJobSystem::RunBlocks(uint32_t worker) {
    while (true) {
        const uint32_t begin = next_block_start_.fetch_add(grain_, std::memory_order_relaxed);
        if (begin >= count_) {
            return;
        }
        (*body_)(begin, std::min(begin + grain_, count_), worker);
    }
}

void ThreadPoolJobSystem::WorkerLoop(uint32_t worker) {
    uint64_t last_job = 0;
    while (true) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            job_ready_.wait(lock, [&] { return stopping_ || job_id_ != last_job; });
            if (stopping_) {
                return;
            }
            last_job = job_id_;
        }
        RunBlocks(worker);
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            ++workers_finished_;
        }
        job_done_.notify_one();
    }
}

}  // namespace tessera::net
