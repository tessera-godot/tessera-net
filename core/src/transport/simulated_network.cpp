// SPDX-License-Identifier: Apache-2.0
#include "tessera/net/transport/simulated_network.hpp"

#include <algorithm>
#include <cmath>

namespace tessera::net {

// One end of the network: the server's, or one client's.
class SimulatedNetwork::Endpoint final : public ITransport {
public:
    Endpoint(SimulatedNetwork& network, bool is_server, uint32_t link)
        : network_(network), is_server_(is_server), link_(link) {}

    void Poll(std::vector<TransportEvent>& out) override {
        for (TransportEvent& event : inbox_) {
            out.push_back(std::move(event));
        }
        inbox_.clear();
    }

    bool Send(TransportConnection connection, Channel channel, std::span<const uint8_t> data) override {
        uint32_t link = link_;
        if (is_server_) {
            if (connection == 0) {
                return false;
            }
            link = connection - 1;
        } else if (connection != 0) {
            return false;
        }
        return network_.SendFrom(is_server_, link, channel, data, stats_);
    }

    void Disconnect(TransportConnection connection) override {
        uint32_t link = link_;
        if (is_server_) {
            if (connection == 0) {
                return;
            }
            link = connection - 1;
        } else if (connection != 0) {
            return;
        }
        network_.DisconnectFrom(is_server_, link);
    }

    TransportStats Stats() const noexcept override {
        return stats_;
    }

    void Deliver(TransportEvent event) {
        if (event.kind == TransportEvent::Kind::Message) {
            ++stats_.messages_received;
            stats_.bytes_received += event.data.size();
        }
        inbox_.push_back(std::move(event));
    }

private:
    SimulatedNetwork& network_;
    bool is_server_;
    uint32_t link_;
    std::vector<TransportEvent> inbox_;
    TransportStats stats_;
};

SimulatedNetwork::SimulatedNetwork(const NetworkConditions& conditions)
    : conditions_(conditions), random_(conditions.seed), server_(std::make_unique<Endpoint>(*this, true, 0)) {
}

SimulatedNetwork::~SimulatedNetwork() = default;

ITransport& SimulatedNetwork::Server() noexcept {
    return *server_;
}

ITransport* SimulatedNetwork::AddClient() {
    const uint32_t link = static_cast<uint32_t>(links_.size());
    links_.emplace_back();
    clients_.push_back(std::make_unique<Endpoint>(*this, false, link));
    Queue(now_, link, true, TransportEvent::Kind::Connected, Channel::ReliableOrdered, {});
    Queue(now_, link, false, TransportEvent::Kind::Connected, Channel::ReliableOrdered, {});
    return clients_.back().get();
}

void SimulatedNetwork::SetConditions(const NetworkConditions& conditions) {
    const uint32_t seed = conditions_.seed;
    conditions_ = conditions;
    conditions_.seed = seed;
}

double SimulatedNetwork::Uniform() {
    return static_cast<double>(random_() >> 11) * (1.0 / 9007199254740992.0); // 53 bits, in [0, 1)
}

uint32_t SimulatedNetwork::RollDelay() {
    const uint32_t low = conditions_.min_delay_ticks;
    const uint32_t high = std::max(conditions_.max_delay_ticks, low);
    return low + static_cast<uint32_t>(random_() % (static_cast<uint64_t>(high - low) + 1));
}

void SimulatedNetwork::Queue(uint64_t deliver_at, uint32_t link, bool to_server, TransportEvent::Kind kind,
                             Channel channel, std::vector<uint8_t> data) {
    flight_.push_back(Item{deliver_at, next_sequence_++, link, to_server, kind, channel, std::move(data)});
}

// Reliable items toward one side never overtake each other: each one is delivered no earlier than the last.
uint64_t SimulatedNetwork::ReliableDelivery(Link& link, bool to_server) {
    uint64_t& last = link.last_reliable[to_server ? 0 : 1];
    last = std::max(last, now_ + RollDelay());
    return last;
}

bool SimulatedNetwork::SendFrom(bool from_server, uint32_t link_index, Channel channel,
                                std::span<const uint8_t> data, TransportStats& stats) {
    if (link_index >= links_.size()) {
        return false;
    }
    Link& link = links_[link_index];
    const bool closed = from_server ? (link.server_closed || link.server_notified)
                                    : (link.client_closed || link.client_notified);
    if (closed) {
        return false;
    }
    ++stats.messages_sent;
    stats.bytes_sent += data.size();
    const bool to_server = !from_server;
    std::vector<uint8_t> bytes(data.begin(), data.end());
    if (channel == Channel::ReliableOrdered) {
        Queue(ReliableDelivery(link, to_server), link_index, to_server, TransportEvent::Kind::Message,
              channel, std::move(bytes));
        return true;
    }
    const double fragments =
        std::max<double>(1.0, std::ceil(static_cast<double>(data.size()) /
                                        static_cast<double>(std::max<uint32_t>(1, conditions_.mtu))));
    const double lost = 1.0 - std::pow(1.0 - conditions_.loss, fragments);
    if (Uniform() < lost) {
        ++stats.messages_lost;
        return true;
    }
    const uint64_t first = now_ + RollDelay();
    const bool twice = Uniform() < conditions_.duplicate;
    if (twice) {
        Queue(now_ + RollDelay(), link_index, to_server, TransportEvent::Kind::Message, channel, bytes);
    }
    Queue(first, link_index, to_server, TransportEvent::Kind::Message, channel, std::move(bytes));
    return true;
}

void SimulatedNetwork::DisconnectFrom(bool from_server, uint32_t link_index) {
    if (link_index >= links_.size()) {
        return;
    }
    Link& link = links_[link_index];
    bool& closed = from_server ? link.server_closed : link.client_closed;
    if (closed) {
        return;
    }
    closed = true;
    // The other side is told after everything this side sent before.
    const bool to_server = !from_server;
    Queue(ReliableDelivery(link, to_server), link_index, to_server, TransportEvent::Kind::Disconnected,
          Channel::ReliableOrdered, {});
}

void SimulatedNetwork::Advance(uint64_t tick) {
    now_ = tick;
    std::vector<Item> due;
    std::vector<Item> later;
    for (Item& item : flight_) {
        (item.deliver_at <= tick ? due : later).push_back(std::move(item));
    }
    flight_ = std::move(later);
    std::sort(due.begin(), due.end(), [](const Item& a, const Item& b) {
        return a.deliver_at != b.deliver_at ? a.deliver_at < b.deliver_at : a.sequence < b.sequence;
    });
    for (Item& item : due) {
        Link& link = links_[item.link];
        Endpoint& target = item.to_server ? *server_ : *clients_[item.link];
        const TransportConnection connection = item.to_server ? item.link + 1 : 0;
        switch (item.kind) {
        case TransportEvent::Kind::Message: {
            const bool target_closed = item.to_server ? link.server_closed : link.client_closed;
            if (!target_closed) { // whoever left receives nothing more
                target.Deliver(TransportEvent{item.kind, connection, item.channel, std::move(item.data)});
            }
            break;
        }
        case TransportEvent::Kind::Connected:
            target.Deliver(TransportEvent{item.kind, connection, item.channel, {}});
            break;
        case TransportEvent::Kind::Disconnected:
            (item.to_server ? link.server_notified : link.client_notified) = true;
            target.Deliver(TransportEvent{item.kind, connection, item.channel, {}});
            break;
        }
    }
}

} // namespace tessera::net
