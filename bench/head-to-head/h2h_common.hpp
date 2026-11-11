// SPDX-License-Identifier: Apache-2.0
// What the head-to-head server and bot share: the arguments, the scenario constants, the schema, the
// order-independent checksum of replicated state and a transport wrapper that times its calls.
#pragma once

#include "tessera/net/entity_table.hpp"
#include "tessera/net/replication/property_schema.hpp"
#include "tessera/net/transport/transport.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace h2h {

using tessera::net::ITransport;
using tessera::net::kPageSlots;
using tessera::net::PropertyDescriptor;
using tessera::net::ReplicationSchema;

// The scenario of the baseline in bench/godot-replication: a square world of 2 km by 2 km, 20 updates per
// second, entities that walk at up to 5 m/s along each ground axis.
constexpr float kWorldSide = 2000.0f;
constexpr uint32_t kTickHz = 20;
constexpr float kMaxSpeed = 5.0f;

struct Args {
    uint16_t port = 9100;
    std::string host = "127.0.0.1";
    uint32_t entities = 500;
    uint32_t clients = 8;
    uint32_t run = 10;             // seconds after every client has connected
    uint32_t moving = 100;         // percent of the entities that move
    uint32_t threads = 1;          // workers of the server, the calling thread included
    std::string relevancy = "all"; // "all" or "spatial"
    float range = 250.0f;          // spatial only
    int64_t epoch_ms = 0;          // server: 0 means now; bot: when tick 0 happened, for the lag
};

/// Reads `key=value` arguments, the form the Godot baseline uses. Throws std::invalid_argument for a key
/// that is not known or a value that is not a number where one is needed.
inline Args ParseArgs(int argc, char** argv) {
    Args args;
    for (int i = 1; i < argc; ++i) {
        const std::string text = argv[i];
        const size_t eq = text.find('=');
        if (eq == std::string::npos) {
            throw std::invalid_argument("expected key=value, got: " + text);
        }
        const std::string key = text.substr(0, eq);
        const std::string value = text.substr(eq + 1);
        if (key == "port") {
            args.port = static_cast<uint16_t>(std::stoul(value));
        } else if (key == "host") {
            args.host = value;
        } else if (key == "entities") {
            args.entities = static_cast<uint32_t>(std::stoul(value));
        } else if (key == "clients") {
            args.clients = static_cast<uint32_t>(std::stoul(value));
        } else if (key == "run") {
            args.run = static_cast<uint32_t>(std::stoul(value));
        } else if (key == "moving") {
            args.moving = static_cast<uint32_t>(std::stoul(value));
        } else if (key == "threads") {
            args.threads = static_cast<uint32_t>(std::stoul(value));
        } else if (key == "relevancy") {
            if (value != "all" && value != "spatial") {
                throw std::invalid_argument("relevancy must be all or spatial");
            }
            args.relevancy = value;
        } else if (key == "range") {
            args.range = std::stof(value);
        } else if (key == "epoch_ms") {
            args.epoch_ms = std::stoll(value);
        } else {
            throw std::invalid_argument("unknown argument: " + key);
        }
    }
    return args;
}

/// The columns of the one entity table: where an entity is.
struct Columns {
    float x[kPageSlots];
    float y[kPageSlots];
    float z[kPageSlots];
};

/// One entity type, whose only replicated property is the position, in steps of a centimetre.
inline ReplicationSchema MakeSchema() {
    ReplicationSchema schema;
    schema.AddType(0, {PropertyDescriptor::Position(TESSERA_COLUMN(Columns, x), TESSERA_COLUMN(Columns, y),
                                                    TESSERA_COLUMN(Columns, z), 0.01f, 2048.0f)});
    return schema;
}

/// A fingerprint of the replicated state of a set of entities that does not depend on the order they are
/// added in: the sum, modulo 2^64, of a hash of each entity's id and quantized words.
class StateChecksum {
public:
    void Add(uint64_t entity_raw, const uint32_t* words, uint32_t word_count) {
        uint64_t hash = 14695981039346656037ull;
        Mix(hash, entity_raw);
        for (uint32_t i = 0; i < word_count; ++i) {
            Mix(hash, words[i]);
        }
        sum_ += hash;
        ++count_;
    }
    uint64_t Value() const {
        return sum_;
    }
    uint64_t Count() const {
        return count_;
    }

private:
    // FNV-1a over the eight bytes of a value.
    static void Mix(uint64_t& hash, uint64_t value) {
        for (int shift = 0; shift < 64; shift += 8) {
            hash = (hash ^ ((value >> shift) & 0xFFu)) * 1099511628211ull;
        }
    }
    uint64_t sum_ = 0;
    uint64_t count_ = 0;
};

inline std::string Hex(uint64_t value) {
    char text[32];
    std::snprintf(text, sizeof(text), "0x%016llx", static_cast<unsigned long long>(value));
    return text;
}

/// One line, flushed, so that whoever reads the pipe sees it at once.
inline void PrintJson(const std::string& line) {
    std::fputs(line.c_str(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

inline int64_t SystemMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/// Passes everything to another transport and adds up the time that `Poll` and `Send` (and `Disconnect`)
/// take.
class TimedTransport final : public ITransport {
public:
    explicit TimedTransport(ITransport& inner) : inner_(inner) {}

    void Poll(std::vector<tessera::net::TransportEvent>& out) override {
        const auto start = Clock::now();
        inner_.Poll(out);
        poll_ns_ += Elapsed(start);
    }
    bool Send(tessera::net::TransportConnection connection, tessera::net::Channel channel,
              std::span<const uint8_t> data) override {
        const auto start = Clock::now();
        const bool sent = inner_.Send(connection, channel, data);
        send_ns_ += Elapsed(start);
        return sent;
    }
    void Disconnect(tessera::net::TransportConnection connection) override {
        const auto start = Clock::now();
        inner_.Disconnect(connection);
        send_ns_ += Elapsed(start);
    }
    tessera::net::TransportStats Stats() const noexcept override {
        return inner_.Stats();
    }

    /// The nanoseconds spent in `Poll` since the last call.
    uint64_t TakePollNs() {
        const uint64_t ns = poll_ns_;
        poll_ns_ = 0;
        return ns;
    }
    /// The nanoseconds spent in `Send` and `Disconnect` since the last call.
    uint64_t TakeSendNs() {
        const uint64_t ns = send_ns_;
        send_ns_ = 0;
        return ns;
    }

private:
    using Clock = std::chrono::steady_clock;
    static uint64_t Elapsed(Clock::time_point start) {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
    }

    ITransport& inner_;
    uint64_t poll_ns_ = 0;
    uint64_t send_ns_ = 0;
};

} // namespace h2h
