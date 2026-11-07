// SPDX-License-Identifier: Apache-2.0
#include "network_world.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <vector>

using namespace network_test;

TESSERA_TEST(clients_converge_through_a_bad_network) {
    uint64_t checks = 0, sent = 0, lost = 0;
    for (uint32_t seed = 1; seed <= 20; ++seed) {
        NetParams params;
        params.seed = seed;
        params.clients = 3 + seed % 6; // 3 to 8
        NetWorld world(params);
        world.Run();
        TESSERA_CHECK(world.converged());
        TESSERA_CHECK(world.AllConnected());
        TESSERA_CHECK(world.server().Stats().protocol_errors == 0);
        checks += world.checks();
        sent += world.network().Server().Stats().messages_sent;
        lost += world.network().Server().Stats().messages_lost;
    }
    TESSERA_CHECK(checks > 200);     // the checks really ran, on connected clients...
    TESSERA_CHECK(lost > sent / 20); // ...over a network that really lost things
}

TESSERA_TEST(a_busy_world_with_few_values_converges_too) {
    for (uint32_t seed = 100; seed < 115; ++seed) {
        NetParams params;
        params.seed = seed;
        params.clients = 2;
        params.entities = 9;
        params.ticks = 240;
        params.settle_every = 12;
        params.few_values = true;
        params.conditions.loss = 0.6; // heavy loss: every copy of the right state in a stretch can be lost
        NetWorld world(params);
        world.Run();
        TESSERA_CHECK(world.converged() && world.AllConnected());
    }
}

TESSERA_TEST(the_same_seed_gives_the_same_bytes_and_the_same_client_state_with_1_and_8_workers) {
    for (uint32_t seed = 300; seed < 304; ++seed) {
        NetParams one;
        one.seed = seed;
        one.workers = 1;
        NetParams eight = one;
        eight.workers = 8;
        NetWorld a(one);
        NetWorld b(eight);
        a.Run();
        b.Run();
        TESSERA_CHECK(a.converged() && b.converged());
        TESSERA_CHECK(a.Fingerprint() == b.Fingerprint());
    }
    NetParams other;
    other.seed = 999; // and a different seed really does give a different world
    NetWorld c(other);
    c.Run();
    NetParams first;
    first.seed = 300;
    NetWorld d(first);
    d.Run();
    TESSERA_CHECK(c.Fingerprint() != d.Fingerprint());
}

TESSERA_TEST(a_client_that_disconnects_and_a_new_one_that_takes_its_slot_never_see_each_others_state) {
    NetParams params;
    params.clients = 3;
    NetWorld world(params);
    world.Play(30);
    world.Settle(30);
    TESSERA_CHECK(world.Check() && world.AllConnected());
    const uint32_t old_connection = world.client(1).ConnectionId();
    world.DropClient(1);
    world.Settle(5);
    TESSERA_CHECK(world.client(1).State() == ClientState::Disconnected &&
                  world.client(1).VisibleCount() == 0);
    TESSERA_CHECK(world.disconnects() == 1);
    const size_t fresh = world.AddClient();
    world.Play(25); // the newcomer is in the middle of the changes
    world.Settle(30);
    TESSERA_CHECK(world.client(fresh).State() == ClientState::Connected);
    TESSERA_CHECK(world.client(fresh).ConnectionId() == old_connection); // the lowest free slot
    TESSERA_CHECK(world.Check());
    TESSERA_CHECK(world.server().Stats().protocol_errors == 0);
}

TESSERA_TEST(one_hundred_and_twenty_eight_clients_converge) {
    NetParams params;
    params.clients = 128;
    params.entities = 80;
    params.ticks = 40;
    params.settle_every = 0;
    params.area = 200.0f;
    params.conditions.loss = 0.0;
    params.conditions.duplicate = 0.0;
    params.conditions.max_delay_ticks = 1;
    NetWorld world(params);
    world.Run();
    TESSERA_CHECK(world.converged() && world.AllConnected());
    TESSERA_CHECK(world.server().ConnectionCount() == 128 && world.checks() >= 128);
}

TESSERA_TEST(random_and_truncated_input_in_both_directions_never_crashes_or_breaks_convergence) {
    uint64_t malformed = 0, refused = 0;
    for (uint32_t seed = 500; seed < 510; ++seed) {
        NetParams params;
        params.seed = seed;
        params.clients = 4;
        params.noise = 0.7;
        NetWorld world(params);
        world.Run();
        TESSERA_CHECK(world.converged() && world.AllConnected());
        TESSERA_CHECK(world.server().Stats().protocol_errors ==
                      0); // garbage on the unreliable channel is only dropped
        malformed += world.server().Stats().malformed_messages + world.server().Stats().messages_ignored;
        for (size_t i = 0; i < world.client_count(); ++i) {
            TESSERA_CHECK(world.client(i).Stats().protocol_errors == 0);
            malformed += world.client(i).Stats().malformed_messages;
            refused += world.client(i).Stats().buffers_refused;
        }
    }
    TESSERA_CHECK(malformed > 100 && refused > 0); // the garbage really arrived and was refused
}

TESSERA_TEST(a_stopped_world_costs_no_state_bytes) {
    NetParams params;
    NetWorld world(params);
    world.Play(30);
    world.Settle(40); // everything confirmed
    TESSERA_CHECK(world.Check());
    const uint64_t before = world.server().Stats().state_buffers_sent;
    const uint64_t bytes_before = world.network().Server().Stats().bytes_sent;
    world.Settle(20);
    TESSERA_CHECK(world.server().Stats().state_buffers_sent == before);
    TESSERA_CHECK(world.network().Server().Stats().bytes_sent == bytes_before); // not a byte
}

TESSERA_TEST(players_see_entities_enter_and_leave_as_they_walk) {
    NetParams params;
    params.clients = 2;
    params.entities = 80;
    NetWorld world(params);
    world.Play(60);
    world.Settle(30);
    TESSERA_CHECK(world.Check());
    size_t most = 0, least = 100000;
    for (size_t i = 0; i < world.client_count(); ++i) {
        most = std::max(most, world.client(i).VisibleCount());
        least = std::min(least, world.client(i).VisibleCount());
    }
    TESSERA_CHECK(most > 0);                   // there was something to see
    TESSERA_CHECK(most < world.mobs().size()); // and not everything was visible: relevancy really filtered
    (void)least;
}

namespace {

NetParams TightBudget() {
    NetParams params;
    params.rate_kbps = 12; // about 50 bytes a tick for a world that changes much more than that
    params.max_buffer_bytes = 100;
    params.min_resend_ticks = 2;
    params.clients = 4;
    return params;
}

} // namespace

TESSERA_TEST(clients_converge_under_a_tight_budget_and_a_bad_network) {
    for (uint32_t seed = 1; seed <= 12; ++seed) {
        NetParams params = TightBudget();
        params.seed = seed;
        NetWorld world(params);
        world.Run();
        TESSERA_CHECK(world.converged() && world.AllConnected());
        TESSERA_CHECK(world.server().Stats().protocol_errors == 0);
    }
}

TESSERA_TEST(a_tight_budget_degrades_frequency_but_never_leaves_a_client_behind_forever) {
    NetParams params = TightBudget();
    params.conditions.loss = 0.0;
    params.conditions.duplicate = 0.0;
    NetWorld world(params);
    world.Play(30);
    double most = 0.0;
    for (int i = 0; i < 60; ++i) {
        world.Play(1);
        most = std::max(most, world.Divergence());
    }
    TESSERA_CHECK(most > 0.05 && most < 0.95); // behind, because the budget does not hold everything...
    world.Settle(120);
    TESSERA_CHECK(world.Divergence() == 0.0 && world.Check()); // ...but caught up once the world stops
}

TESSERA_TEST(one_hundred_and_twenty_eight_clients_converge_under_a_budget) {
    NetParams params;
    params.clients = 128;
    params.entities = 80;
    params.ticks = 40;
    params.settle_every = 0;
    params.area = 200.0f;
    params.rate_kbps = 40;
    params.max_buffer_bytes = 200;
    params.min_resend_ticks = 2;
    params.conditions.loss = 0.0;
    params.conditions.duplicate = 0.0;
    params.conditions.max_delay_ticks = 1;
    NetWorld world(params);
    world.Play(40);
    world.Settle(120);
    TESSERA_CHECK(world.Check() && world.AllConnected());
    TESSERA_CHECK(world.server().ConnectionCount() == 128 && world.checks() >= 128);
}

TESSERA_TEST(the_same_bytes_with_1_and_8_workers_under_a_budget) {
    for (uint32_t seed = 700; seed < 703; ++seed) {
        NetParams one = TightBudget();
        one.seed = seed;
        one.workers = 1;
        NetParams eight = one;
        eight.workers = 8;
        NetWorld a(one);
        NetWorld b(eight);
        a.Run();
        b.Run();
        TESSERA_CHECK(a.converged() && b.converged());
        TESSERA_CHECK(a.Fingerprint() == b.Fingerprint());
    }
}

TESSERA_TEST(a_client_that_disconnects_and_a_new_one_that_takes_its_slot_under_a_budget_both_converge) {
    NetParams params = TightBudget();
    params.clients = 3;
    NetWorld world(params);
    world.Play(30);
    world.Settle(120);
    TESSERA_CHECK(world.Check() && world.AllConnected());
    world.DropClient(1);
    world.Settle(5);
    const size_t fresh = world.AddClient();
    world.Play(25);
    world.Settle(150);
    TESSERA_CHECK(world.client(fresh).State() == ClientState::Connected);
    TESSERA_CHECK(world.Check());
    TESSERA_CHECK(world.server().Stats().protocol_errors == 0);
}
