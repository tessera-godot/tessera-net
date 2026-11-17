// SPDX-License-Identifier: Apache-2.0
// What the example server does with a player whose connection goes away, on real sockets in loopback. A
// player that leaves with notice is freed at once. One that goes silent is freed when ENet gives up on the
// connection, and a client that arrives in the meantime gets an entity of its own: it never takes the old
// one over and never removes it.
#include "example_game.hpp"
#include "tessera/net/client_replica.hpp"
#include "tessera/net/transport/enet_transport.hpp"
#include "tessera/server/server_host.hpp"
#include "test_support.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace tessera::net;
namespace server = tessera::server;

namespace {

using Clock = std::chrono::steady_clock;
using Millis = std::chrono::milliseconds;
using Entities = std::set<EntityId>;

constexpr uint32_t kPlayerType = server::example::ExampleGame::kPlayerType;
constexpr auto kStep = Millis(5);
constexpr auto kShortLimit = Millis(10000);
// ENet drops a peer after 30 seconds at the most; the rest is room for the clock and the tick.
constexpr auto kSilenceLimit = Millis(35000);

long long MillisSince(Clock::time_point start) {
    return std::chrono::duration_cast<Millis>(Clock::now() - start).count();
}

// The example server, with players and no other entities, on a thread of its own as a real program has it.
class Room {
public:
    Room()
        : game_(0, false, true), transport_(Listen()),
          log_(server::LogLevel::Info,
               [this](const std::string& line) {
                   std::lock_guard<std::mutex> lock(lines_mutex_);
                   lines_.push_back(line);
               }),
          host_(Config(), game_, transport_, log_, [this] { transport_.Flush(); }),
          schema_(game_.DescribeWorld().schema) {
        runner_ = std::thread([this] { host_.Run(); });
    }

    ~Room() {
        Stop();
    }

    // Stops the server and waits for its thread. Only after this may the game be read from the test.
    void Stop() {
        host_.RequestStop();
        if (runner_.joinable()) {
            runner_.join();
        }
    }
    // How many player entities the game holds. Stops the server first, so that nothing else touches the game.
    size_t PlayerEntitiesAfterStop() {
        Stop();
        return game_.PlayerEntityCount();
    }

    uint16_t Port() const noexcept {
        return transport_.Port();
    }
    const ReplicationSchema& Schema() const noexcept {
        return *schema_;
    }

    // How many times the server logged that this player's connection was freed. The line is written before
    // the game is told, so a client that sees the entity go must find it here already.
    int Freed(uint32_t player) {
        const std::string tail = "player disconnected: " + std::to_string(player);
        std::lock_guard<std::mutex> lock(lines_mutex_);
        int count = 0;
        for (const std::string& line : lines_) {
            if (line.size() >= tail.size() &&
                line.compare(line.size() - tail.size(), tail.size(), tail) == 0) {
                ++count;
            }
        }
        return count;
    }

private:
    static EnetServerConfig Listen() {
        EnetServerConfig listen;
        listen.bind_address = "127.0.0.1";
        listen.port = 0; // the system chooses; `Port()` says which
        listen.max_clients = 8;
        return listen;
    }
    static server::ServerConfig Config() {
        server::ServerConfig config;
        config.max_players = 8;
        config.tick_rate_hz = 50;
        return config;
    }

    server::example::ExampleGame game_;
    EnetServerTransport transport_;
    std::mutex lines_mutex_;
    std::vector<std::string> lines_;
    server::Logger log_;
    server::ServerHost host_;
    const ReplicationSchema* schema_;
    std::thread runner_;
};

// A client of the room, and what it has seen of the player entities: which ones are there now, and how many
// times each one appeared and went away.
class Client {
public:
    explicit Client(Room& room)
        : transport_(std::make_unique<EnetClientTransport>(Dial(room.Port()))),
          replica_(std::make_unique<ClientReplica>(*transport_, room.Schema())) {}

    // Reads the server and notes what changed. A client that stopped calling this is what a silent one is.
    void Service() {
        if (!replica_) {
            return;
        }
        replica_->Poll();
        transport_->Flush();
        const Entities now = Players();
        for (const EntityId id : now) {
            if (current_.count(id) == 0) {
                ++appeared_[id];
            }
        }
        for (const EntityId id : current_) {
            if (now.count(id) == 0) {
                ++removed_[id];
                if (on_removed) {
                    on_removed(id);
                }
            }
        }
        current_ = now;
    }

    // Leaves the way a program that ends does: the transport goes away and tells the server.
    void Leave() {
        replica_.reset();
        transport_.reset();
    }

    // The player entities this client holds with a state.
    Entities Players() const {
        Entities players;
        if (replica_) {
            replica_->ForEachEntity([&players](EntityId id, const uint32_t*) {
                if (id.type() == kPlayerType) {
                    players.insert(id);
                }
            });
        }
        return players;
    }
    bool Connected() const {
        return replica_ && replica_->State() == ClientState::Connected;
    }
    uint32_t Id() const {
        return replica_->ConnectionId();
    }
    int Appeared(EntityId id) const {
        const auto found = appeared_.find(id);
        return found == appeared_.end() ? 0 : found->second;
    }
    int Removed(EntityId id) const {
        const auto found = removed_.find(id);
        return found == removed_.end() ? 0 : found->second;
    }

    std::function<void(EntityId)> on_removed;

private:
    static EnetClientConfig Dial(uint16_t port) {
        EnetClientConfig dial;
        dial.server_address = "127.0.0.1";
        dial.server_port = port;
        return dial;
    }

    std::unique_ptr<EnetClientTransport> transport_;
    std::unique_ptr<ClientReplica> replica_;
    Entities current_;
    std::map<EntityId, int> appeared_;
    std::map<EntityId, int> removed_;
};

// Services the clients every few milliseconds until `done` says so or the limit passes.
bool PumpUntil(std::initializer_list<Client*> clients, Millis limit, const std::function<bool()>& done) {
    const auto deadline = Clock::now() + limit;
    while (Clock::now() < deadline) {
        for (Client* client : clients) {
            client->Service();
        }
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(kStep);
    }
    return false;
}

// The first entity of `all` that is in none of `known`.
EntityId NewOne(const Entities& all, std::initializer_list<EntityId> known) {
    for (const EntityId id : all) {
        bool is_known = false;
        for (const EntityId other : known) {
            is_known = is_known || other == id;
        }
        if (!is_known) {
            return id;
        }
    }
    return EntityId::Null();
}

} // namespace

TESSERA_TEST(a_client_that_leaves_cleanly_is_freed_at_once) {
    Room room;
    Client b(room);
    TESSERA_CHECK(PumpUntil({&b}, kShortLimit, [&] { return b.Connected() && b.Players().size() == 1; }));
    const EntityId b_own = *b.Players().begin();
    Client a(room);
    TESSERA_CHECK(
        PumpUntil({&a, &b}, kShortLimit, [&] { return a.Players().size() == 2 && b.Players().size() == 2; }));
    const EntityId a_own = NewOne(b.Players(), {b_own});
    const uint32_t a_id = a.Id();

    bool removed_before_the_log = false;
    b.on_removed = [&](EntityId) {
        removed_before_the_log = removed_before_the_log || room.Freed(a_id) == 0;
    };

    const auto left_at = Clock::now();
    a.Leave();
    long long logged_ms = -1;
    const bool gone = PumpUntil({&b}, kShortLimit, [&] {
        if (logged_ms < 0 && room.Freed(a_id) > 0) {
            logged_ms = MillisSince(left_at);
        }
        return b.Players().size() == 1;
    });
    const long long seen_ms = MillisSince(left_at);
    std::fprintf(stderr, "clean leave: logged after %lld ms, gone for the other player after %lld ms\n",
                 logged_ms, seen_ms);

    TESSERA_CHECK(gone);
    TESSERA_CHECK(logged_ms >= 0 && logged_ms <= 2000);
    TESSERA_CHECK(seen_ms <= 2000);
    TESSERA_CHECK(!removed_before_the_log);
    TESSERA_CHECK(room.Freed(a_id) == 1);
    TESSERA_CHECK(b.Players() == Entities{b_own});
    TESSERA_CHECK(b.Removed(a_own) == 1);
    TESSERA_CHECK(b.Removed(b_own) == 0);
    TESSERA_CHECK(room.PlayerEntitiesAfterStop() == 1); // the game let go of the entity of A
}

TESSERA_TEST(a_silent_client_is_freed_by_the_enet_timeout_and_a_new_client_meanwhile_has_its_own_entity) {
    Room room;
    Client b(room);
    TESSERA_CHECK(PumpUntil({&b}, kShortLimit, [&] { return b.Connected() && b.Players().size() == 1; }));
    const EntityId b_own = *b.Players().begin();
    Client a(room);
    TESSERA_CHECK(
        PumpUntil({&a, &b}, kShortLimit, [&] { return a.Players().size() == 2 && b.Players().size() == 2; }));
    const EntityId a_own = NewOne(b.Players(), {b_own});
    const uint32_t a_id = a.Id();
    const uint32_t b_id = b.Id();

    bool removed_before_the_log = false;
    b.on_removed = [&](EntityId) {
        removed_before_the_log = removed_before_the_log || room.Freed(a_id) == 0;
    };

    // A stops being served from here on, and its transport stays: a program that hangs or loses its network
    // does not say goodbye.
    const auto silent_since = Clock::now();
    TESSERA_CHECK(PumpUntil({&b}, Millis(1000), [] { return false; }) == false);
    TESSERA_CHECK(room.Freed(a_id) == 0); // the connection is still there after a second

    // A new client arrives while the old connection is still held by the server.
    Client c(room);
    c.on_removed = [&](EntityId) {
        removed_before_the_log = removed_before_the_log || room.Freed(a_id) == 0;
    };
    TESSERA_CHECK(PumpUntil({&b, &c}, kShortLimit, [&] {
        return c.Connected() && c.Players().size() == 3 && b.Players().size() == 3;
    }));
    TESSERA_CHECK(room.Freed(a_id) == 0);
    TESSERA_CHECK(c.Id() != a_id); // the slot of A is taken, so C cannot be given its number
    TESSERA_CHECK(c.Id() != b_id);
    const EntityId c_own = NewOne(b.Players(), {a_own, b_own});
    TESSERA_CHECK(!c_own.IsNull());
    TESSERA_CHECK(c_own != a_own);
    TESSERA_CHECK(c.Players() == (Entities{a_own, b_own, c_own}));
    TESSERA_CHECK(b.Players() == (Entities{a_own, b_own, c_own}));

    // Wait for the server to give up on A.
    long long freed_ms = -1;
    const bool freed = PumpUntil({&b, &c}, kSilenceLimit, [&] {
        if (room.Freed(a_id) > 0) {
            freed_ms = MillisSince(silent_since);
            return true;
        }
        return false;
    });
    TESSERA_CHECK(freed);
    std::fprintf(stderr, "silent client freed after %lld ms\n", freed_ms);
    TESSERA_CHECK(freed_ms >= 4500);  // ENet keeps a peer for 5,000 ms at least
    TESSERA_CHECK(freed_ms <= 31000); // and drops it after 30,000 ms at the most

    TESSERA_CHECK(
        PumpUntil({&b, &c}, kShortLimit, [&] { return b.Players().size() == 2 && c.Players().size() == 2; }));
    TESSERA_CHECK(room.Freed(a_id) == 1);
    TESSERA_CHECK(!removed_before_the_log);
    TESSERA_CHECK(b.Players() == (Entities{b_own, c_own}));
    TESSERA_CHECK(c.Players() == (Entities{b_own, c_own}));
    // The old entity went away once for each of them, and nobody else's did.
    TESSERA_CHECK(b.Removed(a_own) == 1);
    TESSERA_CHECK(c.Removed(a_own) == 1);
    TESSERA_CHECK(b.Removed(b_own) == 0 && b.Removed(c_own) == 0);
    TESSERA_CHECK(c.Removed(b_own) == 0 && c.Removed(c_own) == 0);
    TESSERA_CHECK(b.Appeared(c_own) == 1 && c.Appeared(c_own) == 1);
    TESSERA_CHECK(room.PlayerEntitiesAfterStop() == 2); // the game let go of the entity of A, and only that
}

TESSERA_TEST(a_new_client_after_a_clean_leave_gets_a_fresh_entity) {
    Room room;
    Client b(room);
    TESSERA_CHECK(PumpUntil({&b}, kShortLimit, [&] { return b.Connected() && b.Players().size() == 1; }));
    const EntityId b_own = *b.Players().begin();
    Client a(room);
    TESSERA_CHECK(
        PumpUntil({&a, &b}, kShortLimit, [&] { return a.Players().size() == 2 && b.Players().size() == 2; }));
    const EntityId a_own = NewOne(b.Players(), {b_own});
    const uint32_t a_id = a.Id();

    a.Leave();
    TESSERA_CHECK(
        PumpUntil({&b}, kShortLimit, [&] { return room.Freed(a_id) > 0 && b.Players().size() == 1; }));

    Client d(room);
    TESSERA_CHECK(PumpUntil({&b, &d}, kShortLimit, [&] {
        return d.Connected() && d.Players().size() == 2 && b.Players().size() == 2;
    }));
    // The number may be the one A had (the server hands out the lowest free one); the entity is not.
    TESSERA_CHECK(d.Id() < 8);
    const EntityId d_own = NewOne(b.Players(), {b_own});
    TESSERA_CHECK(!d_own.IsNull());
    TESSERA_CHECK(d_own != a_own); // the slot of A was reused, the generation in the id tells them apart
    TESSERA_CHECK(d.Players() == (Entities{b_own, d_own}));
    TESSERA_CHECK(b.Removed(a_own) == 1 && b.Appeared(a_own) == 1);
    TESSERA_CHECK(b.Appeared(d_own) == 1 && b.Removed(d_own) == 0);
    TESSERA_CHECK(room.PlayerEntitiesAfterStop() == 2);
}
