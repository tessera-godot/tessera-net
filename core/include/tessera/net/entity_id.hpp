// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <compare>
#include <cstdint>

namespace tessera::net {

/// Identity of an entity: a 64-bit value that is unique across the whole world.
///
/// Layout (most significant bits first): shard 12 | type 8 | index 28 | generation 16.
/// The generation detects stale ids: when a slot is reused its generation changes, so an id that
/// still points to the old occupant no longer matches. Generation 0xFFFF is reserved as the
/// "retired" marker, so valid generations are 0 to 0xFFFE and the null id can never be valid.
class EntityId {
public:
    static constexpr uint32_t kMaxShard = 0xFFF;
    static constexpr uint32_t kMaxType = 0xFF;
    static constexpr uint32_t kMaxIndex = (1u << 28) - 1;
    static constexpr uint32_t kRetiredGeneration = 0xFFFF;

    /// Builds an id from its fields. Out-of-range fields are masked to their width.
    static constexpr EntityId Make(uint32_t shard, uint32_t type, uint32_t index, uint32_t generation) noexcept {
        return EntityId((static_cast<uint64_t>(shard & kMaxShard) << kShardShift) |
                        (static_cast<uint64_t>(type & kMaxType) << kTypeShift) |
                        (static_cast<uint64_t>(index & kMaxIndex) << kIndexShift) |
                        static_cast<uint64_t>(generation & kRetiredGeneration));
    }

    /// The id that refers to nothing. All bits set, so its generation is the retired marker.
    static constexpr EntityId Null() noexcept { return EntityId(~0ull); }

    static constexpr EntityId FromRaw(uint64_t raw) noexcept { return EntityId(raw); }

    constexpr uint64_t raw() const noexcept { return raw_; }
    constexpr uint32_t shard() const noexcept { return static_cast<uint32_t>(raw_ >> kShardShift) & kMaxShard; }
    constexpr uint32_t type() const noexcept { return static_cast<uint32_t>(raw_ >> kTypeShift) & kMaxType; }
    constexpr uint32_t index() const noexcept { return static_cast<uint32_t>(raw_ >> kIndexShift) & kMaxIndex; }
    constexpr uint32_t generation() const noexcept { return static_cast<uint32_t>(raw_) & kRetiredGeneration; }
    constexpr bool IsNull() const noexcept { return raw_ == ~0ull; }

    friend constexpr auto operator<=>(EntityId, EntityId) noexcept = default;

private:
    static constexpr unsigned kIndexShift = 16;
    static constexpr unsigned kTypeShift = 44;
    static constexpr unsigned kShardShift = 52;

    explicit constexpr EntityId(uint64_t raw) noexcept : raw_(raw) {}

    uint64_t raw_;
};

}  // namespace tessera::net
