// SPDX-License-Identifier: Apache-2.0
#include "tessera/ext/schema_description.hpp"
#include "tessera/ext/value_conversion.hpp"
#include "tessera/net/replication/property_codecs.hpp"
#include "test_support.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace tessera::ext;
namespace net = tessera::net;

namespace {

constexpr double kPi = 3.14159265358979323846;

// The columns the test writes its numbers into. Slot 0 is always the one that is read.
struct Columns {
    float x[2];
    float y[2];
    float z[2];
    float angle[2];
    int64_t signed_value[2];
    uint64_t unsigned_value[2];
};

PropertyDescription Described(const std::string& name, const std::string& kind) {
    PropertyDescription p;
    p.name = name;
    p.kind = kind;
    return p;
}
PropertyDescription Position(double step = 0.01, double range = 2048.0) {
    PropertyDescription p = Described("position", "position");
    p.step = step;
    p.range = range;
    return p;
}
PropertyDescription Angle(uint32_t bits) {
    PropertyDescription p = Described("angle", "angle");
    p.bits = bits;
    return p;
}
PropertyDescription Integer(int64_t min_value, int64_t max_value, bool is_signed) {
    PropertyDescription p = Described("number", "integer");
    p.min_value = min_value;
    p.max_value = max_value;
    p.is_signed = is_signed;
    return p;
}
PropertyDescription Mask(uint32_t bits) {
    PropertyDescription p = Described("mask", "mask");
    p.bits = bits;
    return p;
}
PropertyDescription Choice(uint32_t count) {
    PropertyDescription p = Described("choice", "choice");
    p.count = count;
    return p;
}

// A type of one or more properties, built through the real code, with its descriptors pointed at the test's
// own columns (the client never reads columns, so the built ones are only nominal).
class Rig {
public:
    explicit Rig(std::vector<PropertyDescription> properties) {
        const TypeDescription type{1, std::move(properties)};
        BuildResult result = BuildSchema(std::span<const TypeDescription>(&type, 1));
        TESSERA_CHECK(result.schema != nullptr);
        if (!result.schema) {
            return;
        }
        built_ = std::move(result.schema);
        for (const net::PropertyDescriptor& built : built_->Schema().Find(1)->properties) {
            net::PropertyDescriptor d = built;
            switch (d.kind) {
            case net::PropertyKind::Position:
                d.source[0] = TESSERA_COLUMN(Columns, x);
                d.source[1] = TESSERA_COLUMN(Columns, y);
                d.source[2] = TESSERA_COLUMN(Columns, z);
                break;
            case net::PropertyKind::Angle:
                d.source[0] = TESSERA_COLUMN(Columns, angle);
                break;
            case net::PropertyKind::Integer:
                d.source[0] = d.is_signed ? TESSERA_COLUMN(Columns, signed_value)
                                          : TESSERA_COLUMN(Columns, unsigned_value);
                break;
            case net::PropertyKind::Bits:
            case net::PropertyKind::Enum:
                d.source[0] = TESSERA_COLUMN(Columns, unsigned_value);
                break;
            }
            descriptors_.push_back(d);
        }
        words_.assign(32, 0);
    }

    // Quantizes the columns as the server would, then converts property `index` back.
    Value Roundtrip(size_t index, const Columns& columns) {
        const net::PropertyDescriptor& d = descriptors_[index];
        for (size_t i = 0; i < descriptors_.size(); ++i) {
            net::QuantizeProperty(descriptors_[i], reinterpret_cast<const std::byte*>(&columns), 0,
                                  words_.data() + descriptors_[i].word_offset);
        }
        return ConvertProperty(d, KindOf(index), words_.data());
    }

    // Converts words that were not made from columns.
    Value FromWords(size_t index, std::initializer_list<uint32_t> words) {
        std::fill(words_.begin(), words_.end(), 0);
        size_t at = descriptors_[index].word_offset;
        for (uint32_t w : words) {
            words_[at++] = w;
        }
        return ConvertProperty(descriptors_[index], KindOf(index), words_.data());
    }

    const net::PropertyDescriptor& Descriptor(size_t index) const {
        return descriptors_[index];
    }

private:
    ValueKind KindOf(size_t index) const {
        return (*built_->Properties(1))[index].kind;
    }

    std::unique_ptr<BuiltSchema> built_;
    std::vector<net::PropertyDescriptor> descriptors_;
    std::vector<uint32_t> words_;
};

Columns Zero() {
    Columns c{};
    return c;
}

double Circular(double a, double b) {
    double d = std::fmod(std::fabs(a - b), 2.0 * kPi);
    return d > kPi ? 2.0 * kPi - d : d;
}

} // namespace

TESSERA_TEST(convert_position_is_within_one_step) {
    Rig rig({Position(0.01, 2048.0)});
    const float points[][3] = {{0, 0, 0}, {120.5f, 0, -40.25f}, {2048.0f, -2048.0f, 2047.99f}};
    for (const auto& p : points) {
        Columns c = Zero();
        c.x[0] = p[0];
        c.y[0] = p[1];
        c.z[0] = p[2];
        const Value v = rig.Roundtrip(0, c);
        TESSERA_CHECK(v.kind == Value::Kind::Vector3);
        TESSERA_CHECK(std::fabs(v.vec[0] - p[0]) <= 0.01 + 1e-6);
        TESSERA_CHECK(std::fabs(v.vec[1] - p[1]) <= 0.01 + 1e-6);
        TESSERA_CHECK(std::fabs(v.vec[2] - p[2]) <= 0.01 + 1e-6);
    }
}

TESSERA_TEST(convert_position_extremes_stay_inside_the_range) {
    Rig rig({Position(0.01, 2048.0)});
    const uint32_t top = static_cast<uint32_t>(rig.Descriptor(0).max_ordinal);
    const Value low = rig.FromWords(0, {0, 0, 0});
    const Value high = rig.FromWords(0, {top, top, top});
    for (int axis = 0; axis < 3; ++axis) {
        TESSERA_CHECK(low.vec[axis] >= -2048.0 && low.vec[axis] <= 2048.0);
        TESSERA_CHECK(high.vec[axis] >= -2048.0 && high.vec[axis] <= 2048.0);
        TESSERA_CHECK(low.vec[axis] < high.vec[axis]);
    }
}

TESSERA_TEST(convert_angle_in_radians) {
    Rig rig({Angle(12)});
    const double step = 2.0 * kPi / 4096.0;
    for (double radians : {0.0, kPi / 2.0, kPi, 2.0 * kPi - 1e-4}) {
        Columns c = Zero();
        c.angle[0] = static_cast<float>(radians);
        const Value v = rig.Roundtrip(0, c);
        TESSERA_CHECK(v.kind == Value::Kind::Float);
        TESSERA_CHECK(Circular(v.number, radians) <= step + 1e-6);
    }
}

TESSERA_TEST(convert_integer_signed_and_unsigned_bounds) {
    Rig small({Integer(-128, 127, true)});
    for (int64_t value : {int64_t{-128}, int64_t{-1}, int64_t{0}, int64_t{127}}) {
        Columns c = Zero();
        c.signed_value[0] = value;
        const Value v = small.Roundtrip(0, c);
        TESSERA_CHECK(v.kind == Value::Kind::Int);
        TESSERA_CHECK(v.integer == value);
    }
    Rig wide({Integer(0, 65535, false)});
    for (uint64_t value : {uint64_t{0}, uint64_t{1}, uint64_t{65535}}) {
        Columns c = Zero();
        c.unsigned_value[0] = value;
        TESSERA_CHECK(wide.Roundtrip(0, c).integer == static_cast<int64_t>(value));
    }
}

TESSERA_TEST(convert_integer_64_bit_extremes) {
    const int64_t lowest = std::numeric_limits<int64_t>::min();
    const int64_t highest = std::numeric_limits<int64_t>::max();
    Rig full({Integer(lowest, highest, true)});
    for (int64_t value : {lowest, lowest + 1, int64_t{-1}, int64_t{0}, highest - 1, highest}) {
        Columns c = Zero();
        c.signed_value[0] = value;
        TESSERA_CHECK(full.Roundtrip(0, c).integer == value);
    }
    Rig positive({Integer(0, highest, false)});
    for (int64_t value : {int64_t{0}, int64_t{1}, highest}) {
        Columns c = Zero();
        c.unsigned_value[0] = static_cast<uint64_t>(value);
        TESSERA_CHECK(positive.Roundtrip(0, c).integer == value);
    }
}

TESSERA_TEST(convert_flag_is_a_bool) {
    Rig rig({Described("flag", "flag")});
    for (uint64_t value : {uint64_t{0}, uint64_t{1}}) {
        Columns c = Zero();
        c.unsigned_value[0] = value;
        const Value v = rig.Roundtrip(0, c);
        TESSERA_CHECK(v.kind == Value::Kind::Bool);
        TESSERA_CHECK(v.flag == (value == 1));
    }
}

TESSERA_TEST(convert_mask_64_bits_keeps_every_bit) {
    Rig rig({Mask(64)});
    Columns c = Zero();
    c.unsigned_value[0] = 0xFFFFFFFFFFFFFFFFull;
    TESSERA_CHECK(rig.Roundtrip(0, c).integer == -1);
    c.unsigned_value[0] = 0x8000000000000001ull;
    TESSERA_CHECK(rig.Roundtrip(0, c).integer == std::numeric_limits<int64_t>::min() + 1);
    c.unsigned_value[0] = 0x00000001FFFFFFFFull;
    const Value v = rig.Roundtrip(0, c);
    TESSERA_CHECK(v.kind == Value::Kind::Int);
    TESSERA_CHECK(v.integer == 0x00000001FFFFFFFFll);
}

TESSERA_TEST(convert_mask_narrow_has_no_high_bits) {
    Rig rig({Mask(5)});
    Columns c = Zero();
    c.unsigned_value[0] = 0b10101;
    TESSERA_CHECK(rig.Roundtrip(0, c).integer == 0b10101);
    c.unsigned_value[0] = 0xFF; // only the five low bits are part of the mask
    TESSERA_CHECK(rig.Roundtrip(0, c).integer == 0b11111);
}

TESSERA_TEST(convert_choice_values) {
    Rig two({Choice(2)});
    Rig many({Choice(300)});
    for (uint64_t value : {uint64_t{0}, uint64_t{1}}) {
        Columns c = Zero();
        c.unsigned_value[0] = value;
        const Value v = two.Roundtrip(0, c);
        TESSERA_CHECK(v.kind == Value::Kind::Int);
        TESSERA_CHECK(v.integer == static_cast<int64_t>(value));
    }
    for (uint64_t value : {uint64_t{0}, uint64_t{1}, uint64_t{299}}) {
        Columns c = Zero();
        c.unsigned_value[0] = value;
        TESSERA_CHECK(many.Roundtrip(0, c).integer == static_cast<int64_t>(value));
    }
}

TESSERA_TEST(convert_second_property_uses_its_word_offset) {
    Rig rig({Position(), Integer(-1000, 1000, true)});
    Columns c = Zero();
    c.x[0] = 10.0f;
    c.y[0] = 20.0f;
    c.z[0] = 30.0f;
    c.signed_value[0] = -777;
    TESSERA_CHECK(rig.Descriptor(1).word_offset == 3);
    TESSERA_CHECK(rig.Roundtrip(1, c).integer == -777);
    TESSERA_CHECK(std::fabs(rig.Roundtrip(0, c).vec[1] - 20.0) <= 0.01 + 1e-6);
}

TESSERA_TEST(convert_never_yields_nan_or_infinity) {
    Rig rig({Position(), Angle(12)});
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    Columns c = Zero();
    c.x[0] = nan;
    c.y[0] = inf;
    c.z[0] = -inf;
    c.angle[0] = nan;
    const Value position = rig.Roundtrip(0, c);
    for (int axis = 0; axis < 3; ++axis) {
        TESSERA_CHECK(std::isfinite(position.vec[axis]));
    }
    TESSERA_CHECK(std::isfinite(rig.Roundtrip(1, c).number));
    c.angle[0] = inf;
    TESSERA_CHECK(std::isfinite(rig.Roundtrip(1, c).number));
}
