// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <vector>

#include "tessera/net/entity_id.hpp"
#include "tessera/net/relevancy/relevancy_groups.hpp"
#include "test_support.hpp"

using tessera::net::ConnectionId;
using tessera::net::EntityId;
using tessera::net::GroupId;
using tessera::net::GroupKind;
using tessera::net::RelevancyGroups;

namespace {

EntityId Id(uint32_t index) {
    return EntityId::Make(0, 1, index, 0);
}

std::vector<EntityId> Included(const RelevancyGroups& groups, ConnectionId connection) {
    std::vector<EntityId> out;
    groups.AppendIncluded(connection, out);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

}  // namespace

TESSERA_TEST(exclusion_group_blocks_members_for_every_connection_by_default) {
    RelevancyGroups groups;
    const GroupId g = groups.CreateGroup(GroupKind::Exclusion);
    groups.AddMember(g, Id(1));
    TESSERA_CHECK(groups.IsExcluded(Id(1), 0));
    TESSERA_CHECK(groups.IsExcluded(Id(1), 99));
    TESSERA_CHECK(!groups.IsExcluded(Id(2), 0));
}

TESSERA_TEST(exclusion_group_allows_one_connection_when_its_status_is_set) {
    RelevancyGroups groups;
    const GroupId g = groups.CreateGroup(GroupKind::Exclusion);
    groups.AddMember(g, Id(1));
    TESSERA_CHECK(groups.SetStatus(g, 1, true));
    TESSERA_CHECK(!groups.IsExcluded(Id(1), 1));
    TESSERA_CHECK(groups.IsExcluded(Id(1), 2));
    TESSERA_CHECK(groups.IsAllowed(g, 1));
    TESSERA_CHECK(!groups.IsAllowed(g, 2));
}

TESSERA_TEST(inclusion_group_lists_members_only_for_allowed_connections) {
    RelevancyGroups groups;
    const GroupId g = groups.CreateGroup(GroupKind::Inclusion);
    groups.AddMember(g, Id(1));
    groups.AddMember(g, Id(2));
    TESSERA_CHECK(Included(groups, 3).empty());
    groups.SetStatus(g, 3, true);
    TESSERA_CHECK(Included(groups, 3).size() == 2);
    TESSERA_CHECK(Included(groups, 4).empty());
}

TESSERA_TEST(set_status_for_all_overrides_the_default_and_clears_exceptions) {
    RelevancyGroups groups;
    const GroupId g = groups.CreateGroup(GroupKind::Exclusion);
    groups.AddMember(g, Id(1));
    groups.SetStatus(g, 1, false);
    TESSERA_CHECK(groups.SetStatusForAll(g, true));
    TESSERA_CHECK(!groups.IsExcluded(Id(1), 1));
    TESSERA_CHECK(!groups.IsExcluded(Id(1), 2));
    groups.SetStatus(g, 2, false);
    TESSERA_CHECK(groups.IsExcluded(Id(1), 2));
    TESSERA_CHECK(!groups.IsExcluded(Id(1), 5));
}

TESSERA_TEST(entity_in_two_exclusion_groups_is_blocked_if_either_blocks) {
    RelevancyGroups groups;
    const GroupId a = groups.CreateGroup(GroupKind::Exclusion);
    const GroupId b = groups.CreateGroup(GroupKind::Exclusion);
    groups.AddMember(a, Id(1));
    groups.AddMember(b, Id(1));
    groups.SetStatus(a, 1, true);
    TESSERA_CHECK(groups.IsExcluded(Id(1), 1));
    groups.SetStatus(b, 1, true);
    TESSERA_CHECK(!groups.IsExcluded(Id(1), 1));
}

TESSERA_TEST(destroying_a_group_removes_members_and_status) {
    RelevancyGroups groups;
    const GroupId g = groups.CreateGroup(GroupKind::Exclusion);
    groups.AddMember(g, Id(1));
    TESSERA_CHECK(groups.DestroyGroup(g));
    TESSERA_CHECK(!groups.IsExcluded(Id(1), 0));
    TESSERA_CHECK(groups.GroupCount() == 0);
    TESSERA_CHECK(!groups.DestroyGroup(g));
    TESSERA_CHECK(!groups.AddMember(g, Id(1)));
    TESSERA_CHECK(!groups.SetStatus(g, 0, true));
    TESSERA_CHECK(!groups.SetStatusForAll(g, true));
    TESSERA_CHECK(!groups.IsAllowed(g, 0));
}

TESSERA_TEST(membership_calls_report_duplicates_and_unknowns) {
    RelevancyGroups groups;
    const GroupId g = groups.CreateGroup(GroupKind::Inclusion);
    TESSERA_CHECK(groups.AddMember(g, Id(1)));
    TESSERA_CHECK(!groups.AddMember(g, Id(1)));
    TESSERA_CHECK(groups.IsMember(g, Id(1)));
    TESSERA_CHECK(!groups.RemoveMember(g, Id(2)));
    TESSERA_CHECK(groups.RemoveMember(g, Id(1)));
    TESSERA_CHECK(!groups.IsMember(g, Id(1)));
    TESSERA_CHECK(!groups.AddMember(12345, Id(1)));
    TESSERA_CHECK(!groups.RemoveMember(12345, Id(1)));
    TESSERA_CHECK(!groups.IsMember(12345, Id(1)));
}

TESSERA_TEST(remove_entity_leaves_every_group) {
    RelevancyGroups groups;
    const GroupId a = groups.CreateGroup(GroupKind::Exclusion);
    const GroupId b = groups.CreateGroup(GroupKind::Inclusion);
    groups.AddMember(a, Id(1));
    groups.AddMember(b, Id(1));
    groups.SetStatusForAll(b, true);
    groups.RemoveEntity(Id(1));
    TESSERA_CHECK(!groups.IsMember(a, Id(1)));
    TESSERA_CHECK(!groups.IsMember(b, Id(1)));
    TESSERA_CHECK(!groups.IsExcluded(Id(1), 0));
    TESSERA_CHECK(Included(groups, 0).empty());
    groups.RemoveEntity(Id(77));  // unknown entity: no effect, no crash
}

TESSERA_TEST(remove_connection_clears_only_that_connections_exceptions) {
    RelevancyGroups groups;
    const GroupId g = groups.CreateGroup(GroupKind::Exclusion);
    groups.AddMember(g, Id(1));
    groups.SetStatus(g, 1, true);
    groups.SetStatus(g, 2, true);
    groups.RemoveConnection(1);
    TESSERA_CHECK(groups.IsExcluded(Id(1), 1));
    TESSERA_CHECK(!groups.IsExcluded(Id(1), 2));
}

TESSERA_TEST(group_ids_are_never_reused) {
    RelevancyGroups groups;
    const GroupId first = groups.CreateGroup(GroupKind::Inclusion);
    groups.DestroyGroup(first);
    const GroupId second = groups.CreateGroup(GroupKind::Inclusion);
    TESSERA_CHECK(first != 0 && second != 0 && first != second);
}
