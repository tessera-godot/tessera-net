// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "tessera/net/replication/bit_stream.hpp"
#include "tessera/net/replication/property_schema.hpp"

#include <cstdint>

namespace tessera::net {

// A buffer is a run of records followed by a terminator. One record holds the state of one entity:
//
//   gap        VarUInt  compact id minus the previous record's compact id (the first one counts from -1),
//                       so it is at least 1; 0 is the terminator
//   complete   1 bit    1: every property follows in full; 0: only what differs from a confirmed state
//   age - 1    VarUInt  (partial only) how many ticks before this one that confirmed state was sent, minus 1
//   mask       one bit per property of the type, in schema order (partial only): 1 if it follows
//   values     each property that follows, written by its codec

/// Writes one record. `base` is the confirmed state to write against (`base_age` ticks old, at least 1), or
/// nullptr to write a complete record. A partial record marks only the properties whose words differ.
void WriteRecord(const TypeSchema& type, uint32_t id_gap, uint32_t base_age, const uint32_t* base,
                 const uint32_t* current, BitWriter& writer);

/// Ends a buffer.
void WriteTerminator(BitWriter& writer);

/// Reads the start of a record: the gap (0 means the terminator, and nothing else is read), whether it is
/// complete and, if not, the age of its baseline. Returns false if the data ends early or a number does not
/// fit.
bool ReadRecordHead(BitReader& reader, uint32_t* gap, bool* complete, uint32_t* base_age);

/// Reads the rest of a record of `type` into `out` (`type.words` words). A partial record starts from `base`.
/// Returns false, leaving `out` untouched, if the data ends early or a value is outside its declared range.
bool ReadRecordBody(const TypeSchema& type, BitReader& reader, bool complete, const uint32_t* base,
                    uint32_t* out);

} // namespace tessera::net
