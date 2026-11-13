// SPDX-License-Identifier: Apache-2.0
#include "tessera/net/protocol.hpp"
#include "tessera/net/replication/bit_stream.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <initializer_list>
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

// An input message with one entry for each sequence number, each holding `size` bytes of `0x10 + index`.
Input MakeInput(std::initializer_list<uint32_t> sequences, size_t size) {
    Input input;
    uint8_t index = 0;
    for (const uint32_t sequence : sequences) {
        InputEntry entry;
        entry.sequence = sequence;
        entry.size = static_cast<uint8_t>(size);
        for (size_t i = 0; i < size && i < entry.bytes.size(); ++i) {
            entry.bytes[i] = static_cast<uint8_t>(0x10 + index);
        }
        input.entries.push_back(entry);
        ++index;
    }
    return input;
}

// A hand-written `Input` message: the sequence of the newest, the count, then for each entry (oldest first)
// its `back`, its size and `size` bytes.
struct RawEntry {
    uint64_t back;
    uint64_t size;
};

std::vector<uint8_t> RawInput(uint64_t newest, uint64_t count, const std::vector<RawEntry>& entries,
                              bool extra_byte = false, uint64_t kind = 6) {
    std::vector<uint8_t> bytes;
    BitWriter writer(bytes);
    writer.WriteBits(kind, 8);
    writer.WriteVarUInt(newest);
    writer.WriteVarUInt(count);
    for (const RawEntry& entry : entries) {
        writer.WriteVarUInt(entry.back);
        writer.WriteVarUInt(entry.size);
        for (uint64_t i = 0; i < entry.size; ++i) {
            writer.WriteBits(0xAB, 8);
        }
    }
    if (extra_byte) {
        writer.WriteBits(0x55, 8);
    }
    writer.Finish();
    return bytes;
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
    std::vector<std::vector<uint8_t>> all = messages;
    all.emplace_back();
    Encode(MakeInput({7}, 2), all.back());
    const MessageKind expected[] = {MessageKind::ClientHello, MessageKind::ServerHello,
                                    MessageKind::EventBatch,  MessageKind::StateBuffer,
                                    MessageKind::Ack,         MessageKind::Input};
    for (size_t i = 0; i < all.size(); ++i) {
        MessageKind kind{};
        TESSERA_CHECK(PeekKind(all[i], &kind) && kind == expected[i]);
    }
    MessageKind kind{};
    for (const uint8_t unknown : {uint8_t{0}, uint8_t{7}, uint8_t{255}}) {
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
            bytes[0] = static_cast<uint8_t>(1 + random() % 6); // a known kind, so the body is parsed
        }
        Input input;
        Decode(bytes, &input);
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

TESSERA_TEST(input_round_trips_with_one_to_four_entries) {
    for (uint32_t n = 1; n <= 4; ++n) {
        Input input;
        for (uint32_t i = 0; i < n; ++i) {
            InputEntry entry;
            entry.sequence = 100 + i;
            entry.size = static_cast<uint8_t>(i);
            for (uint32_t b = 0; b < i; ++b) {
                entry.bytes[b] = static_cast<uint8_t>(0x10 + i);
            }
            input.entries.push_back(entry);
        }
        std::vector<uint8_t> bytes;
        Encode(input, bytes);
        Input back;
        TESSERA_CHECK(Decode(bytes, &back));
        TESSERA_CHECK(back.entries.size() == n);
        for (uint32_t i = 0; i < n && i < back.entries.size(); ++i) {
            TESSERA_CHECK(back.entries[i].sequence == 100 + i && back.entries[i].size == i);
            for (uint32_t b = 0; b < i && b < back.entries[i].Bytes().size(); ++b) {
                TESSERA_CHECK(back.entries[i].Bytes()[b] == 0x10 + i);
            }
        }
    }
}

TESSERA_TEST(input_round_trips_empty_and_maximum_sized_bytes) {
    Input input = MakeInput({5, 6}, 0);
    input.entries[1].size = static_cast<uint8_t>(kMaxInputBytes);
    input.entries[1].bytes.fill(0xFF);
    std::vector<uint8_t> bytes;
    Encode(input, bytes);
    Input back;
    TESSERA_CHECK(Decode(bytes, &back) && back.entries.size() == 2);
    if (back.entries.size() == 2) {
        TESSERA_CHECK(back.entries[0].size == 0 && back.entries[0].Bytes().empty());
        TESSERA_CHECK(back.entries[1].size == kMaxInputBytes);
        for (const uint8_t b : back.entries[1].Bytes()) {
            TESSERA_CHECK(b == 0xFF);
        }
    }
}

TESSERA_TEST(input_sequences_across_the_uint32_wrap_round_trip) {
    const Input input = MakeInput({0xFFFFFFFEu, 0xFFFFFFFFu, 1u}, 1);
    std::vector<uint8_t> bytes;
    Encode(input, bytes);
    Input back;
    TESSERA_CHECK(Decode(bytes, &back) && back.entries.size() == 3);
    if (back.entries.size() == 3) {
        TESSERA_CHECK(back.entries[0].sequence == 0xFFFFFFFEu && back.entries[1].sequence == 0xFFFFFFFFu &&
                      back.entries[2].sequence == 1u);
    }
}

TESSERA_TEST(input_truncated_at_every_length_is_refused) {
    const Input input = MakeInput({1000, 1001, 1002, 1003}, 16);
    std::vector<uint8_t> bytes;
    Encode(input, bytes);
    for (size_t length = 0; length < bytes.size(); ++length) {
        Input result;
        result.entries.resize(7);
        TESSERA_CHECK(!Decode(std::span<const uint8_t>(bytes.data(), length), &result));
        TESSERA_CHECK(result.entries.size() == 7); // left untouched
    }
    Input whole;
    TESSERA_CHECK(Decode(bytes, &whole));
}

TESSERA_TEST(an_input_entry_whose_sequence_works_out_to_zero_is_refused) {
    Input input;
    TESSERA_CHECK(!Decode(RawInput(1, 2, {{1, 1}, {0, 1}}), &input)); // the older entry would be 0
    TESSERA_CHECK(Decode(RawInput(2, 2, {{1, 1}, {0, 1}}), &input));  // 1 and 2 are fine
}

TESSERA_TEST(input_with_a_bad_count_size_order_or_extra_bytes_is_refused) {
    Input input;
    TESSERA_CHECK(Decode(RawInput(10, 1, {{0, 1}}), &input)); // the shape the checks below break
    TESSERA_CHECK(!Decode(RawInput(10, 0, {}), &input));      // no entries
    TESSERA_CHECK(!Decode(RawInput(10, 5, {{4, 1}, {3, 1}, {2, 1}, {1, 1}, {0, 1}}), &input)); // five
    TESSERA_CHECK(!Decode(RawInput(10, 1, {{0, 17}}), &input));                                // size 17
    TESSERA_CHECK(!Decode(RawInput(10, 2, {{1, 1}, {1, 1}}), &input));   // back does not fall
    TESSERA_CHECK(!Decode(RawInput(10, 2, {{1, 1}, {2, 1}}), &input));   // back rises
    TESSERA_CHECK(!Decode(RawInput(10, 2, {{3, 1}, {2, 1}}), &input));   // the newest has back 2
    TESSERA_CHECK(!Decode(RawInput(10, 1, {{0, 1}}, true), &input));     // a byte too many
    TESSERA_CHECK(!Decode(RawInput(10, 1, {{0, 1}}, false, 5), &input)); // another kind
}

TESSERA_TEST(random_bytes_never_crash_the_input_decoder) {
    std::mt19937 random(77);
    for (int round = 0; round < 10000; ++round) {
        std::vector<uint8_t> bytes(random() % 100);
        for (uint8_t& b : bytes) {
            b = static_cast<uint8_t>(random());
        }
        if (!bytes.empty() && round % 2 == 0) {
            bytes[0] = 6;
        }
        Input input;
        if (Decode(bytes, &input)) {
            TESSERA_CHECK(input.entries.size() >= 1 && input.entries.size() <= kMaxInputsPerMessage);
            for (const InputEntry& entry : input.entries) {
                TESSERA_CHECK(entry.size <= kMaxInputBytes);
            }
        }
    }
}

TESSERA_TEST(the_largest_input_message_fits_one_packet) {
    const Input input = MakeInput({0xFFFFFFFCu, 0xFFFFFFFDu, 0xFFFFFFFEu, 0xFFFFFFFFu}, kMaxInputBytes);
    std::vector<uint8_t> bytes;
    Encode(input, bytes);
    TESSERA_CHECK(bytes.size() <= 80 && bytes.size() < 1200);
}

TESSERA_TEST(encode_cuts_an_entry_size_above_the_maximum) {
    Input input = MakeInput({3}, 4);
    input.entries[0].size = 200;
    std::vector<uint8_t> bytes;
    Encode(input, bytes);
    Input back;
    TESSERA_CHECK(Decode(bytes, &back) && back.entries.size() == 1 && back.entries[0].size == kMaxInputBytes);
}

TESSERA_TEST(next_input_sequence_counts_up_and_skips_zero) {
    TESSERA_CHECK(NextInputSequence(1) == 2);
    TESSERA_CHECK(NextInputSequence(0xFFFFFFFEu) == 0xFFFFFFFFu);
    TESSERA_CHECK(NextInputSequence(0xFFFFFFFFu) == 1);
    TESSERA_CHECK(NextInputSequence(0) == 1);
}

TESSERA_TEST(input_window_judges_new_repeated_and_late_entries) {
    InputWindow window;
    TESSERA_CHECK(window.Judge(5) == InputVerdict::New);
    window.Accept(5);
    TESSERA_CHECK(window.Judge(5) == InputVerdict::Repeated);
    window.Accept(8);
    TESSERA_CHECK(window.Judge(5) == InputVerdict::Repeated);
    TESSERA_CHECK(window.Judge(6) == InputVerdict::Late);
    TESSERA_CHECK(window.Judge(7) == InputVerdict::Late);
    TESSERA_CHECK(window.Judge(8) == InputVerdict::Repeated);
    TESSERA_CHECK(window.Judge(9) == InputVerdict::New);
}

TESSERA_TEST(input_window_wraps_around_uint32) {
    InputWindow window;
    window.Accept(0xFFFFFFFDu);
    window.Accept(0xFFFFFFFFu);
    TESSERA_CHECK(window.Judge(1) == InputVerdict::New);
    window.Accept(1);
    TESSERA_CHECK(window.Judge(0xFFFFFFFFu) == InputVerdict::Repeated);
    TESSERA_CHECK(window.Judge(0xFFFFFFFEu) == InputVerdict::Late);
    TESSERA_CHECK(window.Judge(0) == InputVerdict::Late);
    TESSERA_CHECK(window.Latest() == 1);
}

TESSERA_TEST(input_window_a_missed_entry_that_arrives_after_newer_ones_is_late) {
    InputWindow window;
    window.Accept(1);
    window.Accept(2);
    window.Accept(4);
    TESSERA_CHECK(window.Judge(3) == InputVerdict::Late); // never delivered, and 4 is already out
    TESSERA_CHECK(window.Judge(2) == InputVerdict::Repeated);
    TESSERA_CHECK(window.Judge(1) == InputVerdict::Repeated);
    window.Accept(44); // a jump of 40: 4 falls out of the 32 the window remembers
    TESSERA_CHECK(window.Judge(4) == InputVerdict::Late);
    TESSERA_CHECK(window.Judge(44) == InputVerdict::Repeated);
}

TESSERA_TEST(input_window_judging_does_not_change_it_until_accepted) {
    InputWindow window;
    TESSERA_CHECK(window.Judge(9) == InputVerdict::New);
    TESSERA_CHECK(window.Judge(9) == InputVerdict::New);
    TESSERA_CHECK(window.Latest() == 0);
    window.Accept(0); // 0 is never a sequence
    TESSERA_CHECK(window.Latest() == 0);
    window.Accept(9);
    window.Accept(7); // late: not new, so it changes nothing
    TESSERA_CHECK(window.Latest() == 9 && window.Judge(7) == InputVerdict::Late);
    window.Accept(9);
    TESSERA_CHECK(window.Latest() == 9);
}
