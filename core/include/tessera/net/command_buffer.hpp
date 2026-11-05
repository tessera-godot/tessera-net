// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "tessera/net/entity_id.hpp"

namespace tessera::net {

/// A request to create or destroy an entity, queued during the simulation phase and applied in the
/// structure phase, when no other thread touches the tables.
struct StructuralCommand {
    enum class Kind : uint8_t { Create, Destroy };

    Kind kind;
    uint32_t type;  ///< Entity type to create (used by Create).
    EntityId id;    ///< Entity to destroy (used by Destroy).
};

/// One queue per worker, so workers can queue commands in parallel without locks.
///
/// `ApplyAll` visits the queues in a fixed order (worker 0 first, then worker 1, and so on, each in
/// the order its commands were queued). The result therefore does not depend on thread timing.
template <class Command>
class CommandBuffers {
public:
    explicit CommandBuffers(uint32_t worker_count) : lanes_(std::max(worker_count, 1u)) {}

    /// Queues a command on the lane of `worker`. Safe to call in parallel for different workers.
    void Push(uint32_t worker, Command command) {
        assert(worker < lanes_.size());
        lanes_[worker].commands.push_back(std::move(command));
    }

    /// Calls `fn(const Command&)` for every queued command in deterministic order, then empties the
    /// queues. Must run alone (no concurrent `Push`). Returns the number of commands applied.
    template <class F>
    size_t ApplyAll(F&& fn) {
        size_t applied = 0;
        for (Lane& lane : lanes_) {
            for (const Command& command : lane.commands) {
                fn(command);
                ++applied;
            }
            lane.commands.clear();
        }
        return applied;
    }

    size_t PendingCount() const noexcept {
        size_t total = 0;
        for (const Lane& lane : lanes_) {
            total += lane.commands.size();
        }
        return total;
    }

private:
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)  // padding is intended: one cache line per lane avoids false sharing
#endif
    struct alignas(64) Lane {
        std::vector<Command> commands;
    };
#ifdef _MSC_VER
#pragma warning(pop)
#endif

    std::vector<Lane> lanes_;
};

}  // namespace tessera::net
