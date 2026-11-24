// A small JSON reader and writer: enough for events, replies and the settings files. It never
// throws; bad input gives std::nullopt. No framework headers, so the core tests run anywhere.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bada::json {

struct Value {
    enum class Type { kNull, kBool, kNumber, kString, kArray, kObject } type = Type::kNull;
    bool boolean = false;
    double number = 0;
    std::string string;  // UTF-8
    std::vector<Value> array;
    std::map<std::string, Value> object;

    // The member `key` of an object, or nullptr.
    const Value* Get(const std::string& key) const;
    const std::string* String() const { return type == Type::kString ? &string : nullptr; }
    bool IsObject() const { return type == Type::kObject; }
    bool IsArray() const { return type == Type::kArray; }
    bool IsString() const { return type == Type::kString; }
    bool IsNumber() const { return type == Type::kNumber; }
    bool IsBool() const { return type == Type::kBool; }
    bool IsNull() const { return type == Type::kNull; }

    // Member helpers with a fallback for missing or mistyped members.
    std::string GetString(const std::string& key, std::string fallback = {}) const;
    bool GetBool(const std::string& key, bool fallback = false) const;
    double GetNumber(const std::string& key, double fallback = 0) const;

    // Sets a member, making this an object if it isn't one. Returns *this, for chaining.
    Value& Set(const std::string& key, Value value) {
        type = Type::kObject;
        object[key] = std::move(value);
        return *this;
    }
    // Appends an item, making this an array if it isn't one.
    Value& Push(Value value) {
        type = Type::kArray;
        array.push_back(std::move(value));
        return *this;
    }
};

std::optional<Value> Parse(std::string_view text);

// Serializes `value` compactly (object keys in sorted order).
std::string Write(const Value& value);
// `text` as a JSON string literal, quotes included.
std::string Quote(std::string_view text);

Value Str(std::string_view text);
Value Bool(bool b);
Value Num(double d);
Value Null();
Value Array();
Value Object();

// Deep equality.
bool Equal(const Value& a, const Value& b);

}  // namespace bada::json
