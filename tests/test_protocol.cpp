// SPDX-License-Identifier: Apache-2.0
#include "tessera/net/protocol.hpp"
#include "tessera/net/replication/bit_stream.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <random>
#include <vector>

using namespace tessera::net;

namespace {

VisibilityEvent MakeEvent(VisibilityEvent::Kind kind, uint64_t raw, uint32_t compact_id) {
    return VisibilityEvent{kind, EntityId::FromRaw(raw), compact_id};
}

EventBatch MakeBatch(uint64_t number, uint64_t tick, size_t count) {
    EventBatch batch{number, tick, {}};
    for (size_t i = 0; i < count; ++i) {
        const auto kind = static_cast<VisibilityEvent::Kind>(i % 3);
        batch.events.push_back(
            MakeEvent(kind, 0x0123456789ABCDEFull + i * 0x10001ull, static_cast<uint32_t>(i * 7)));
    }
    return batch;
}

bool SameEvents(const std::vector<VisibilityEvent>& a, const std::vector<VisibilityEvent>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].kind != b[i].kind || a[i].id != b[i].id || a[i].compact_id != b[i].compact_id) {
            return false;
        }
    }
    return true;
}

std::vector<std::vector<uint8_t>> OneOfEachMessage() {
    std::vector<std::vector<uint8_t>> messages(5);
    Encode(ClientHello{kProtocolVersion, 0xFEDCBA9876543210ull}, messages[0]);
    Encode(ServerHello{true, RejectReason::None, kProtocolVersion, 17, 30, 65536, 32}, messages[1]);
    Encode(MakeBatch(3, 900, 20), messages[2]);
    EncodeStateBuffer(StateBufferHeader{900, 3}, std::vector<uint8_t>{1, 2, 3, 4, 5}, messages[3]);
    Encode(Ack{900, 0xA5A5A5A5u}, messages[4]);
    return messages;
}

} // namespace

TESSERA_TEST(each_message_round_trips) {
    {
        std::vector<uint8_t> bytes;
        Encode(ClientHello{kProtocolVersion, ~0ull}, bytes);
        ClientHello hello{};
        TESSERA_CHECK(Decode(bytes, &hello) && hello.version == kProtocolVersion &&
                      hello.schema_hash == ~0ull);
    }
    {
        std::vector<uint8_t> bytes;
        Encode(ServerHello{true, RejectReason::None, 1, 4'000'000'000u, 240, 16'777'216, 255}, bytes);
        ServerHello hello{};
        TESSERA_CHECK(Decode(bytes, &hello) && hello.accepted && hello.reason == RejectReason::None &&
                      hello.version == 1);
        TESSERA_CHECK(hello.connection_id == 4'000'000'000u && hello.tick_rate == 240 &&
                      hello.max_visible == 16'777'216 && hello.max_baseline_age_ticks == 255);
    }
    for (const size_t count : {size_t{0}, size_t{1}, size_t{1000}}) {
        std::vector<uint8_t> bytes;
        const EventBatch batch = MakeBatch((1ull << 63) + 5, 1ull << 63, count);
        Encode(batch, bytes);
        EventBatch back;
        TESSERA_CHECK(Decode(bytes, &back) && back.number == batch.number && back.tick == batch.tick);
        TESSERA_CHECK(SameEvents(back.events, batch.events));
    }
    {
        std::vector<uint8_t> bytes;
        Encode(Ack{1ull << 63, 0xFFFFFFFFu}, bytes);
        Ack ack{};
        TESSERA_CHECK(Decode(bytes, &ack) && ack.latest_tick == (1ull << 63) && ack.mask == 0xFFFFFFFFu);
    }
}

TESSERA_TEST(server_hello_rejections_round_trip_with_their_reason) {
    for (const RejectReason reason :
         {RejectReason::VersionMismatch, RejectReason::SchemaMismatch, RejectReason::ServerFull}) {
        std::vector<uint8_t> bytes;
        Encode(ServerHello{false, reason, 0, 0, 0, 0, 0}, bytes);
        ServerHello hello{};
        TESSERA_CHECK(Decode(bytes, &hello) && !hello.accepted && hello.reason == reason);
    }
}

TESSERA_TEST(state_buffer_payload_is_returned_unchanged_and_may_be_empty) {
    for (const size_t size : {size_t{0}, size_t{1}, size_t{6000}}) {
        std::vector<uint8_t> payload(size);
        for (size_t i = 0; i < size; ++i) {
            payload[i] = static_cast<uint8_t>(i * 31 + 7);
        }
        std::vector<uint8_t> bytes;
        EncodeStateBuffer(StateBufferHeader{123456, 42}, payload, bytes);
        StateBufferHeader header{};
        std::span<const uint8_t> back;
        TESSERA_CHECK(DecodeStateBuffer(bytes, &header, &back));
        TESSERA_CHECK(header.tick == 123456 && header.batches_sent == 42);
        TESSERA_CHECK(back.size() == payload.size() && std::equal(back.begin(), back.end(), payload.begin()));
    }
}

TESSERA_TEST(peek_kind_reads_the_first_byte_and_rejects_unknown_kinds) {
    const std::vector<std::vector<uint8_t>> messages = OneOfEachMessage();
    const MessageKind expected[] = {MessageKind::ClientHello, MessageKind::ServerHello,
                                    MessageKind::EventBatch, MessageKind::StateBuffer, MessageKind::Ack};
    for (size_t i = 0; i < messages.size(); ++i) {
        MessageKind kind{};
        TESSERA_CHECK(PeekKind(messages[i], &kind) && kind == expected[i]);
    }
    MessageKind kind{};
    for (const uint8_t unknown : {uint8_t{0}, uint8_t{6}, uint8_t{255}}) {
        const std::vector<uint8_t> bytes = {unknown, 1, 2, 3};
        TESSERA_CHECK(!PeekKind(bytes, &kind));
    }
    TESSERA_CHECK(!PeekKind(std::vector<uint8_t>{}, &kind));
}

TESSERA_TEST(truncated_messages_are_refused_at_every_length) {
    const std::vector<std::vector<uint8_t>> messages = OneOfEachMessage();
    for (size_t m = 0; m < messages.size(); ++m) {
        // A state buffer with a longer prefix is just a shorter payload; only the header has to be whole.
        size_t limit = messages[m].size();
        if (m == 3) {
            limit -= 5; // the payload of OneOfEachMessage
        }
        for (size_t length = 0; length < limit; ++length) {
            const std::span<const uint8_t> prefix(messages[m].data(), length);
            ClientHello hello{};
            ServerHello server{};
            EventBatch batch;
            StateBufferHeader header{};
            std::span<const uint8_t> payload;
            Ack ack{};
            const bool accepted = m == 0   ? Decode(prefix, &hello)
                                  : m == 1 ? Decode(prefix, &server)
                                  : m == 2 ? Decode(prefix, &batch)
                                  : m == 3 ? DecodeStateBuffer(prefix, &header, &payload)
                                           : Decode(prefix, &ack);
            TESSERA_CHECK(!accepted);
        }
    }
}

TESSERA_TEST(messages_with_extra_bytes_or_the_wrong_kind_are_refused) {
    std::vector<std::vector<uint8_t>> messages = OneOfEachMessage();
    ClientHello hello{};
    messages[0].push_back(0x55);
    TESSERA_CHECK(!Decode(messages[0], &hello));
    ServerHello server{};
    TESSERA_CHECK(!Decode(messages[0], &server)); // a ClientHello is not a ServerHello
    Ack ack{};
    messages[4].push_back(0);
    TESSERA_CHECK(!Decode(messages[4], &ack));
}

TESSERA_TEST(random_bytes_never_crash_the_decoders) {
    std::mt19937 random(2024);
    for (int round = 0; round < 10000; ++round) {
        std::vector<uint8_t> bytes(random() % 40);
        for (uint8_t& b : bytes) {
            b = static_cast<uint8_t>(random());
        }
        if (!bytes.empty() && round % 2 == 0) {
            bytes[0] = static_cast<uint8_t>(1 + random() % 5); // a known kind, so the body is parsed
        }
        ClientHello hello{};
        ServerHello server{};
        EventBatch batch;
        StateBufferHeader header{};
        std::span<const uint8_t> payload;
        Ack ack{};
        Decode(bytes, &hello);
        Decode(bytes, &server);
        Decode(bytes, &batch);
        DecodeStateBuffer(bytes, &header, &payload);
        Decode(bytes, &ack);
        MessageKind kind{};
        PeekKind(bytes, &kind);
    }
    TESSERA_CHECK(true); // reaching this line is the result
}

TESSERA_TEST(an_event_count_larger_than_the_message_is_refused_without_allocating) {
    std::vector<uint8_t> bytes;
    BitWriter writer(bytes);
    writer.WriteBits(static_cast<uint64_t>(MessageKind::EventBatch), 8);
    writer.WriteVarUInt(1);
    writer.WriteVarUInt(1);
    writer.WriteVarUInt(4'000'000'000ull); // four billion events, in a message that has none
    writer.Finish();
    EventBatch batch;
    TESSERA_CHECK(!Decode(bytes, &batch));
    TESSERA_CHECK(batch.events.capacity() < 1000);
}

TESSERA_TEST(an_event_with_an_unknown_kind_or_a_compact_id_out_of_range_is_refused) {
    auto batch_with = [](uint64_t kind, uint64_t compact_id) {
        std::vector<uint8_t> bytes;
        BitWriter writer(bytes);
        writer.WriteBits(static_cast<uint64_t>(MessageKind::EventBatch), 8);
        writer.WriteVarUInt(1);
        writer.WriteVarUInt(1);
        writer.WriteVarUInt(1);
        writer.WriteBits(kind, 2);
        writer.WriteBits(0x1234, 64);
        writer.WriteVarUInt(compact_id);
        writer.Finish();
        return bytes;
    };
    EventBatch batch;
    TESSERA_CHECK(Decode(batch_with(0, 5), &batch));
    TESSERA_CHECK(!Decode(batch_with(3, 5), &batch));          // only 0, 1 and 2 are kinds
    TESSERA_CHECK(!Decode(batch_with(0, 1ull << 33), &batch)); // does not fit in a compact id
}

TESSERA_TEST(ack_window_returns_each_tick_once) {
    AckWindow window;
    TESSERA_CHECK(
        (window.Add(Ack{10, 0b101}) == std::vector<uint64_t>{7, 9, 10})); // bits 0 and 2 are ticks 9 and 7
    TESSERA_CHECK(window.Add(Ack{10, 0b101}).empty());                    // the same ack again
    TESSERA_CHECK(
        (window.Add(Ack{11, 0b111}) == std::vector<uint64_t>{8, 11})); // 10 and 9 were known, 8 is new
    TESSERA_CHECK(window.Latest() == 11);
}

TESSERA_TEST(ack_window_ignores_ticks_older_than_64) {
    AckWindow window;
    window.Add(Ack{200, 0});
    TESSERA_CHECK(
        window.Add(Ack{130, 0xFFFFFFFFu}).empty()); // 130 down to 98 are all more than 63 ticks behind 200
    TESSERA_CHECK(window.Latest() == 200);
}

TESSERA_TEST(ack_window_handles_a_jump_forward_and_a_late_older_ack) {
    AckWindow window;
    TESSERA_CHECK((window.Add(Ack{10, 0}) == std::vector<uint64_t>{10}));
    TESSERA_CHECK((window.Add(Ack{100, 0}) == std::vector<uint64_t>{100}));
    TESSERA_CHECK((window.Add(Ack{50, 0}) == std::vector<uint64_t>{50})); // late, but inside the window
    TESSERA_CHECK(window.Latest() == 100);                                // and it does not move time back
    TESSERA_CHECK(window.Add(Ack{50, 0}).empty());
}

TESSERA_TEST(ack_window_with_latest_zero_and_a_full_mask_does_not_underflow) {
    AckWindow window;
    TESSERA_CHECK((window.Add(Ack{0, 0xFFFFFFFFu}) == std::vector<uint64_t>{0}));
    TESSERA_CHECK((window.Add(Ack{3, 0xFFFFFFFFu}) ==
                   std::vector<uint64_t>{1, 2, 3})); // ticks 2, 1 and 0 below 3; 0 known
}
