// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace tessera::net {

/// Runs a loop body in parallel. The rest of the library depends on this interface only, so the
/// implementation can be replaced.
class IJobSystem {
public:
    /// Called with a half-open block [begin, end) and the index of the worker running it.
    using Body = std::function<void(uint32_t begin, uint32_t end, uint32_t worker)>;

    virtual ~IJobSystem() = default;

    /// Number of workers, including the calling thread (which is worker 0). At least 1.
    virtual uint32_t WorkerCount() const noexcept = 0;

    /// Splits [0, count) into blocks of up to `grain` items and runs `body` on every block.
    /// Returns only after all blocks have finished. `body` must not throw. Only one thread may
    /// call this at a time.
    virtual void ParallelFor(uint32_t count, uint32_t grain, const Body& body) = 0;
};

/// A fixed pool of worker threads that sleep between jobs.
class ThreadPoolJobSystem final : public IJobSystem {
public:
    /// `worker_count` counts the calling thread, so 1 creates no threads and runs everything on the
    /// caller. 0 is treated as 1.
    explicit ThreadPoolJobSystem(uint32_t worker_count);
    ~ThreadPoolJobSystem() override;

    ThreadPoolJobSystem(const ThreadPoolJobSystem&) = delete;
    ThreadPoolJobSystem& operator=(const ThreadPoolJobSystem&) = delete;

    /// Logical cores minus one, and at least 1.
    static uint32_t DefaultWorkerCount() noexcept;

    uint32_t WorkerCount() const noexcept override;
    void ParallelFor(uint32_t count, uint32_t grain, const Body& body) override;

private:
    void WorkerLoop(uint32_t worker);
    void RunBlocks(uint32_t worker);

    uint32_t worker_count_;
    std::vector<std::thread> threads_;

    std::mutex mutex_;
    std::condition_variable job_ready_;
    std::condition_variable job_done_;
    bool stopping_ = false;
    uint64_t job_id_ = 0;
    uint32_t workers_finished_ = 0;

    // Current job. Written under the mutex before workers are woken, then read-only.
    const Body* body_ = nullptr;
    uint32_t count_ = 0;
    uint32_t grain_ = 1;
    std::atomic<uint32_t> next_block_start_{0};
};

}  // namespace tessera::net
