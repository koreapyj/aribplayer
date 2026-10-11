#include "wvp/JsonWriter.h"

#include <charconv>
#include <cmath>
#include <cstdio>

namespace wvp {

void JsonWriter::Newline() {
    if (!pretty_) return;
    out_.push_back('\n');
    out_.append(stack_.size() * 2, ' ');
}

void JsonWriter::BeforeValue() {
    if (afterKey_) { afterKey_ = false; return; }
    if (!stack_.empty()) {
        if (stack_.back().hasItems) out_.push_back(',');
        stack_.back().hasItems = true;
        Newline();
    }
}

JsonWriter& JsonWriter::BeginObject() { BeforeValue(); out_.push_back('{'); stack_.push_back({ true, false }); return *this; }
JsonWriter& JsonWriter::BeginArray() { BeforeValue(); out_.push_back('['); stack_.push_back({ false, false }); return *this; }

JsonWriter& JsonWriter::EndObject() {
    bool had = !stack_.empty() && stack_.back().hasItems;
    if (!stack_.empty()) stack_.pop_back();
    if (had) Newline();
    out_.push_back('}');
    return *this;
}
JsonWriter& JsonWriter::EndArray() {
    bool had = !stack_.empty() && stack_.back().hasItems;
    if (!stack_.empty()) stack_.pop_back();
    if (had) Newline();
    out_.push_back(']');
    return *this;
}

namespace {
void EscapeTo(std::string& o, std::string_view s) {
    o.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\b': o += "\\b"; break;
        case '\f': o += "\\f"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", (unsigned)c); o += b; }
            else o.push_back((char)c);
        }
    }
    o.push_back('"');
}
}

JsonWriter& JsonWriter::Key(std::string_view name) {
    BeforeValue();
    EscapeTo(out_, name);
    out_ += pretty_ ? ": " : ":";
    afterKey_ = true;
    return *this;
}

JsonWriter& JsonWriter::String(std::string_view utf8) { BeforeValue(); EscapeTo(out_, utf8); return *this; }

JsonWriter& JsonWriter::Number(double v) {
    BeforeValue();
    if (!std::isfinite(v)) { out_ += "null"; return *this; }
    char buf[64];
    auto r = std::to_chars(buf, buf + sizeof buf, v);
    out_.append(buf, r.ptr);
    return *this;
}
JsonWriter& JsonWriter::Int(std::int64_t v) { BeforeValue(); out_ += std::to_string(v); return *this; }
JsonWriter& JsonWriter::Bool(bool v) { BeforeValue(); out_ += v ? "true" : "false"; return *this; }
JsonWriter& JsonWriter::Null() { BeforeValue(); out_ += "null"; return *this; }

}
