// SPDX-License-Identifier: Apache-2.0
#include "tessera/net/relevancy/relevancy_groups.hpp"

#include <algorithm>

namespace tessera::net {

GroupId RelevancyGroups::CreateGroup(GroupKind kind) {
    const GroupId id = next_id_++;
    groups_[id].kind = kind;
    return id;
}

bool RelevancyGroups::DestroyGroup(GroupId group) {
    const auto it = groups_.find(group);
    if (it == groups_.end()) {
        return false;
    }
    for (const uint64_t raw : it->second.members) {
        const auto entity_it = groups_of_entity_.find(raw);
        std::vector<GroupId>& list = entity_it->second;
        list.erase(std::remove(list.begin(), list.end(), group), list.end());
        if (list.empty()) {
            groups_of_entity_.erase(entity_it);
        }
    }
    groups_.erase(it);
    return true;
}

bool RelevancyGroups::AddMember(GroupId group, EntityId entity) {
    const auto it = groups_.find(group);
    if (it == groups_.end() || !it->second.members.insert(entity.raw()).second) {
        return false;
    }
    groups_of_entity_[entity.raw()].push_back(group);
    return true;
}

bool RelevancyGroups::RemoveMember(GroupId group, EntityId entity) {
    const auto it = groups_.find(group);
    if (it == groups_.end() || it->second.members.erase(entity.raw()) == 0) {
        return false;
    }
    const auto entity_it = groups_of_entity_.find(entity.raw());
    std::vector<GroupId>& list = entity_it->second;
    list.erase(std::remove(list.begin(), list.end(), group), list.end());
    if (list.empty()) {
        groups_of_entity_.erase(entity_it);
    }
    return true;
}

bool RelevancyGroups::IsMember(GroupId group, EntityId entity) const {
    const auto it = groups_.find(group);
    return it != groups_.end() && it->second.members.contains(entity.raw());
}

bool RelevancyGroups::SetStatus(GroupId group, ConnectionId connection, bool allowed) {
    const auto it = groups_.find(group);
    if (it == groups_.end()) {
        return false;
    }
    it->second.exceptions[connection] = allowed;
    return true;
}

bool RelevancyGroups::SetStatusForAll(GroupId group, bool allowed) {
    const auto it = groups_.find(group);
    if (it == groups_.end()) {
        return false;
    }
    it->second.default_allowed = allowed;
    it->second.exceptions.clear();
    return true;
}

bool RelevancyGroups::Allowed(const Group& group, ConnectionId connection) {
    const auto exception = group.exceptions.find(connection);
    return exception != group.exceptions.end() ? exception->second : group.default_allowed;
}

bool RelevancyGroups::IsAllowed(GroupId group, ConnectionId connection) const {
    const auto it = groups_.find(group);
    return it != groups_.end() && Allowed(it->second, connection);
}

bool RelevancyGroups::IsExcluded(EntityId entity, ConnectionId connection) const {
    const auto entity_it = groups_of_entity_.find(entity.raw());
    if (entity_it == groups_of_entity_.end()) {
        return false;
    }
    for (const GroupId id : entity_it->second) {
        const Group& group = groups_.find(id)->second;
        if (group.kind == GroupKind::Exclusion && !Allowed(group, connection)) {
            return true;
        }
    }
    return false;
}

void RelevancyGroups::AppendIncluded(ConnectionId connection, std::vector<EntityId>& out) const {
    for (const auto& [id, group] : groups_) {
        if (group.kind != GroupKind::Inclusion || !Allowed(group, connection)) {
            continue;
        }
        for (const uint64_t raw : group.members) {
            out.push_back(EntityId::FromRaw(raw));
        }
    }
}

void RelevancyGroups::RemoveEntity(EntityId entity) {
    const auto entity_it = groups_of_entity_.find(entity.raw());
    if (entity_it == groups_of_entity_.end()) {
        return;
    }
    for (const GroupId id : entity_it->second) {
        groups_.find(id)->second.members.erase(entity.raw());
    }
    groups_of_entity_.erase(entity_it);
}

void RelevancyGroups::RemoveConnection(ConnectionId connection) {
    for (auto& [id, group] : groups_) {
        group.exceptions.erase(connection);
    }
}

} // namespace tessera::net
