#include "wvp/JsonWriter.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <iterator>

#include "wvp/Utf.h"

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
JsonWriter& JsonWriter::String(std::wstring_view w) { return String(std::string_view(ToUtf8(w))); }

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

namespace fileio {

bool ReadAllBytes(const std::wstring& path, std::string& out) {
    out.clear();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    bool ok = GetFileSizeEx(h, &sz) && sz.QuadPart >= 0 && sz.QuadPart < (1LL << 30);
    if (ok) {
        out.resize((size_t)sz.QuadPart);
        size_t done = 0;
        while (ok && done < out.size()) {
            DWORD got = 0;
            ok = ReadFile(h, out.data() + done, (DWORD)std::min<size_t>(out.size() - done, 1u << 24), &got, nullptr) && got != 0;
            done += got;
        }
        out.resize(done);
    }
    CloseHandle(h);
    return ok;
}

bool EnsureDirectory(const std::wstring& dir) {
    if (dir.empty()) return false;
    DWORD a = GetFileAttributesW(dir.c_str());
    if (a != INVALID_FILE_ATTRIBUTES) return (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
    size_t cut = dir.find_last_of(L"\\/");
    if (cut != std::wstring::npos && cut > 0 && !(cut == 2 && dir[1] == L':')) {
        if (!EnsureDirectory(dir.substr(0, cut))) return false;
    }
    if (CreateDirectoryW(dir.c_str(), nullptr)) return true;
    a = GetFileAttributesW(dir.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool AtomicWriteFile(const std::wstring& path, std::string_view bytes, bool durable) {
    size_t cut = path.find_last_of(L"\\/");
    if (cut != std::wstring::npos && cut > 0) { if (!EnsureDirectory(path.substr(0, cut))) return false; }
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    size_t done = 0;
    while (ok && done < bytes.size()) {
        DWORD put = 0;
        ok = WriteFile(h, bytes.data() + done, (DWORD)std::min<size_t>(bytes.size() - done, 1u << 24), &put, nullptr) && put != 0;
        done += put;
    }
    if (ok && durable) ok = FlushFileBuffers(h) != 0;
    CloseHandle(h);
    if (ok) ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | (durable ? MOVEFILE_WRITE_THROUGH : 0)) != 0;
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}

std::wstring LocalAppDataApp() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, (DWORD)std::size(buf));
    std::wstring base;
    if (n > 0 && n < std::size(buf)) base.assign(buf, n);
    else {
        DWORD t = GetTempPathW((DWORD)std::size(buf), buf);
        if (t > 0 && t < std::size(buf)) base.assign(buf, t);
    }
    while (!base.empty() && (base.back() == L'\\' || base.back() == L'/')) base.pop_back();
    return base + L"\\aribplayer";
}

}
}
