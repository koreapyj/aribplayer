#include "wvp/JsonLite.h"

#include <cmath>
#include <cstdlib>

namespace wvp::json {

const Value* Value::Find(std::string_view key) const {
    if (kind != Kind::Object) return nullptr;
    for (size_t i = 0; i < keys.size(); i++) if (keys[i] == key) return &vals[i];
    return nullptr;
}

std::string Value::Str(std::string_view key) const {
    auto* v = Find(key);
    return v && v->kind == Kind::String ? v->s : std::string();
}
bool Value::HasString(std::string_view key) const {
    auto* v = Find(key);
    return v && v->kind == Kind::String;
}
double Value::Dbl(std::string_view key) const {
    auto* v = Find(key);
    return v && v->kind == Kind::Number ? v->n : 0.0;
}
int Value::Int(std::string_view key) const {
    auto* v = Find(key);
    if (!v || v->kind != Kind::Number || !std::isfinite(v->n)) return 0;
    if (v->n > 2147483647.0) return 2147483647;
    if (v->n < -2147483648.0) return (-2147483647 - 1);
    return (int)v->n;
}
bool Value::Bool(std::string_view key) const {
    auto* v = Find(key);
    return v && v->kind == Kind::Bool && v->b;
}

namespace {

class Parser {
public:
    explicit Parser(std::string_view t) : t_(t) {}

    std::optional<Value> Run() {
        Value v;
        if (!ParseValue(v, 0)) return std::nullopt;
        SkipWs();
        if (p_ != t_.size()) return std::nullopt;
        return v;
    }

private:
    std::string_view t_;
    size_t p_ = 0;

    void SkipWs() { while (p_ < t_.size() && (t_[p_] == ' ' || t_[p_] == '\t' || t_[p_] == '\n' || t_[p_] == '\r')) p_++; }
    bool Lit(std::string_view w) {
        if (t_.compare(p_, w.size(), w) == 0) { p_ += w.size(); return true; }
        return false;
    }

    bool ParseValue(Value& v, int depth) {
        if (depth > 64) return false;
        SkipWs();
        if (p_ >= t_.size()) return false;
        char c = t_[p_];
        if (c == '{') return ParseObject(v, depth);
        if (c == '[') return ParseArray(v, depth);
        if (c == '"') { v.kind = Value::Kind::String; return ParseString(v.s); }
        if (Lit("true")) { v.kind = Value::Kind::Bool; v.b = true; return true; }
        if (Lit("false")) { v.kind = Value::Kind::Bool; v.b = false; return true; }
        if (Lit("null")) { v.kind = Value::Kind::Null; return true; }
        return ParseNumber(v);
    }

    bool ParseNumber(Value& v) {
        size_t st = p_;
        if (p_ < t_.size() && (t_[p_] == '-' || t_[p_] == '+')) p_++;
        while (p_ < t_.size() && ((t_[p_] >= '0' && t_[p_] <= '9') || t_[p_] == '.' || t_[p_] == 'e' || t_[p_] == 'E' || t_[p_] == '-' || t_[p_] == '+')) p_++;
        if (p_ == st) return false;
        std::string tmp(t_.substr(st, p_ - st));
        char* end = nullptr;
        double d = std::strtod(tmp.c_str(), &end);
        if (end == tmp.c_str() || *end != '\0') return false;
        v.kind = Value::Kind::Number;
        v.n = d;
        return true;
    }

    static void AppendUtf8(std::string& o, unsigned cp) {
        if (cp < 0x80) o.push_back((char)cp);
        else if (cp < 0x800) { o.push_back((char)(0xC0 | (cp >> 6))); o.push_back((char)(0x80 | (cp & 0x3F))); }
        else if (cp < 0x10000) { o.push_back((char)(0xE0 | (cp >> 12))); o.push_back((char)(0x80 | ((cp >> 6) & 0x3F))); o.push_back((char)(0x80 | (cp & 0x3F))); }
        else { o.push_back((char)(0xF0 | (cp >> 18))); o.push_back((char)(0x80 | ((cp >> 12) & 0x3F))); o.push_back((char)(0x80 | ((cp >> 6) & 0x3F))); o.push_back((char)(0x80 | (cp & 0x3F))); }
    }

    bool Hex4(unsigned& out) {
        if (p_ + 4 > t_.size()) return false;
        unsigned v = 0;
        for (int i = 0; i < 4; i++) {
            char c = t_[p_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
            else return false;
        }
        out = v;
        return true;
    }

    bool ParseString(std::string& out) {
        if (t_[p_] != '"') return false;
        p_++;
        out.clear();
        while (p_ < t_.size()) {
            char c = t_[p_++];
            if (c == '"') return true;
            if (c != '\\') { out.push_back(c); continue; }
            if (p_ >= t_.size()) return false;
            char e = t_[p_++];
            switch (e) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                unsigned cp;
                if (!Hex4(cp)) return false;
                if (cp >= 0xD800 && cp <= 0xDBFF && p_ + 1 < t_.size() && t_[p_] == '\\' && t_[p_ + 1] == 'u') {
                    size_t save = p_;
                    p_ += 2;
                    unsigned lo;
                    if (Hex4(lo) && lo >= 0xDC00 && lo <= 0xDFFF) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    else p_ = save;
                }
                if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
                AppendUtf8(out, cp);
                break;
            }
            default: return false;
            }
        }
        return false;
    }

    bool ParseArray(Value& v, int depth) {
        v.kind = Value::Kind::Array;
        p_++;
        SkipWs();
        if (p_ < t_.size() && t_[p_] == ']') { p_++; return true; }
        for (;;) {
            Value item;
            if (!ParseValue(item, depth + 1)) return false;
            v.arr.push_back(std::move(item));
            SkipWs();
            if (p_ >= t_.size()) return false;
            if (t_[p_] == ',') { p_++; continue; }
            if (t_[p_] == ']') { p_++; return true; }
            return false;
        }
    }

    bool ParseObject(Value& v, int depth) {
        v.kind = Value::Kind::Object;
        p_++;
        SkipWs();
        if (p_ < t_.size() && t_[p_] == '}') { p_++; return true; }
        for (;;) {
            SkipWs();
            if (p_ >= t_.size() || t_[p_] != '"') return false;
            std::string key;
            if (!ParseString(key)) return false;
            SkipWs();
            if (p_ >= t_.size() || t_[p_] != ':') return false;
            p_++;
            Value item;
            if (!ParseValue(item, depth + 1)) return false;
            v.keys.push_back(std::move(key));
            v.vals.push_back(std::move(item));
            SkipWs();
            if (p_ >= t_.size()) return false;
            if (t_[p_] == ',') { p_++; continue; }
            if (t_[p_] == '}') { p_++; return true; }
            return false;
        }
    }
};

}

std::optional<Value> Parse(std::string_view text) { return Parser(text).Run(); }

}
