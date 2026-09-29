#ifndef MICROPIXEL_SDK_JSON_READER_HPP
#define MICROPIXEL_SDK_JSON_READER_HPP

#include <stdint.h>
#include <string_view>

namespace micropixel::json {

// Allocation-free JSON view used by bounded Guests. Values point directly at
// the response buffer; the caller must keep that buffer alive while using a
// Value. The reader validates the complete document, UTF-8, nesting and item
// counts before exposing it.
enum class Type : uint8_t { kNull, kBoolean, kNumber, kString, kArray, kObject };

struct Value final {
    Type type{Type::kNull};
    const char* data{};
    uint32_t size{};
};

struct Limits final {
    uint16_t max_depth{12U};
    uint16_t max_items_per_container{128U};
};

class Reader final {
   public:
    constexpr Reader(const char* bytes, uint32_t size, Limits limits = {})
        : begin_(bytes), end_(bytes == nullptr ? nullptr : bytes + size), limits_(limits) {}

    [[nodiscard]] bool Parse(Value& output) const {
        if (begin_ == nullptr || end_ == nullptr || begin_ == end_) return false;
        const char* cursor = begin_;
        SkipSpace(cursor);
        if (!ParseValue(cursor, 0U, output)) return false;
        SkipSpace(cursor);
        return cursor == end_;
    }

    [[nodiscard]] bool Find(const Value& object, std::string_view key, Value& output) const {
        if (!Contained(object) || object.type != Type::kObject || object.size < 2U) return false;
        const char* cursor = object.data + 1;
        const char* finish = object.data + object.size - 1;
        SkipSpace(cursor, finish);
        if (cursor == finish) return false;
        uint16_t count = 0U;
        while (cursor < finish && count++ < limits_.max_items_per_container) {
            Value name{};
            if (!ParseString(cursor, finish, name)) return false;
            SkipSpace(cursor, finish);
            if (cursor == finish || *cursor++ != ':') return false;
            SkipSpace(cursor, finish);
            Value candidate{};
            if (!ParseValue(cursor, 1U, candidate, finish)) return false;
            if (UnescapedStringEquals(name, key)) {
                output = candidate;
                return true;
            }
            SkipSpace(cursor, finish);
            if (cursor == finish) return false;
            if (*cursor++ == '}') return false;
            if (*(cursor - 1) != ',') return false;
            SkipSpace(cursor, finish);
        }
        return false;
    }

    [[nodiscard]] bool At(const Value& array, uint16_t index, Value& output) const {
        if (!Contained(array) || array.type != Type::kArray || array.size < 2U ||
            index >= limits_.max_items_per_container) {
            return false;
        }
        const char* cursor = array.data + 1;
        const char* finish = array.data + array.size - 1;
        SkipSpace(cursor, finish);
        uint16_t current = 0U;
        while (cursor < finish && current <= index) {
            Value candidate{};
            if (!ParseValue(cursor, 1U, candidate, finish)) return false;
            if (current++ == index) {
                output = candidate;
                return true;
            }
            SkipSpace(cursor, finish);
            if (cursor == finish || *cursor++ != ',') return false;
            SkipSpace(cursor, finish);
        }
        return false;
    }

    [[nodiscard]] bool Count(const Value& array, uint16_t& output) const {
        if (!Contained(array) || array.type != Type::kArray || array.size < 2U) return false;
        const char* cursor = array.data + 1;
        const char* finish = array.data + array.size - 1;
        SkipSpace(cursor, finish);
        output = 0U;
        if (cursor == finish) return true;
        while (cursor < finish && output < limits_.max_items_per_container) {
            Value ignored{};
            if (!ParseValue(cursor, 1U, ignored, finish)) return false;
            ++output;
            SkipSpace(cursor, finish);
            if (cursor == finish) return true;
            if (*cursor++ != ',') return false;
            SkipSpace(cursor, finish);
        }
        return false;
    }

    [[nodiscard]] static std::string_view String(const Value& value) {
        if (value.type != Type::kString || value.size < 2U) return {};
        return {value.data + 1, value.size - 2U};
    }

    [[nodiscard]] static bool Boolean(const Value& value, bool& output) {
        if (value.type != Type::kBoolean) return false;
        if (value.size == 4U && value.data[0] == 't') {
            output = true;
            return true;
        }
        if (value.size == 5U && value.data[0] == 'f') {
            output = false;
            return true;
        }
        return false;
    }

    [[nodiscard]] static bool Integer(const Value& value, int64_t& output) {
        if (value.type != Type::kNumber || value.size == 0U) return false;
        const char* cursor = value.data;
        const char* finish = value.data + value.size;
        const bool negative = *cursor == '-';
        if (negative && ++cursor == finish) return false;
        uint64_t magnitude = 0U;
        while (cursor != finish) {
            if (*cursor < '0' || *cursor > '9') return false;
            const uint8_t digit = static_cast<uint8_t>(*cursor++ - '0');
            if (magnitude > (UINT64_MAX - digit) / 10U) return false;
            magnitude = magnitude * 10U + digit;
        }
        constexpr uint64_t kNegativeLimit = static_cast<uint64_t>(INT64_MAX) + 1U;
        if ((!negative && magnitude > static_cast<uint64_t>(INT64_MAX)) || (negative && magnitude > kNegativeLimit)) {
            return false;
        }
        output = negative ? (magnitude == kNegativeLimit ? INT64_MIN : -static_cast<int64_t>(magnitude))
                          : static_cast<int64_t>(magnitude);
        return true;
    }

    // Parses a non-exponent JSON number into a signed fixed-point integer.
    // Excess fractional precision is rejected, which keeps prices exact.
    [[nodiscard]] static bool Fixed(const Value& value, uint8_t decimals, int64_t& output) {
        if (value.type != Type::kNumber || decimals > 6U || value.size == 0U) return false;
        const char* cursor = value.data;
        const char* finish = value.data + value.size;
        const bool negative = *cursor == '-';
        if (negative && ++cursor == finish) return false;
        uint64_t magnitude = 0U;
        uint8_t fractional = 0U;
        bool point = false;
        while (cursor != finish) {
            if (*cursor == '.' && !point) {
                point = true;
                ++cursor;
                continue;
            }
            if (*cursor < '0' || *cursor > '9' || (point && fractional == decimals)) return false;
            const uint8_t digit = static_cast<uint8_t>(*cursor++ - '0');
            if (magnitude > (static_cast<uint64_t>(INT64_MAX) - digit) / 10U) return false;
            magnitude = magnitude * 10U + digit;
            if (point) ++fractional;
        }
        while (fractional++ < decimals) {
            if (magnitude > static_cast<uint64_t>(INT64_MAX) / 10U) return false;
            magnitude *= 10U;
        }
        output = negative ? -static_cast<int64_t>(magnitude) : static_cast<int64_t>(magnitude);
        return true;
    }

   private:
    static void SkipSpace(const char*& cursor, const char* finish) {
        while (cursor < finish && (*cursor == ' ' || *cursor == '\n' || *cursor == '\r' || *cursor == '\t')) ++cursor;
    }
    void SkipSpace(const char*& cursor) const { SkipSpace(cursor, end_); }

    [[nodiscard]] bool Contained(const Value& value) const {
        return value.data != nullptr && value.data >= begin_ && value.data <= end_ &&
               value.size <= static_cast<uint32_t>(end_ - value.data);
    }

    [[nodiscard]] static bool Match(const char*& cursor, const char* finish, const char* literal, uint32_t length) {
        if (static_cast<uint32_t>(finish - cursor) < length) return false;
        for (uint32_t index = 0U; index < length; ++index) {
            if (cursor[index] != literal[index]) return false;
        }
        cursor += length;
        return true;
    }

    [[nodiscard]] static bool Utf8(const char*& cursor, const char* finish) {
        const uint8_t first = static_cast<uint8_t>(*cursor++);
        if (first < 0x80U) return true;
        uint8_t count = 0U;
        uint32_t codepoint = 0U;
        uint32_t minimum = 0U;
        if ((first & 0xe0U) == 0xc0U) { count = 1U; codepoint = first & 0x1fU; minimum = 0x80U; }
        else if ((first & 0xf0U) == 0xe0U) { count = 2U; codepoint = first & 0x0fU; minimum = 0x800U; }
        else if ((first & 0xf8U) == 0xf0U) { count = 3U; codepoint = first & 0x07U; minimum = 0x10000U; }
        else return false;
        if (finish - cursor < count) return false;
        for (uint8_t index = 0U; index < count; ++index) {
            const uint8_t next = static_cast<uint8_t>(*cursor++);
            if ((next & 0xc0U) != 0x80U) return false;
            codepoint = (codepoint << 6U) | (next & 0x3fU);
        }
        return codepoint >= minimum && codepoint <= 0x10ffffU && !(codepoint >= 0xd800U && codepoint <= 0xdfffU);
    }

    [[nodiscard]] static bool Hex4(const char*& cursor, const char* finish) {
        if (finish - cursor < 4) return false;
        for (uint8_t index = 0U; index < 4U; ++index) {
            const char value = *cursor++;
            if (!((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') ||
                  (value >= 'A' && value <= 'F'))) return false;
        }
        return true;
    }

    [[nodiscard]] static bool ParseString(const char*& cursor, const char* finish, Value& output) {
        if (cursor == finish || *cursor != '"') return false;
        const char* start = cursor++;
        while (cursor < finish) {
            const uint8_t byte = static_cast<uint8_t>(*cursor);
            if (byte == '"') {
                ++cursor;
                output = {Type::kString, start, static_cast<uint32_t>(cursor - start)};
                return true;
            }
            if (byte < 0x20U) return false;
            if (byte == '\\') {
                ++cursor;
                if (cursor == finish) return false;
                const char escape = *cursor++;
                if (escape == 'u') {
                    if (!Hex4(cursor, finish)) return false;
                } else if (!(escape == '"' || escape == '\\' || escape == '/' || escape == 'b' || escape == 'f' ||
                             escape == 'n' || escape == 'r' || escape == 't')) return false;
            } else if (!Utf8(cursor, finish)) return false;
        }
        return false;
    }

    [[nodiscard]] static bool UnescapedStringEquals(const Value& value, std::string_view expected) {
        const auto raw = String(value);
        if (raw.size() != expected.size()) return false;
        for (size_t index = 0U; index < raw.size(); ++index) {
            if (raw[index] == '\\' || raw[index] != expected[index]) return false;
        }
        return true;
    }

    [[nodiscard]] static bool ParseNumber(const char*& cursor, const char* finish, Value& output) {
        const char* start = cursor;
        if (cursor < finish && *cursor == '-') ++cursor;
        if (cursor == finish) return false;
        if (*cursor == '0') ++cursor;
        else {
            if (*cursor < '1' || *cursor > '9') return false;
            while (cursor < finish && *cursor >= '0' && *cursor <= '9') ++cursor;
        }
        if (cursor < finish && *cursor == '.') {
            ++cursor;
            const char* fraction = cursor;
            while (cursor < finish && *cursor >= '0' && *cursor <= '9') ++cursor;
            if (cursor == fraction) return false;
        }
        if (cursor < finish && (*cursor == 'e' || *cursor == 'E')) {
            ++cursor;
            if (cursor < finish && (*cursor == '+' || *cursor == '-')) ++cursor;
            const char* exponent = cursor;
            while (cursor < finish && *cursor >= '0' && *cursor <= '9') ++cursor;
            if (cursor == exponent) return false;
        }
        output = {Type::kNumber, start, static_cast<uint32_t>(cursor - start)};
        return true;
    }

    [[nodiscard]] bool ParseContainer(const char*& cursor, uint16_t depth, Value& output, const char* finish,
                                      bool object) const {
        if (depth >= limits_.max_depth) return false;
        const char* start = cursor++;
        const char closing = object ? '}' : ']';
        SkipSpace(cursor, finish);
        if (cursor < finish && *cursor == closing) {
            ++cursor;
            output = {object ? Type::kObject : Type::kArray, start, static_cast<uint32_t>(cursor - start)};
            return true;
        }
        uint16_t count = 0U;
        while (cursor < finish && count++ < limits_.max_items_per_container) {
            if (object) {
                Value key{};
                if (!ParseString(cursor, finish, key)) return false;
                SkipSpace(cursor, finish);
                if (cursor == finish || *cursor++ != ':') return false;
                SkipSpace(cursor, finish);
            }
            Value ignored{};
            if (!ParseValue(cursor, static_cast<uint16_t>(depth + 1U), ignored, finish)) return false;
            SkipSpace(cursor, finish);
            if (cursor == finish) return false;
            if (*cursor == closing) {
                ++cursor;
                output = {object ? Type::kObject : Type::kArray, start, static_cast<uint32_t>(cursor - start)};
                return true;
            }
            if (*cursor++ != ',') return false;
            SkipSpace(cursor, finish);
        }
        return false;
    }

    [[nodiscard]] bool ParseValue(const char*& cursor, uint16_t depth, Value& output,
                                  const char* finish = nullptr) const {
        if (finish == nullptr) finish = end_;
        if (cursor == finish) return false;
        if (*cursor == '"') return ParseString(cursor, finish, output);
        if (*cursor == '{') return ParseContainer(cursor, depth, output, finish, true);
        if (*cursor == '[') return ParseContainer(cursor, depth, output, finish, false);
        const char* start = cursor;
        if (Match(cursor, finish, "null", 4U)) { output = {Type::kNull, start, 4U}; return true; }
        if (Match(cursor, finish, "true", 4U)) { output = {Type::kBoolean, start, 4U}; return true; }
        if (Match(cursor, finish, "false", 5U)) { output = {Type::kBoolean, start, 5U}; return true; }
        return ParseNumber(cursor, finish, output);
    }

    const char* begin_{};
    const char* end_{};
    Limits limits_{};
};

}  // namespace micropixel::json

#endif
