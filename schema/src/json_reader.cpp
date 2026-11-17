// SPDX-License-Identifier: Apache-2.0
#include "tessera/schema/json_reader.hpp"

#include <cerrno>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <utility>

namespace tessera::schema::json {

Value Value::MakeNull(Position where) {
    Value v;
    v.position = where;
    return v;
}

Value Value::MakeBool(Position where, bool value) {
    Value v;
    v.position = where;
    v.type_ = Type::Bool;
    v.bool_ = value;
    return v;
}

Value Value::MakeInteger(Position where, int64_t value) {
    Value v;
    v.position = where;
    v.type_ = Type::Integer;
    v.integer_ = value;
    return v;
}

Value Value::MakeNumber(Position where, double value) {
    Value v;
    v.position = where;
    v.type_ = Type::Number;
    v.number_ = value;
    return v;
}

Value Value::MakeString(Position where, std::string value) {
    Value v;
    v.position = where;
    v.type_ = Type::String;
    v.text_ = std::move(value);
    return v;
}

Value Value::MakeArray(Position where) {
    Value v;
    v.position = where;
    v.type_ = Type::Array;
    return v;
}

Value Value::MakeObject(Position where) {
    Value v;
    v.position = where;
    v.type_ = Type::Object;
    return v;
}

void Value::Append(Value item) {
    items_.push_back(std::move(item));
}

void Value::Append(std::string key, Position key_position, Value value) {
    members_.push_back(Member{std::move(key), key_position, std::move(value)});
}

const Value* Value::Find(std::string_view key) const noexcept {
    for (const Member& member : members_) {
        if (member.key == key) {
            return &member.value;
        }
    }
    return nullptr;
}

namespace {

bool IsDigit(char c) {
    return c >= '0' && c <= '9';
}

bool IsIdentifierStart(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool IsIdentifierPart(char c) {
    return IsIdentifierStart(c) || IsDigit(c);
}

bool StartsAValue(char c) {
    return c == '"' || c == '-' || IsDigit(c) || c == '[' || c == '{' || c == 't' || c == 'f' || c == 'n';
}

// How a byte is named in a message: the character itself when it can be seen, its value when not.
std::string Describe(char c) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte >= 0x20 && byte < 0x7F) {
        return std::string("\"") + c + "\"";
    }
    char buffer[16];
    std::snprintf(buffer, sizeof buffer, "byte 0x%02X", static_cast<unsigned>(byte));
    return buffer;
}

void AppendUtf8(std::string& out, uint32_t code_point) {
    if (code_point < 0x80) {
        out += static_cast<char>(code_point);
    } else if (code_point < 0x800) {
        out += static_cast<char>(0xC0 | (code_point >> 6));
        out += static_cast<char>(0x80 | (code_point & 0x3F));
    } else if (code_point < 0x10000) {
        out += static_cast<char>(0xE0 | (code_point >> 12));
        out += static_cast<char>(0x80 | ((code_point >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code_point & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (code_point >> 18));
        out += static_cast<char>(0x80 | ((code_point >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((code_point >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code_point & 0x3F));
    }
}

class Parser {
public:
    Parser(std::string_view text, const Limits& limits, size_t start) : text_(text), limits_(limits), at_(start) {}

    ParseResult Run() {
        Value value;
        SkipSpace();
        if (AtEnd()) {
            Fail(Here(), "the file is empty, a JSON value was expected");
            return Failure();
        }
        if (!ParseValue(value, 0)) {
            return Failure();
        }
        SkipSpace();
        if (!AtEnd()) {
            UnexpectedHere("the end of the file");
            return Failure();
        }
        ParseResult result;
        result.value = std::move(value);
        return result;
    }

private:
    struct Open {
        Position start;
        const char* what;
    };

    ParseResult Failure() {
        ParseResult result;
        result.error = std::move(error_);
        return result;
    }

    bool AtEnd() const {
        return at_ >= text_.size();
    }
    char Peek() const {
        return text_[at_];
    }
    char PeekNext() const {
        return at_ + 1 < text_.size() ? text_[at_ + 1] : '\0';
    }
    Position Here() const {
        return Position{line_, column_};
    }

    void Advance() {
        if (text_[at_] == '\n') {
            ++line_;
            column_ = 1;
        } else {
            ++column_;
        }
        ++at_;
    }

    void SkipSpace() {
        while (!AtEnd()) {
            const char c = Peek();
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
                return;
            }
            Advance();
        }
    }

    bool Fail(Position where, std::string message, std::string fix = {}) {
        error_ = Error{where, std::move(message), std::move(fix)};
        return false;
    }

    // The byte at hand is not one of the things that may be here. Says what it is and what was expected.
    bool UnexpectedHere(const std::string& expected) {
        if (AtEnd()) {
            if (!open_.empty()) {
                return Fail(open_.back().start, std::string("this ") + open_.back().what + " is never closed",
                            std::string("add the closing ") + (open_.back().what[0] == 'a' ? "]" : "}") +
                                " at the end");
            }
            return Fail(Here(), "the file ends too early, " + expected + " was expected");
        }
        const char c = Peek();
        if (c == '/' && (PeekNext() == '/' || PeekNext() == '*')) {
            return Fail(Here(), "comments are not allowed in JSON",
                        "delete the comment; to keep a remark, put it in a \"note\" field");
        }
        if (c == '\'') {
            return Fail(Here(), "text is written with double quotes in JSON, not single quotes",
                        "replace the single quotes with double quotes, like \"text\"");
        }
        return Fail(Here(), "unexpected " + Describe(c) + ", " + expected + " was expected");
    }

    bool ParseValue(Value& out, uint32_t depth) {
        SkipSpace();
        if (AtEnd()) {
            return UnexpectedHere("a value");
        }
        const Position where = Here();
        const char c = Peek();
        switch (c) {
        case '{':
            return ParseObject(out, depth + 1);
        case '[':
            return ParseArray(out, depth + 1);
        case '"': {
            std::string text;
            if (!ParseString(text)) {
                return false;
            }
            out = Value::MakeString(where, std::move(text));
            return true;
        }
        case 't':
            return ParseWord(out, "true", Value::MakeBool(where, true));
        case 'f':
            return ParseWord(out, "false", Value::MakeBool(where, false));
        case 'n':
            return ParseWord(out, "null", Value::MakeNull(where));
        case 'N':
        case 'I':
            return Fail(where, "NaN and Infinity are not numbers in JSON", "use a finite number");
        case '+':
            return Fail(where, "a number cannot start with a plus sign", "remove the plus sign");
        case '.':
            return Fail(where, "a number needs a digit before the decimal point", "write 0.5 instead of .5");
        default:
            if (c == '-' || IsDigit(c)) {
                return ParseNumber(out);
            }
            return UnexpectedHere("a value");
        }
    }

    bool ParseWord(Value& out, std::string_view word, Value value) {
        if (text_.substr(at_, word.size()) != word) {
            return Fail(Here(), std::string("unexpected text, ") + std::string(word) + " was expected here");
        }
        for (size_t i = 0; i < word.size(); ++i) {
            Advance();
        }
        out = std::move(value);
        return true;
    }

    bool EnterContainer(uint32_t depth, const char* what) {
        if (depth > limits_.max_depth) {
            return Fail(Here(), "this nests deeper than " + std::to_string(limits_.max_depth) + " levels");
        }
        open_.push_back(Open{Here(), what});
        return true;
    }

    bool ParseArray(Value& out, uint32_t depth) {
        if (!EnterContainer(depth, "array")) {
            return false;
        }
        const Position start = Here();
        out = Value::MakeArray(start);
        Advance(); // [
        SkipSpace();
        if (!AtEnd() && Peek() == ']') {
            Advance();
            open_.pop_back();
            return true;
        }
        for (;;) {
            Value item;
            if (!ParseValue(item, depth)) {
                return false;
            }
            out.Append(std::move(item));
            SkipSpace();
            if (AtEnd()) {
                return UnexpectedHere("\",\" or \"]\"");
            }
            const char c = Peek();
            if (c == ']') {
                Advance();
                open_.pop_back();
                return true;
            }
            if (c != ',') {
                return NotCommaOr("\"]\"");
            }
            const Position comma = Here();
            Advance();
            SkipSpace();
            if (!AtEnd() && Peek() == ']') {
                return Fail(comma, "there is a comma after the last item", "remove the comma before the \"]\"");
            }
        }
    }

    bool NotCommaOr(const char* closing) {
        const std::string expected = std::string("\",\" or ") + closing;
        if (!AtEnd() && StartsAValue(Peek()) && Peek() != '/') {
            return Fail(Here(), "unexpected " + Describe(Peek()) + ", " + expected + " was expected",
                        "add a comma after the value before this one");
        }
        return UnexpectedHere(expected);
    }

    bool ParseObject(Value& out, uint32_t depth) {
        if (!EnterContainer(depth, "object")) {
            return false;
        }
        const Position start = Here();
        out = Value::MakeObject(start);
        std::unordered_map<std::string, Position> seen;
        Advance(); // {
        SkipSpace();
        if (!AtEnd() && Peek() == '}') {
            Advance();
            open_.pop_back();
            return true;
        }
        for (;;) {
            SkipSpace();
            if (AtEnd()) {
                return UnexpectedHere("a key");
            }
            const Position key_position = Here();
            if (Peek() != '"') {
                if (IsIdentifierStart(Peek())) {
                    std::string word;
                    for (size_t i = at_; i < text_.size() && IsIdentifierPart(text_[i]); ++i) {
                        word += text_[i];
                    }
                    return Fail(key_position, "a key must be in double quotes",
                                "write \"" + word + "\" with double quotes");
                }
                return UnexpectedHere("a key in double quotes");
            }
            std::string key;
            if (!ParseString(key)) {
                return false;
            }
            const auto [place, inserted] = seen.emplace(key, key_position);
            if (!inserted) {
                return Fail(key_position,
                            "the key \"" + key + "\" appears twice in this object, the first one is on line " +
                                std::to_string(place->second.line),
                            "remove one of the two");
            }
            SkipSpace();
            if (AtEnd() || Peek() != ':') {
                return UnexpectedHere("\":\" after the key");
            }
            Advance();
            Value value;
            if (!ParseValue(value, depth)) {
                return false;
            }
            out.Append(std::move(key), key_position, std::move(value));
            SkipSpace();
            if (AtEnd()) {
                return UnexpectedHere("\",\" or \"}\"");
            }
            const char c = Peek();
            if (c == '}') {
                Advance();
                open_.pop_back();
                return true;
            }
            if (c != ',') {
                return NotCommaOr("\"}\"");
            }
            const Position comma = Here();
            Advance();
            SkipSpace();
            if (!AtEnd() && Peek() == '}') {
                return Fail(comma, "there is a comma after the last member", "remove the comma before the \"}\"");
            }
        }
    }

    bool ParseHex4(uint32_t& code) {
        code = 0;
        for (int i = 0; i < 4; ++i) {
            if (AtEnd()) {
                return false;
            }
            const char c = Peek();
            uint32_t digit;
            if (c >= '0' && c <= '9') {
                digit = static_cast<uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                digit = static_cast<uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                digit = static_cast<uint32_t>(c - 'A' + 10);
            } else {
                return false;
            }
            code = code * 16 + digit;
            Advance();
        }
        return true;
    }

    bool ParseString(std::string& out) {
        const Position start = Here();
        Advance(); // opening quote
        for (;;) {
            if (AtEnd()) {
                return Fail(start, "this string is never closed", "add the closing double quote");
            }
            const char c = Peek();
            if (c == '"') {
                Advance();
                return true;
            }
            if (c == '\n' || c == '\r') {
                return Fail(start, "this string is never closed before the end of the line",
                            "add the closing double quote");
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                return Fail(Here(), "a string cannot hold the control character " + Describe(c) +
                                        ", write it as an escape such as \\u0009");
            }
            if (c == '\\') {
                if (!ParseEscape(out)) {
                    return false;
                }
            } else {
                out += c;
                Advance();
            }
            if (out.size() > limits_.max_string) {
                return Fail(start, "this string is longer than " + std::to_string(limits_.max_string) + " bytes");
            }
        }
    }

    bool ParseEscape(std::string& out) {
        const Position where = Here();
        Advance(); // backslash
        if (AtEnd()) {
            return Fail(where, "this string is never closed", "add the closing double quote");
        }
        const char c = Peek();
        switch (c) {
        case '"':
        case '\\':
        case '/':
            out += c;
            Advance();
            return true;
        case 'b':
            out += '\b';
            Advance();
            return true;
        case 'f':
            out += '\f';
            Advance();
            return true;
        case 'n':
            out += '\n';
            Advance();
            return true;
        case 'r':
            out += '\r';
            Advance();
            return true;
        case 't':
            out += '\t';
            Advance();
            return true;
        case 'u':
            Advance();
            return ParseUnicodeEscape(out, where);
        default:
            return Fail(where, "\\" + std::string(1, c) + " is not an escape that JSON knows");
        }
    }

    bool ParseUnicodeEscape(std::string& out, Position where) {
        uint32_t code = 0;
        if (!ParseHex4(code)) {
            return Fail(where, "\\u must be followed by four hexadecimal digits");
        }
        if (code >= 0xD800 && code <= 0xDBFF) {
            uint32_t low = 0;
            if (at_ + 1 < text_.size() && Peek() == '\\' && PeekNext() == 'u') {
                Advance();
                Advance();
                if (!ParseHex4(low)) {
                    return Fail(where, "\\u must be followed by four hexadecimal digits");
                }
            }
            if (low < 0xDC00 || low > 0xDFFF) {
                return Fail(where, "a high surrogate escape must be followed by a low surrogate escape");
            }
            AppendUtf8(out, 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00));
            return true;
        }
        if (code >= 0xDC00 && code <= 0xDFFF) {
            return Fail(where, "a low surrogate escape cannot stand alone");
        }
        AppendUtf8(out, code);
        return true;
    }

    bool ParseNumber(Value& out) {
        const Position where = Here();
        const size_t begin = at_;
        if (Peek() == '-') {
            Advance();
            if (AtEnd() || !IsDigit(Peek())) {
                return Fail(where, "a minus sign must be followed by digits",
                            AtEnd() || (Peek() != 'I' && Peek() != 'N') ? "write the number after the minus sign"
                                                                       : "use a finite number");
            }
        }
        if (Peek() == '0') {
            Advance();
            if (!AtEnd() && IsDigit(Peek())) {
                return Fail(where, "a number cannot have a leading zero", "remove the zeros before the first digit");
            }
            if (!AtEnd() && (Peek() == 'x' || Peek() == 'X')) {
                return Fail(where, "hexadecimal numbers are not allowed", "write the number in decimal");
            }
        } else {
            while (!AtEnd() && IsDigit(Peek())) {
                Advance();
            }
        }
        bool whole = true;
        if (!AtEnd() && Peek() == '.') {
            Advance();
            if (AtEnd() || !IsDigit(Peek())) {
                return Fail(where, "a decimal point must be followed by a digit",
                            "add a digit after the point, or remove the point");
            }
            while (!AtEnd() && IsDigit(Peek())) {
                Advance();
            }
            whole = false;
        }
        if (!AtEnd() && (Peek() == 'e' || Peek() == 'E')) {
            Advance();
            if (!AtEnd() && (Peek() == '+' || Peek() == '-')) {
                Advance();
            }
            if (AtEnd() || !IsDigit(Peek())) {
                return Fail(where, "an exponent must have digits", "add the digits of the exponent, or remove the e");
            }
            while (!AtEnd() && IsDigit(Peek())) {
                Advance();
            }
            whole = false;
        }
        const std::string_view token = text_.substr(begin, at_ - begin);
        if (whole) {
            return MakeInteger(out, where, token);
        }
        // strtod reads the decimal point of the current locale: give it the one it expects.
        std::string copy(token);
        const char* point = std::localeconv()->decimal_point;
        if (point != nullptr && point[0] != '\0' && std::string_view(point) != ".") {
            const size_t dot = copy.find('.');
            if (dot != std::string::npos) {
                copy.replace(dot, 1, point);
            }
        }
        errno = 0;
        const double value = std::strtod(copy.c_str(), nullptr);
        if (!std::isfinite(value)) {
            return Fail(where, "this number is too large for a 64-bit decimal number");
        }
        out = Value::MakeNumber(where, value);
        return true;
    }

    bool MakeInteger(Value& out, Position where, std::string_view token) {
        const bool negative = token[0] == '-';
        uint64_t magnitude = 0;
        const uint64_t limit = negative ? (uint64_t{1} << 63) : (uint64_t{1} << 63) - 1;
        for (const char c : token.substr(negative ? 1 : 0)) {
            const auto digit = static_cast<uint64_t>(c - '0');
            if (magnitude > (limit - digit) / 10) {
                return Fail(where, "this whole number does not fit in 64 bits", "use a smaller number");
            }
            magnitude = magnitude * 10 + digit;
        }
        out = Value::MakeInteger(where, negative ? static_cast<int64_t>(~magnitude + 1) : static_cast<int64_t>(magnitude));
        return true;
    }

    std::string_view text_;
    const Limits& limits_;
    size_t at_;
    uint32_t line_ = 1;
    uint32_t column_ = 1;
    std::vector<Open> open_;
    Error error_;
};

} // namespace

ParseResult Parse(std::string_view text, const Limits& limits) {
    if (text.size() > limits.max_bytes) {
        ParseResult result;
        result.error = Error{Position{1, 1}, "the file is larger than " + std::to_string(limits.max_bytes) +
                                                 " bytes, which is the most this tool reads",
                             {}};
        return result;
    }
    size_t start = 0;
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
        start = 3; // a byte order mark: editors on Windows write one
    }
    return Parser(text, limits, start).Run();
}

} // namespace tessera::schema::json
