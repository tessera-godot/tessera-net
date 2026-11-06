// SPDX-License-Identifier: Apache-2.0
#include "tessera/net/replication/record_codec.hpp"

#include "tessera/net/replication/property_codecs.hpp"

#include <algorithm>
#include <cstring>

namespace tessera::net {

void WriteRecord(const TypeSchema& type, uint32_t id_gap, uint32_t base_age, const uint32_t* base,
                 const uint32_t* current, BitWriter& writer) {
    writer.WriteVarUInt(id_gap);
    const bool complete = base == nullptr;
    writer.WriteBool(complete);
    if (complete) {
        for (const PropertyDescriptor& property : type.properties) {
            WriteAbsolute(property, current + property.word_offset, writer);
        }
        return;
    }
    writer.WriteVarUInt(base_age - 1);
    uint64_t mask = 0;
    for (size_t i = 0; i < type.properties.size(); ++i) {
        const PropertyDescriptor& property = type.properties[i];
        if (std::memcmp(base + property.word_offset, current + property.word_offset,
                        property.words * sizeof(uint32_t)) != 0) {
            mask |= 1ull << i;
        }
    }
    writer.WriteBits(mask, static_cast<uint32_t>(type.properties.size()));
    for (size_t i = 0; i < type.properties.size(); ++i) {
        if ((mask >> i) & 1ull) {
            const PropertyDescriptor& property = type.properties[i];
            WriteDelta(property, base + property.word_offset, current + property.word_offset, writer);
        }
    }
}

void WriteTerminator(BitWriter& writer) {
    writer.WriteVarUInt(0);
}

bool ReadRecordHead(BitReader& reader, uint32_t* gap, bool* complete, uint32_t* base_age) {
    const uint64_t read_gap = reader.ReadVarUInt();
    if (!reader.ok() || read_gap > UINT32_MAX) {
        return false;
    }
    *gap = static_cast<uint32_t>(read_gap);
    *complete = false;
    *base_age = 0;
    if (read_gap == 0) {
        return true; // the terminator
    }
    *complete = reader.ReadBool();
    if (!*complete) {
        const uint64_t age = reader.ReadVarUInt();
        if (!reader.ok() || age >= UINT32_MAX) {
            return false;
        }
        *base_age = static_cast<uint32_t>(age) + 1;
    }
    return reader.ok();
}

bool ReadRecordBody(const TypeSchema& type, BitReader& reader, bool complete, const uint32_t* base,
                    uint32_t* out) {
    uint32_t scratch[ReplicationSchema::kMaxWordsPerEntity];
    if (complete) {
        for (const PropertyDescriptor& property : type.properties) {
            if (!ReadAbsolute(property, reader, scratch + property.word_offset)) {
                return false;
            }
        }
    } else {
        std::memcpy(scratch, base, type.words * sizeof(uint32_t));
        const uint64_t mask = reader.ReadBits(static_cast<uint32_t>(type.properties.size()));
        if (!reader.ok()) {
            return false;
        }
        for (size_t i = 0; i < type.properties.size(); ++i) {
            if (((mask >> i) & 1ull) == 0) {
                continue;
            }
            const PropertyDescriptor& property = type.properties[i];
            if (!ReadDelta(property, reader, base + property.word_offset, scratch + property.word_offset)) {
                return false;
            }
        }
    }
    std::memcpy(out, scratch, type.words * sizeof(uint32_t));
    return true;
}

} // namespace tessera::net
