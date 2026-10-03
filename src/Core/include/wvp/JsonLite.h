#pragma once
// Minimal JSON DOM + parser for the small payloads libffplay emits (events, media info).
// Strings are UTF-8. Missing / wrongly typed members yield "", 0 or false.
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wvp::json {

struct Value {
    enum class Kind { Null, Bool, Number, String, Array, Object };
    Kind kind = Kind::Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Value> arr;
    std::vector<std::string> keys;     // Object keys (parallel to vals)
    std::vector<Value> vals;

    bool IsObject() const { return kind == Kind::Object; }
    bool IsArray() const { return kind == Kind::Array; }
    const Value* Find(std::string_view key) const;

    std::string Str(std::string_view key) const;   // "" unless the member is a string
    double Dbl(std::string_view key) const;        // 0 unless the member is a number
    int Int(std::string_view key) const;           // 0 unless the member is a number (truncated)
    bool Bool(std::string_view key) const;         // false unless the member is literal true
    bool HasString(std::string_view key) const;
};

// nullopt on syntax error / trailing garbage.
std::optional<Value> Parse(std::string_view text);

}
