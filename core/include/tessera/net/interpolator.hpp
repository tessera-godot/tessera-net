// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "tessera/net/replication/property_schema.hpp"
#include "tessera/net/replication/receiver_state.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace tessera::net {

struct InterpolationConfig {
    /// How many ticks behind the newest state the entities are shown (0 to 30).
    uint32_t delay_ticks = 2;
    /// Two states of an entity that are further apart than this many world units (all its positions
    /// counted together) are not blended: the entity stays where it was and moves to the new state when
    /// the shown time reaches it.
    double snap_distance = 2.5;
    /// Two states of an entity that are more than this many ticks apart (1 to 255) are not blended either.
    uint32_t max_gap_ticks = 8;
};

struct InterpolationStats {
    uint64_t states = 0;         ///< States kept.
    uint64_t states_ignored = 0; ///< States not kept: a tick already held, older than a full history, or
                                 ///< for an entity that is not visible.
    uint64_t gap_holds = 0;      ///< States not blended with the one before because of the gap in ticks.
    uint64_t snaps = 0;          ///< States not blended with the one before because of the distance.
    uint64_t stalls = 0;         ///< Times the shown time had to wait for a state that had not arrived.
    uint64_t resyncs = 0;        ///< Times the shown time jumped forward to catch up with the states.
};

/// What the client shows of the entities it does not control: a little in the past, smoothed between the two
/// states received on either side of the time shown. It keeps a short history of states per visible entity
/// and a clock of its own, and it reads no real time: whoever calls `Advance` says how much time passed.
///
/// It is fed by `ReceiverState::Apply` (it is an `IRecordObserver`), so that it holds what the client holds,
/// late states included, and by `ClientReplica`, which says when entities come and go.
///
/// Storage is a pool for each entity type, taken when an entity becomes visible and given back when it
/// leaves: nothing is allocated when a state arrives or a frame is shown.
class Interpolator final : public IRecordObserver {
public:
    static constexpr uint32_t kMaxDelayTicks = 30;
    /// States kept per entity beyond the delay: 2 for the slack of the clock, 4 for how far it may lag before
    /// it jumps, and one to close the segment being shown.
    static constexpr uint32_t kHistorySlack = 6;
    static constexpr double kBandTicks = 2.0;   ///< How far the shown time may drift from its target.
    static constexpr double kRateAdjust = 0.1;  ///< How much faster or slower it runs outside the band.
    static constexpr double kResyncTicks = 4.0; ///< Lagging further than this, it jumps to the target.

    /// The schema must outlive the interpolator.
    explicit Interpolator(const ReplicationSchema& schema);

    /// Turns interpolation on with `config`, forgetting every state (call `Reset` after it to size the list
    /// of entities). Returns false, changing nothing and saying why in `error` when given, for a delay above
    /// 30, a snap distance that is not a number above 0, or a gap limit outside 1 to 255.
    bool Configure(const InterpolationConfig& config, std::string* error);

    /// Turns interpolation off and gives everything back.
    void Clear() noexcept;

    /// Forgets the states, the clock and the counters, and makes room for `max_visible` entities (0 gives
    /// the room back), keeping the configuration. Does nothing when interpolation is off.
    void Reset(uint32_t max_visible);

    bool Enabled() const noexcept {
        return enabled_;
    }
    const InterpolationConfig& Config() const noexcept {
        return config_;
    }
    const InterpolationStats& Stats() const noexcept {
        return stats_;
    }
    /// How many states are kept per entity: the delay plus `kHistorySlack`.
    uint32_t Capacity() const noexcept {
        return config_.delay_ticks + kHistorySlack;
    }

    /// An entity of `type` became visible under `compact_id`. False for an id out of range, a type without
    /// schema, or interpolation off. An id that was in use is released first.
    bool OnEnter(uint32_t compact_id, uint32_t type);
    /// The entity under `compact_id` is gone.
    void OnLeave(uint32_t compact_id) noexcept;
    /// A state of the entity arrived (`words` of its type, copied).
    void OnRecord(uint32_t compact_id, uint64_t tick, const uint32_t* words) override;

    /// How many states are kept for an entity, and the tick of one of them (0 is the oldest).
    size_t StateCount(uint32_t compact_id) const noexcept;
    uint64_t StateTick(uint32_t compact_id, uint32_t index) const noexcept;
    /// The words of one of the states, or nullptr.
    const uint32_t* StateWords(uint32_t compact_id, uint32_t index) const noexcept;
    /// Whether the state at `index` is blended with the one before it. False for the oldest.
    bool Linked(uint32_t compact_id, uint32_t index) const noexcept;

    /// The memory held now: the lists, the pools and what they keep free.
    size_t MemoryBytes() const noexcept;

private:
    struct Entity {
        const TypeSchema* type = nullptr;
        uint32_t chunk = kNoChunk;
        uint32_t count = 0;
    };
    static constexpr uint32_t kNoChunk = 0xFFFFFFFFu;
    static constexpr uint32_t kChunksPerBlock = 128;

    // The room for the states of one entity: `Capacity()` ticks, that many times `words` words, and as many
    // marks.
    struct Block {
        std::unique_ptr<uint64_t[]> ticks;
        std::unique_ptr<uint32_t[]> words;
        std::unique_ptr<uint8_t[]> links;
    };
    struct Pool {
        std::vector<Block> blocks;
        std::vector<uint32_t> free; // chunks not in use
    };
    struct View {
        uint64_t* ticks;
        uint32_t* words;
        uint8_t* links;
    };

    View Locate(const Entity& entity) const noexcept;
    bool GrowPool(Pool& pool, uint32_t words);
    void Release(Entity& entity) noexcept;
    bool Link(const TypeSchema& type, uint64_t before_tick, const uint32_t* before, uint64_t tick,
              const uint32_t* words, bool* gap_failed) const noexcept;

    const ReplicationSchema& schema_;
    bool enabled_ = false;
    InterpolationConfig config_;
    InterpolationStats stats_;
    std::vector<Entity> entities_;
    std::vector<Pool> pools_; // by type
};

} // namespace tessera::net
