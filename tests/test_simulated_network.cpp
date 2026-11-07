// SPDX-License-Identifier: Apache-2.0
#include "tessera/net/transport/simulated_network.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <cstdint>
#include <set>
#include <vector>

using namespace tessera::net;

namespace {

std::vector<TransportEvent> Drain(ITransport& transport) {
    std::vector<TransportEvent> events;
    transport.Poll(events);
    return events;
}

// A message that carries a number in its first four bytes, padded to `size` bytes.
std::vector<uint8_t> Message(uint32_t number, size_t size = 4) {
    std::vector<uint8_t> bytes(std::max<size_t>(size, 4), 0);
    for (int i = 0; i < 4; ++i) {
        bytes[static_cast<size_t>(i)] = static_cast<uint8_t>(number >> (8 * i));
    }
    return bytes;
}

uint32_t NumberOf(const std::vector<uint8_t>& bytes) {
    uint32_t number = 0;
    for (int i = 0; i < 4; ++i) {
        number |= static_cast<uint32_t>(bytes[static_cast<size_t>(i)]) << (8 * i);
    }
    return number;
}

std::vector<TransportEvent> OnlyMessages(const std::vector<TransportEvent>& events) {
    std::vector<TransportEvent> messages;
    for (const TransportEvent& event : events) {
        if (event.kind == TransportEvent::Kind::Message) {
            messages.push_back(event);
        }
    }
    return messages;
}

// Connects one client and returns the id the server gave it.
TransportConnection Connect(SimulatedNetwork& network, ITransport*& client) {
    client = network.AddClient();
    network.Advance(0);
    TransportConnection id = 0;
    for (const TransportEvent& event : Drain(network.Server())) {
        if (event.kind == TransportEvent::Kind::Connected) {
            id = event.connection;
        }
    }
    Drain(*client);
    return id;
}

NetworkConditions Conditions(double loss = 0.0, double duplicate = 0.0, uint32_t min_delay = 0,
                             uint32_t max_delay = 0) {
    NetworkConditions conditions;
    conditions.loss = loss;
    conditions.duplicate = duplicate;
    conditions.min_delay_ticks = min_delay;
    conditions.max_delay_ticks = max_delay;
    return conditions;
}

} // namespace

TESSERA_TEST(client_connects_and_both_sides_see_it) {
    SimulatedNetwork network(Conditions());
    ITransport* client = network.AddClient();
    TESSERA_CHECK(Drain(network.Server()).empty()); // nothing until time advances
    network.Advance(0);
    const std::vector<TransportEvent> server_events = Drain(network.Server());
    TESSERA_CHECK(server_events.size() == 1 && server_events[0].kind == TransportEvent::Kind::Connected);
    const std::vector<TransportEvent> client_events = Drain(*client);
    TESSERA_CHECK(client_events.size() == 1 && client_events[0].kind == TransportEvent::Kind::Connected);
    TESSERA_CHECK(client_events[0].connection == 0); // a client sees the server as connection 0
    TESSERA_CHECK(Drain(network.Server()).empty() && Drain(*client).empty()); // Poll takes what it returns
}

TESSERA_TEST(messages_arrive_in_both_directions) {
    SimulatedNetwork network(Conditions());
    ITransport* client = nullptr;
    const TransportConnection id = Connect(network, client);
    TESSERA_CHECK(client->Send(0, Channel::ReliableOrdered, Message(11)));
    TESSERA_CHECK(network.Server().Send(id, Channel::Unreliable, Message(22)));
    network.Advance(1);
    const std::vector<TransportEvent> at_server = OnlyMessages(Drain(network.Server()));
    const std::vector<TransportEvent> at_client = OnlyMessages(Drain(*client));
    TESSERA_CHECK(at_server.size() == 1 && at_server[0].connection == id &&
                  NumberOf(at_server[0].data) == 11);
    TESSERA_CHECK(at_server[0].channel == Channel::ReliableOrdered);
    TESSERA_CHECK(at_client.size() == 1 && at_client[0].connection == 0 && NumberOf(at_client[0].data) == 22);
    TESSERA_CHECK(at_client[0].channel == Channel::Unreliable);
}

TESSERA_TEST(reliable_messages_arrive_in_order_even_with_random_delays) {
    SimulatedNetwork network(Conditions(0.0, 0.0, 0, 5));
    ITransport* client = nullptr;
    const TransportConnection id = Connect(network, client);
    uint32_t sent = 0;
    for (uint64_t tick = 1; tick <= 20; ++tick) {
        network.Advance(tick);
        for (int i = 0; i < 10; ++i) {
            network.Server().Send(id, Channel::ReliableOrdered, Message(sent++));
        }
    }
    std::vector<uint32_t> numbers;
    for (uint64_t tick = 21; tick <= 40; ++tick) {
        network.Advance(tick);
        for (const TransportEvent& event : OnlyMessages(Drain(*client))) {
            numbers.push_back(NumberOf(event.data));
        }
    }
    TESSERA_CHECK(numbers.size() == 200);
    TESSERA_CHECK(std::is_sorted(numbers.begin(), numbers.end()));
}

TESSERA_TEST(reliable_messages_are_never_lost) {
    SimulatedNetwork network(Conditions(0.9, 0.0, 0, 3));
    ITransport* client = nullptr;
    const TransportConnection id = Connect(network, client);
    for (uint32_t i = 0; i < 200; ++i) {
        network.Server().Send(id, Channel::ReliableOrdered, Message(i));
    }
    size_t received = 0;
    for (uint64_t tick = 1; tick <= 10; ++tick) {
        network.Advance(tick);
        received += OnlyMessages(Drain(*client)).size();
    }
    TESSERA_CHECK(received == 200);
}

TESSERA_TEST(unreliable_messages_can_be_lost_and_reordered) {
    SimulatedNetwork network(Conditions(0.3, 0.0, 0, 5));
    ITransport* client = nullptr;
    const TransportConnection id = Connect(network, client);
    for (uint32_t i = 0; i < 500; ++i) {
        network.Server().Send(id, Channel::Unreliable, Message(i));
    }
    std::vector<uint32_t> numbers;
    for (uint64_t tick = 1; tick <= 10; ++tick) {
        network.Advance(tick);
        for (const TransportEvent& event : OnlyMessages(Drain(*client))) {
            numbers.push_back(NumberOf(event.data));
        }
    }
    TESSERA_CHECK(numbers.size() < 500 && numbers.size() > 250);
    TESSERA_CHECK(!std::is_sorted(numbers.begin(), numbers.end())); // at least one arrived out of order
}

TESSERA_TEST(loss_follows_the_fragment_count) {
    NetworkConditions conditions = Conditions(0.05);
    conditions.mtu = 1200;
    SimulatedNetwork network(conditions);
    ITransport* client = nullptr;
    const TransportConnection id = Connect(network, client);
    for (uint32_t i = 0; i < 1000; ++i) {
        network.Server().Send(id, Channel::Unreliable, Message(i, 100));   // 1 fragment
        network.Server().Send(id, Channel::Unreliable, Message(i, 12000)); // 10 fragments
    }
    network.Advance(1);
    size_t small = 0, large = 0;
    for (const TransportEvent& event : OnlyMessages(Drain(*client))) {
        (event.data.size() == 100 ? small : large) += 1;
    }
    const double small_loss = 1.0 - static_cast<double>(small) / 1000.0;
    const double large_loss = 1.0 - static_cast<double>(large) / 1000.0;
    TESSERA_CHECK(small_loss > 0.025 && small_loss < 0.08); // about 5%
    TESSERA_CHECK(large_loss > 0.33 && large_loss < 0.47);  // about 1 - 0.95^10 = 40%
}

TESSERA_TEST(duplicates_are_delivered_twice) {
    SimulatedNetwork network(Conditions(0.0, 1.0));
    ITransport* client = nullptr;
    const TransportConnection id = Connect(network, client);
    network.Server().Send(id, Channel::Unreliable, Message(7));
    network.Advance(1);
    const std::vector<TransportEvent> messages = OnlyMessages(Drain(*client));
    TESSERA_CHECK(messages.size() == 2 && NumberOf(messages[0].data) == 7 && NumberOf(messages[1].data) == 7);
}

TESSERA_TEST(delay_is_in_ticks) {
    SimulatedNetwork network(Conditions(0.0, 0.0, 3, 3));
    ITransport* client = nullptr;
    const TransportConnection id = Connect(network, client);
    network.Advance(10);
    network.Server().Send(id, Channel::Unreliable, Message(1));
    network.Server().Send(id, Channel::ReliableOrdered, Message(2));
    network.Advance(12);
    TESSERA_CHECK(OnlyMessages(Drain(*client)).empty());
    network.Advance(13);
    TESSERA_CHECK(OnlyMessages(Drain(*client)).size() == 2);
}

TESSERA_TEST(same_seed_gives_the_same_events) {
    auto run = [](uint32_t seed) {
        NetworkConditions conditions = Conditions(0.2, 0.1, 0, 4);
        conditions.seed = seed;
        SimulatedNetwork network(conditions);
        ITransport* client = nullptr;
        const TransportConnection id = Connect(network, client);
        std::vector<std::vector<uint8_t>> trace;
        for (uint64_t tick = 1; tick <= 30; ++tick) {
            network.Advance(tick);
            for (uint32_t i = 0; i < 5; ++i) {
                network.Server().Send(id, i % 2 ? Channel::Unreliable : Channel::ReliableOrdered,
                                      Message(static_cast<uint32_t>(tick) * 10 + i));
            }
            for (const TransportEvent& event : Drain(*client)) {
                trace.push_back(event.data);
            }
        }
        return trace;
    };
    TESSERA_CHECK(run(5) == run(5));
    TESSERA_CHECK(run(5) != run(6));
}

TESSERA_TEST(disconnect_is_seen_by_the_other_side_after_the_messages_before_it) {
    SimulatedNetwork network(Conditions(0.0, 0.0, 0, 4));
    ITransport* client = nullptr;
    const TransportConnection id = Connect(network, client);
    client->Send(0, Channel::ReliableOrdered, Message(1));
    client->Send(0, Channel::ReliableOrdered, Message(2));
    client->Disconnect(0);
    std::vector<TransportEvent> events;
    for (uint64_t tick = 1; tick <= 12; ++tick) {
        network.Advance(tick);
        network.Server().Poll(events);
    }
    TESSERA_CHECK(events.size() == 3);
    TESSERA_CHECK(events[0].kind == TransportEvent::Kind::Message && NumberOf(events[0].data) == 1);
    TESSERA_CHECK(events[1].kind == TransportEvent::Kind::Message && NumberOf(events[1].data) == 2);
    TESSERA_CHECK(events[2].kind == TransportEvent::Kind::Disconnected && events[2].connection == id);
}

TESSERA_TEST(send_after_disconnect_fails_and_nothing_is_delivered_to_the_one_that_left) {
    SimulatedNetwork network(Conditions(0.0, 0.0, 1, 1));
    ITransport* client = nullptr;
    const TransportConnection id = Connect(network, client);
    network.Server().Send(id, Channel::ReliableOrdered, Message(5)); // on its way when the client leaves
    client->Disconnect(0);
    TESSERA_CHECK(!client->Send(0, Channel::ReliableOrdered, Message(6)));
    network.Advance(5);
    TESSERA_CHECK(OnlyMessages(Drain(*client)).empty()); // the one that left receives nothing more
    const std::vector<TransportEvent> events = Drain(network.Server());
    TESSERA_CHECK(!events.empty() && events.back().kind == TransportEvent::Kind::Disconnected);
    TESSERA_CHECK(!network.Server().Send(id, Channel::ReliableOrdered, Message(7))); // it knows now
}

TESSERA_TEST(connection_ids_are_never_reused_and_old_messages_never_reach_a_new_client) {
    SimulatedNetwork network(Conditions(0.0, 0.0, 2, 3));
    ITransport* a = nullptr;
    const TransportConnection id_a = Connect(network, a);
    for (uint32_t i = 0; i < 5; ++i) {
        a->Send(0, Channel::ReliableOrdered, Message(i));
    }
    a->Disconnect(0);
    ITransport* b = network.AddClient();
    std::set<TransportConnection> message_sources;
    TransportConnection id_b = 0;
    for (uint64_t tick = 1; tick <= 12; ++tick) {
        network.Advance(tick);
        for (const TransportEvent& event : Drain(network.Server())) {
            if (event.kind == TransportEvent::Kind::Connected) {
                id_b = event.connection;
            } else if (event.kind == TransportEvent::Kind::Message) {
                message_sources.insert(event.connection);
            }
        }
        Drain(*b);
    }
    TESSERA_CHECK(id_b != 0 && id_b != id_a);
    TESSERA_CHECK(message_sources.size() == 1 &&
                  *message_sources.begin() == id_a); // nothing of A looks like B
    TESSERA_CHECK(!network.Server().Send(id_a, Channel::ReliableOrdered, Message(9)));
    TESSERA_CHECK(network.Server().Send(id_b, Channel::ReliableOrdered, Message(9)));
}

TESSERA_TEST(stats_count_bytes_messages_and_losses) {
    SimulatedNetwork network(Conditions(1.0)); // every unreliable message is lost
    ITransport* client = nullptr;
    const TransportConnection id = Connect(network, client);
    network.Server().Send(id, Channel::Unreliable, Message(1, 100));
    network.Server().Send(id, Channel::Unreliable, Message(2, 50));
    network.Server().Send(id, Channel::ReliableOrdered, Message(3, 30));
    network.Advance(1);
    Drain(*client);
    const TransportStats sent = network.Server().Stats();
    TESSERA_CHECK(sent.messages_sent == 3 && sent.bytes_sent == 180 && sent.messages_lost == 2);
    const TransportStats received = client->Stats();
    TESSERA_CHECK(received.messages_received == 1 && received.bytes_received == 30);
}

TESSERA_TEST(many_clients_are_independent) {
    SimulatedNetwork network(Conditions(0.0, 0.0, 0, 2));
    std::vector<ITransport*> clients;
    for (int i = 0; i < 128; ++i) {
        clients.push_back(network.AddClient());
    }
    network.Advance(0);
    std::set<TransportConnection> ids;
    for (const TransportEvent& event : Drain(network.Server())) {
        TESSERA_CHECK(event.kind == TransportEvent::Kind::Connected);
        ids.insert(event.connection);
    }
    TESSERA_CHECK(ids.size() == 128);
    for (uint32_t i = 0; i < 128; ++i) {
        Drain(*clients[i]);
        clients[i]->Send(0, Channel::ReliableOrdered, Message(i));
    }
    std::set<uint32_t> numbers;
    for (uint64_t tick = 1; tick <= 4; ++tick) {
        network.Advance(tick);
        for (const TransportEvent& event : Drain(network.Server())) {
            TESSERA_CHECK(ids.count(event.connection) == 1);
            numbers.insert(NumberOf(event.data));
        }
    }
    TESSERA_CHECK(numbers.size() == 128);
}

TESSERA_TEST(conditions_can_change_while_running) {
    SimulatedNetwork network(Conditions(1.0));
    ITransport* client = nullptr;
    const TransportConnection id = Connect(network, client);
    network.Server().Send(id, Channel::Unreliable, Message(1));
    network.SetConditions(Conditions(0.0));
    network.Server().Send(id, Channel::Unreliable, Message(2));
    network.Advance(1);
    const std::vector<TransportEvent> messages = OnlyMessages(Drain(*client));
    TESSERA_CHECK(messages.size() == 1 && NumberOf(messages[0].data) == 2);
    TESSERA_CHECK(network.InFlight() == 0);
}
