#include "features/SingleInstance.h"

#include "AppContext.h"
#include "LogicTest.h"
#include "features/OpenFlows.h"
#include "wvp/MediaUri.h"
#include "wvp/Platform.h"
#include "wvp/win32/Utf.h"

namespace app {

namespace {
constexpr const wchar_t* kMutexName = L"Local\\aribplayer.SingleInstance";
constexpr const wchar_t* kMainClass = L"aribplayer.Main";   // App.cpp window class
HANDLE g_mutex = nullptr;

bool ClaimMutex() {   // true = we are the primary (first) instance
    if (g_mutex) return true;
    g_mutex = CreateMutexW(nullptr, FALSE, kMutexName);
    if (!g_mutex) return true;   // cannot tell: behave as primary
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(g_mutex);
        g_mutex = nullptr;
        return false;
    }
    return true;
}

std::wstring CurrentDirectory() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetCurrentDirectoryW(ARRAYSIZE(buf), buf);
    return (n > 0 && n < ARRAYSIZE(buf)) ? std::wstring(buf, n) : std::wstring();
}

bool IsUrlLike(const std::wstring& s) {
    return s.find(L"://") != std::wstring::npos || s.rfind(L"file:", 0) == 0;
}

void Activate(HWND hwnd) {
    if (!hwnd) return;
    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    else if (!IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
}
}

std::wstring EncodeCommandLine(const std::wstring& cwd, const std::vector<std::wstring>& args) {
    std::wstring s = cwd;
    s.push_back(L'\0');
    for (const auto& a : args) {
        s += a;
        s.push_back(L'\0');
    }
    return s;
}

bool DecodeCommandLine(const void* data, size_t byteCount, ForwardedCommandLine& out) {
    out = {};
    if (!data || byteCount == 0 || (byteCount % sizeof(wchar_t)) != 0) return false;
    const wchar_t* p = static_cast<const wchar_t*>(data);
    size_t n = byteCount / sizeof(wchar_t);
    std::vector<std::wstring> fields;
    size_t start = 0;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == L'\0') {
            fields.emplace_back(p + start, i - start);
            start = i + 1;
        }
    }
    if (start < n) fields.emplace_back(p + start, n - start);   // unterminated tail
    if (fields.empty()) return false;
    out.cwd = fields[0];
    out.args.assign(fields.begin() + 1, fields.end());
    return true;
}

std::vector<std::wstring> ResolveArgs(const std::wstring& cwd, const std::vector<std::wstring>& args) {
    std::vector<std::wstring> out;
    for (auto a : args) {
        if (a.empty()) continue;
        if (a.size() >= 2 && a.front() == L'"' && a.back() == L'"') a = a.substr(1, a.size() - 2);
        if (a.empty()) continue;
        if (IsUrlLike(a) || cwd.empty()) {
            out.push_back(a);
            continue;
        }
        out.push_back(wvp::ToWide(wvp::platform::Combine(wvp::ToUtf8(cwd), wvp::ToUtf8(a))));
    }
    return out;
}

bool ForwardToExistingInstance(const std::vector<std::wstring>& args) {
    if (ClaimMutex()) return false;   // we are the first instance (mutex now held for the process lifetime)
    // Another instance owns the mutex. It may still be creating its window: wait briefly.
    HWND target = nullptr;
    for (int i = 0; i < 50 && !target; i++) {
        target = FindWindowW(kMainClass, nullptr);
        if (!target) Sleep(100);
    }
    if (!target) return false;   // stale mutex without a window: start normally

    std::vector<std::wstring> inputs;
    for (const auto& a : args)
        if (a.rfind(L"--", 0) != 0) inputs.push_back(a);
    std::wstring payload = EncodeCommandLine(CurrentDirectory(), inputs);

    DWORD pid = 0;
    GetWindowThreadProcessId(target, &pid);
    if (pid) AllowSetForegroundWindow(pid);

    COPYDATASTRUCT cds{};
    cds.dwData = kCopyDataMagic;
    cds.cbData = (DWORD)(payload.size() * sizeof(wchar_t));
    cds.lpData = payload.data();
    DWORD_PTR result = 0;
    LRESULT ok = SendMessageTimeoutW(target, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds), SMTO_ABORTIFHUNG | SMTO_NORMAL, 8000,
                                     &result);
    return ok != 0;
}

void RegisterPrimaryInstance(HWND) {
    // Headless checks must never become the instance that real launches forward to.
    if (wcsstr(GetCommandLineW(), L"--headless-check")) return;
    ClaimMutex();
}

bool HandleCopyData(AppContext& ctx, const COPYDATASTRUCT* cds) {
    if (!cds || cds->dwData != kCopyDataMagic) return false;
    ForwardedCommandLine cl;
    if (!DecodeCommandLine(cds->lpData, cds->cbData, cl)) return true;   // ours, but malformed: ignore
    auto paths = ResolveArgs(cl.cwd, cl.args);
    Activate(ctx.MainHwnd());
    if (!paths.empty()) OpenPaths(ctx, paths);
    return true;
}

LOGIC_TEST(single_instance_payload_roundtrip) {
    std::vector<std::wstring> args = {L"a.mp4", L"C:\\x y\\b.mkv", L"https://h/p?q=1"};
    auto s = EncodeCommandLine(L"D:\\work", args);
    LT_CHECK_EQ(s.size(), std::wstring(L"D:\\work").size() + 1 + 5 + 1 + 12 + 1 + 15 + 1);
    ForwardedCommandLine cl;
    LT_CHECK(DecodeCommandLine(s.data(), s.size() * sizeof(wchar_t), cl));
    LT_CHECK(cl.cwd == L"D:\\work");
    LT_CHECK(cl.args == args);
    auto e = EncodeCommandLine(L"D:\\work", {});
    LT_CHECK(DecodeCommandLine(e.data(), e.size() * sizeof(wchar_t), cl));
    LT_CHECK(cl.args.empty());
    LT_CHECK(cl.cwd == L"D:\\work");
    auto n = EncodeCommandLine(L"", {L"x"});
    LT_CHECK(DecodeCommandLine(n.data(), n.size() * sizeof(wchar_t), cl));
    LT_CHECK(cl.cwd.empty());
    LT_CHECK_EQ(cl.args.size(), (size_t)1);
}

LOGIC_TEST(single_instance_decode_rejects_malformed) {
    ForwardedCommandLine cl;
    LT_CHECK(!DecodeCommandLine(nullptr, 10, cl));
    wchar_t buf[4] = {L'a', 0, L'b', 0};
    LT_CHECK(!DecodeCommandLine(buf, 0, cl));
    LT_CHECK(!DecodeCommandLine(buf, 3, cl));   // odd byte count
    wchar_t tail[3] = {L'c', 0, L'x'};          // unterminated last field is still accepted
    LT_CHECK(DecodeCommandLine(tail, sizeof(tail), cl));
    LT_CHECK(cl.cwd == L"c");
    LT_CHECK_EQ(cl.args.size(), (size_t)1);
}

LOGIC_TEST(single_instance_resolve_args) {
    auto r = ResolveArgs(L"D:\\media", {L"a.mp4", L"sub\\b.mkv", L"E:\\abs\\c.mp4", L"https://h/x.m3u8", L"rtsp://cam/1", L"", L"\"q uoted.mp4\""});
    LT_CHECK_EQ(r.size(), (size_t)6);
    if (r.size() == 6) {
        LT_CHECK(r[0] == L"D:\\media\\a.mp4");
        LT_CHECK(r[1] == L"D:\\media\\sub\\b.mkv");
        LT_CHECK(r[2] == L"E:\\abs\\c.mp4");
        LT_CHECK(r[3] == L"https://h/x.m3u8");
        LT_CHECK(r[4] == L"rtsp://cam/1");
        LT_CHECK(r[5] == L"D:\\media\\q uoted.mp4");
    }
    auto none = ResolveArgs(L"", {L"rel.mp4"});
    LT_CHECK_EQ(none.size(), (size_t)1);
    LT_CHECK(ResolveArgs(L"D:\\m", {}).empty());
}

}
