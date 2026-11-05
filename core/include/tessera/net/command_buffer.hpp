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
/// Every command carries an `order_key` chosen by the code that queues it (for example the index of
/// the item being processed). `ApplyAll` applies commands in key order, so the result does not depend
/// on which worker handled which item or on thread timing, and it is the same with 1 or 8 workers.
///
/// Key rule: a key identifies one source of commands (for example one input item) among ALL commands
/// pushed between two `ApplyAll` calls. Several commands from the same source share its key and keep
/// their enqueue order. Two different sources must never share a key: ties between workers are broken
/// by worker index, which depends on timing, and that would make the result vary from run to run.
/// Do not call `Push` from inside the `ApplyAll` callback.
template <class Command>
class CommandBuffers {
public:
    explicit CommandBuffers(uint32_t worker_count) : lanes_(std::max(worker_count, 1u)) {}

    /// Queues a command on the lane of `worker`. Safe to call in parallel for different workers.
    void Push(uint32_t worker, uint64_t order_key, Command command) {
        assert(worker < lanes_.size());
        lanes_[worker].commands.push_back(Entry{order_key, std::move(command)});
    }

    /// Calls `fn(const Command&)` for every queued command in key order, then empties the queues.
    /// Must run alone (no concurrent `Push`). Returns the number of commands applied.
    template <class F>
    size_t ApplyAll(F&& fn) {
        order_.clear();
        for (uint32_t lane = 0; lane < lanes_.size(); ++lane) {
            for (uint32_t i = 0; i < lanes_[lane].commands.size(); ++i) {
                order_.push_back(Ref{lanes_[lane].commands[i].key, lane, i});
            }
        }
        std::sort(order_.begin(), order_.end(), [](const Ref& a, const Ref& b) {
            if (a.key != b.key) return a.key < b.key;
            if (a.lane != b.lane) return a.lane < b.lane;
            return a.index < b.index;
        });
        for (size_t i = 1; i < order_.size(); ++i) {
            // Equal keys must come from one lane (one source); see the key rule above.
            assert(order_[i].key != order_[i - 1].key || order_[i].lane == order_[i - 1].lane);
        }
        for (const Ref& ref : order_) {
            fn(lanes_[ref.lane].commands[ref.index].command);
        }
        const size_t applied = order_.size();
        for (Lane& lane : lanes_) {
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
    struct Entry {
        uint64_t key;
        Command command;
    };
    struct Ref {
        uint64_t key;
        uint32_t lane;
        uint32_t index;
    };
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)  // padding is intended: one cache line per lane avoids false sharing
#endif
    struct alignas(64) Lane {
        std::vector<Entry> commands;
    };
#ifdef _MSC_VER
#pragma warning(pop)
#endif

    std::vector<Lane> lanes_;
    std::vector<Ref> order_;  // reused between calls, so steady-state use does not allocate
};

}  // namespace tessera::net
