#pragma once
// Small pretty-printing JSON writer (UTF-8 output). The atomic file helpers it used to declare live in FileIo.h.
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "FileIo.h"   // fileio::AtomicWriteFile & co. (historically declared here)

namespace wvp {

class JsonWriter {
public:
    explicit JsonWriter(bool pretty = true) : pretty_(pretty) {}

    JsonWriter& BeginObject();
    JsonWriter& EndObject();
    JsonWriter& BeginArray();
    JsonWriter& EndArray();
    JsonWriter& Key(std::string_view name);                 // inside an object

    JsonWriter& String(std::string_view utf8);
    JsonWriter& String(std::wstring_view w);
    JsonWriter& Number(double v);                           // shortest round-trip form; non-finite -> null
    JsonWriter& Int(std::int64_t v);
    JsonWriter& Bool(bool v);
    JsonWriter& Null();

    JsonWriter& Prop(std::string_view k, const std::string& utf8) { Key(k); return String(std::string_view(utf8)); }
    JsonWriter& Prop(std::string_view k, const std::wstring& w) { Key(k); return String(std::wstring_view(w)); }
    JsonWriter& Prop(std::string_view k, const char* s) { Key(k); return String(std::string_view(s)); }
    JsonWriter& Prop(std::string_view k, double v) { Key(k); return Number(v); }
    JsonWriter& Prop(std::string_view k, int v) { Key(k); return Int(v); }
    JsonWriter& Prop(std::string_view k, std::int64_t v) { Key(k); return Int(v); }
    JsonWriter& Prop(std::string_view k, std::uint32_t v) { Key(k); return Int(v); }
    JsonWriter& Prop(std::string_view k, bool v) { Key(k); return Bool(v); }

    const std::string& Str() const { return out_; }

private:
    void BeforeValue();
    void Newline();
    struct Level { bool isObject; bool hasItems; };
    std::string out_;
    std::vector<Level> stack_;
    bool pretty_;
    bool afterKey_ = false;
};

}
