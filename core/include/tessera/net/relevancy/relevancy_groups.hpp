// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "tessera/net/entity_id.hpp"
#include "tessera/net/relevancy/relevancy_types.hpp"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tessera::net {

/// Identifies a group. 0 is never a valid group.
using GroupId = uint32_t;

enum class GroupKind : uint8_t {
    /// Members are visible to the connections the group allows, even when their type would hide them.
    Inclusion,
    /// Members are hidden from every connection except the ones the group allows. Exclusion wins
    /// over everything else.
    Exclusion,
};

/// Lets the game override the type-based rules for sets of entities, for example "hide this squad's
/// loot from other teams" (exclusion) or "show this objective to the whole team" (inclusion).
///
/// Whether a connection is allowed by a group is a status: each group has a default (`false` at
/// first) and optional exceptions per connection.
///
/// Thread safety: the `const` calls may run in parallel with each other. Anything that changes a
/// group must run alone, in the structure phase of a tick.
class RelevancyGroups {
public:
    /// Creates an empty group. Ids are never reused.
    GroupId CreateGroup(GroupKind kind);

    /// Deletes a group together with its members and statuses. False if it does not exist.
    bool DestroyGroup(GroupId group);

    /// False if the group does not exist or the entity is already a member.
    bool AddMember(GroupId group, EntityId entity);

    /// False if the group does not exist or the entity is not a member.
    bool RemoveMember(GroupId group, EntityId entity);

    bool IsMember(GroupId group, EntityId entity) const;

    /// Sets what the group allows for one connection. False if the group does not exist.
    bool SetStatus(GroupId group, ConnectionId connection, bool allowed);

    /// Sets the default status and erases every per-connection exception. False if the group does
    /// not exist.
    bool SetStatusForAll(GroupId group, bool allowed);

    /// The status of a connection in a group: its exception if it has one, else the default. False
    /// if the group does not exist.
    bool IsAllowed(GroupId group, ConnectionId connection) const;

    /// True if the entity belongs to an exclusion group that does not allow this connection.
    bool IsExcluded(EntityId entity, ConnectionId connection) const;

    /// Appends the members of every inclusion group that allows this connection. The same entity
    /// can appear more than once, and the order is not specified.
    void AppendIncluded(ConnectionId connection, std::vector<EntityId>& out) const;

    /// Removes an entity from every group (call when the entity is destroyed).
    void RemoveEntity(EntityId entity);

    /// Erases the exceptions of a connection in every group (call when the connection goes away, so
    /// a later connection with the same id starts clean).
    void RemoveConnection(ConnectionId connection);

    size_t GroupCount() const noexcept {
        return groups_.size();
    }

private:
    struct Group {
        GroupKind kind;
        bool default_allowed = false;
        std::unordered_map<ConnectionId, bool> exceptions;
        std::unordered_set<uint64_t> members; // EntityId::raw()
    };

    static bool Allowed(const Group& group, ConnectionId connection);

    GroupId next_id_ = 1;
    std::unordered_map<GroupId, Group> groups_;
    // The reverse index: which groups an entity belongs to. Makes IsExcluded one lookup per entity.
    std::unordered_map<uint64_t, std::vector<GroupId>> groups_of_entity_;
};

} // namespace tessera::net
