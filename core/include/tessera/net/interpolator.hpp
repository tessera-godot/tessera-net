// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "tessera/net/replication/property_schema.hpp"
#include "tessera/net/replication/receiver_state.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
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
    /// States kept per entity beyond the delay: 4 for how far the shown time may lag behind its target before
    /// it jumps, one for the state at or before the shown time, and one spare.
    static constexpr uint32_t kHistorySlack = 6;
    static constexpr double kBandTicks = 2.0;    ///< The widest the band around the target gets (large delays).
    static constexpr double kMinBandTicks = 0.25; ///< The narrowest it gets (small delays).
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

    /// The newest tick of which a buffer has been applied, as the clock sees it: the first one starts the
    /// clock; one that is not newer than the last does nothing. Call it after a buffer was applied whole.
    void OnBuffer(uint64_t tick) noexcept;
    /// `ticks` of time passed since the last call (negative, zero or not finite time does nothing, and so
    /// does a clock that has not started). The shown time follows the newest tick minus the delay: it runs
    /// at full speed inside a band around that target (a quarter of the delay, between `kMinBandTicks` and
    /// `kBandTicks`), a tenth faster or slower outside it, jumps to the target when it is further behind than
    /// `kResyncTicks`, and never passes the newest tick or goes back.
    void Advance(double ticks) noexcept;
    /// Whether the clock has started (a buffer arrived since the last `Reset`).
    bool Running() const noexcept {
        return running_;
    }
    /// The tick being shown, in ticks and fractions of one; 0 until the clock starts.
    double DisplayTick() const noexcept {
        return display_;
    }
    uint64_t LatestTick() const noexcept {
        return latest_;
    }
    /// The newest tick minus the tick being shown (0 until the clock starts).
    double LagTicks() const noexcept {
        return running_ ? static_cast<double>(latest_) - display_ : 0.0;
    }

    /// The words of the state in force at the shown time: the newest one with a tick not after it (the
    /// oldest one if the shown time is before them all). Integers, enums and bit masks are read from these,
    /// so they change all at once, at the tick of the state that brings them. Nullptr for an entity that is
    /// not there or has no state, and while the clock has not started. The pointer holds until the next
    /// call that changes the states (`OnRecord`, `OnEnter`, `OnLeave`, `Reset`, `Configure`).
    const uint32_t* HeldWords(uint32_t compact_id) const noexcept;
    /// The shown value of a position (3 numbers in `out`) or an angle (1 number, radians in [0, 2 pi)),
    /// blended between the two states around the shown time when they are linked (see `Linked`), otherwise
    /// the value of the older one until the shown time reaches the newer one. Before the first state it is
    /// the first, after the last the last: nothing is extrapolated. False, writing nothing, for any other
    /// kind of property, an index outside the type, an `out` too small, an entity with no state, or a clock
    /// that has not started.
    bool Read(uint32_t compact_id, uint32_t property_index, std::span<double> out) const noexcept;

    /// How many states are kept for an entity, and the tick of one of them (0 is the oldest).
    size_t StateCount(uint32_t compact_id) const noexcept;
    uint64_t StateTick(uint32_t compact_id, uint32_t index) const noexcept;
    /// The words of one of the states, or nullptr.
    const uint32_t* StateWords(uint32_t compact_id, uint32_t index) const noexcept;
    /// Whether the state at `index` is blended with the one before it. False for the oldest.
    bool Linked(uint32_t compact_id, uint32_t index) const noexcept;

    /// How many entities hold room for states now.
    size_t InUse() const noexcept;

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

    uint32_t After(const View& view, uint32_t count) const noexcept;
    double Target() const noexcept;
    double BandTicks() const noexcept;
    void ResetClock() noexcept;

    const ReplicationSchema& schema_;
    bool enabled_ = false;
    bool running_ = false;
    double display_ = 0.0; // the tick being shown
    uint64_t latest_ = 0;  // the newest tick of which a buffer was applied
    InterpolationConfig config_;
    InterpolationStats stats_;
    std::vector<Entity> entities_;
    std::vector<Pool> pools_; // by type
};

} // namespace tessera::net
