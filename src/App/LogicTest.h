#pragma once
// Headless self-test registry for `aribplayer.exe --logictest`. Usable from ANY .cpp (pages/, features/).
// Cases must not create visible windows, touch the clipboard or real %LOCALAPPDATA% (inject temp dirs).
// The runner prints PASS/FAIL per case plus a summary and exits 0 only when all pass. Optional filter: --logictest <substring>.
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace app::lt {

struct Case { const char* name; void (*fn)(); };
inline std::vector<Case>& Registry() { static std::vector<Case> r; return r; }
struct Registrar { Registrar(const char* n, void (*f)()) { Registry().push_back({n, f}); } };
struct State { int failures = 0; std::string firstFailure; };
inline State& Cur() { static State s; return s; }
inline void Fail(const char* file, int line, const std::string& msg) {
    Cur().failures++;
    std::string m = std::string(file) + ":" + std::to_string(line) + ": " + msg;
    if (Cur().firstFailure.empty()) Cur().firstFailure = m;
    std::printf("    FAIL %s\n", m.c_str());
}
template <class T> std::string Str(const T& v) { std::ostringstream o; o << v; return o.str(); }
inline std::string Str(const std::wstring& w) { std::string s; for (wchar_t c : w) s.push_back(c < 128 ? (char)c : '?'); return "L\"" + s + "\""; }
inline std::string Str(const wchar_t* w) { return Str(std::wstring(w)); }
inline std::string Str(bool b) { return b ? "true" : "false"; }
/// Runs the registry; returns the process exit code (0 = all pass).
int RunAll(const std::string& filter);

}

#define LT_CAT2(a, b) a##b
#define LT_CAT(a, b) LT_CAT2(a, b)
#define LOGIC_TEST(name) \
    static void name(); \
    static ::app::lt::Registrar LT_CAT(ltreg_, name)(#name, &name); \
    static void name()
#define LT_CHECK(cond) do { if (!(cond)) ::app::lt::Fail(__FILE__, __LINE__, "CHECK(" #cond ")"); } while (0)
#define LT_CHECK_EQ(a, b) do { auto&& va_ = (a); auto&& vb_ = (b); if (!(va_ == vb_)) ::app::lt::Fail(__FILE__, __LINE__, std::string(#a " == " #b " (") + ::app::lt::Str(va_) + " vs " + ::app::lt::Str(vb_) + ")"); } while (0)
