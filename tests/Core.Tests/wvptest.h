#pragma once
// Tiny dependency-free test framework.
//   TEST_CASE(name) { ... }            register a test (name is an identifier; integration tests: TEST_CASE_INTEGRATION)
//   CHECK(cond) / CHECK_EQ(a, b)       record failure, continue
//   REQUIRE(cond)                      record failure and abort the current test
#include <cstdio>
#include <functional>
#include <sstream>
#include <type_traits>
#include <string>
#include <vector>

namespace wvptest {

struct TestCase {
    const char* name;
    void (*fn)();
    bool integration;
};

inline std::vector<TestCase>& Registry() { static std::vector<TestCase> r; return r; }

struct Registrar {
    Registrar(const char* n, void (*f)(), bool integration) { Registry().push_back({ n, f, integration }); }
};

struct RequireFailure {};
struct SkipFailure { std::string reason; };

struct State {
    int failures = 0;      // in current test
    int checks = 0;
};
inline State& Cur() { static State s; return s; }

inline void Fail(const char* file, int line, const std::string& msg) {
    Cur().failures++;
    std::printf("    FAIL %s:%d: %s\n", file, line, msg.c_str());
}

template <class T> std::string Str(const T& v) {
    if constexpr (std::is_enum_v<T>) { return std::to_string((long long)v); }
    else { std::ostringstream o; o << v; return o.str(); }
}
inline std::string Str(const std::wstring& w) {
    std::string s; for (wchar_t c : w) s.push_back(c < 128 ? (char)c : '?'); return "L\"" + s + "\"";
}
inline std::string Str(const wchar_t* w) { return Str(std::wstring(w)); }
inline std::string Str(const std::string& s) { return "\"" + s + "\""; }
inline std::string Str(const char* s) { return Str(std::string(s)); }
inline std::string Str(bool b) { return b ? "true" : "false"; }

template <class A, class B>
bool EqImpl(const char* file, int line, const char* ea, const char* eb, const A& a, const B& b) {
    Cur().checks++;
    if (a == b) return true;
    Fail(file, line, std::string(ea) + " == " + eb + "  (" + Str(a) + " vs " + Str(b) + ")");
    return false;
}

}

#define WVP_CAT2(a, b) a##b
#define WVP_CAT(a, b) WVP_CAT2(a, b)
#define TEST_CASE(name) \
    static void name(); \
    static ::wvptest::Registrar WVP_CAT(reg_, name)(#name, &name, false); \
    static void name()
#define TEST_CASE_INTEGRATION(name) \
    static void name(); \
    static ::wvptest::Registrar WVP_CAT(reg_, name)(#name, &name, true); \
    static void name()

#define CHECK(cond) do { ::wvptest::Cur().checks++; if (!(cond)) ::wvptest::Fail(__FILE__, __LINE__, "CHECK(" #cond ")"); } while (0)
#define CHECK_MSG(cond, msg) do { ::wvptest::Cur().checks++; if (!(cond)) ::wvptest::Fail(__FILE__, __LINE__, std::string("CHECK(" #cond ") ") + (msg)); } while (0)
#define CHECK_EQ(a, b) do { ::wvptest::EqImpl(__FILE__, __LINE__, #a, #b, (a), (b)); } while (0)
#define REQUIRE(cond) do { ::wvptest::Cur().checks++; if (!(cond)) { ::wvptest::Fail(__FILE__, __LINE__, "REQUIRE(" #cond ")"); throw ::wvptest::RequireFailure{}; } } while (0)
#define REQUIRE_MSG(cond, msg) do { ::wvptest::Cur().checks++; if (!(cond)) { ::wvptest::Fail(__FILE__, __LINE__, std::string("REQUIRE(" #cond ") ") + (msg)); throw ::wvptest::RequireFailure{}; } } while (0)
