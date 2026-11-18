// SPDX-License-Identifier: Apache-2.0
// A small, strict JSON reader. It reads the standard (RFC 8259) and nothing more: no comments, no trailing
// commas, no single quotes, no NaN or Infinity, no repeated keys. Every error says where it is and, when
// there is an obvious correction, what to change. The size of the text, how deep it nests and how long a
// string may be are limited, so a hostile or broken file ends in an error and never in a crash.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::schema::json {

/// A place in the text: counted from 1, the column in bytes. A line break is "\n" ("\r\n" counts as one).
struct Position {
    uint32_t line = 1;
    uint32_t column = 1;
};

class Value;

/// One key and value of an object, in the order they are written.
struct Member;

enum class Type { Null, Bool, Integer, Number, String, Array, Object };

class Value {
public:
    Value() = default;

    Type GetType() const noexcept {
        return type_;
    }
    bool IsInteger() const noexcept {
        return type_ == Type::Integer;
    }
    /// Integer or Number: both can be read as a double.
    bool IsNumeric() const noexcept {
        return type_ == Type::Integer || type_ == Type::Number;
    }
    bool AsBool() const noexcept {
        return bool_;
    }
    int64_t AsInteger() const noexcept {
        return integer_;
    }
    /// The value of a Number, or of an Integer converted to a double.
    double AsNumber() const noexcept {
        return type_ == Type::Integer ? static_cast<double>(integer_) : number_;
    }
    const std::string& AsString() const noexcept {
        return text_;
    }
    const std::vector<Value>& Items() const noexcept {
        return items_;
    }
    const std::vector<Member>& Members() const noexcept;

    /// Where the value starts in the text.
    Position position;

    /// The value of the member called `key`, or nullptr. Only for an object.
    const Value* Find(std::string_view key) const noexcept;

    // Used by the reader to build a value.
    static Value MakeNull(Position where);
    static Value MakeBool(Position where, bool value);
    static Value MakeInteger(Position where, int64_t value);
    static Value MakeNumber(Position where, double value);
    static Value MakeString(Position where, std::string value);
    static Value MakeArray(Position where);
    static Value MakeObject(Position where);
    void Append(Value item);
    void Append(std::string key, Position key_position, Value value);

private:
    Type type_ = Type::Null;
    bool bool_ = false;
    int64_t integer_ = 0;
    double number_ = 0.0;
    std::string text_;
    std::vector<Value> items_;
    std::vector<Member> members_;
};

struct Member {
    std::string key;
    Position key_position;
    Value value;
};

inline const std::vector<Member>& Value::Members() const noexcept {
    return members_;
}

struct Limits {
    size_t max_bytes = size_t{1} << 20; ///< Size of the whole text.
    uint32_t max_depth = 16;            ///< How many arrays and objects may be open at once.
    size_t max_string = 1024;           ///< Bytes of one string, after the escapes are read.
};

struct Error {
    Position position;
    std::string message;
    std::string fix; ///< How to correct it, or empty when there is no obvious way.
};

struct ParseResult {
    std::optional<Value> value;
    std::optional<Error> error;
};

/// Reads one JSON value (and nothing after it). A UTF-8 byte order mark at the start is skipped.
ParseResult Parse(std::string_view text, const Limits& limits = {});

} // namespace tessera::schema::json
