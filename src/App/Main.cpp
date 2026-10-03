// aribplayer.exe [options] [inputs...]      (run with --help for the full text, see features/Logging.cpp UsageText)
#include <windows.h>
#include <ole2.h>
#include <shellapi.h>

#include <cstdio>
#include <cwctype>

#include "App.h"
#include "LogicTest.h"
#include "features/CrashDump.h"
#include "features/Logging.h"
#include "features/SingleInstance.h"
#include "wvp/FfPlayer.h"
#include "wvp/TestGuard.h"

namespace {
std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    app::crash::Install();
    struct ReturnGuard { ~ReturnGuard() { app::crash::MarkCleanExit(); } } returnGuard;   // runs on every return, never on exit()
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    app::AppOptions o;
    app::log::Settings ls;
    bool logic = false, help = false;
    std::wstring badArg;
    std::string logicFilter;
    std::wstring probeOut, probeLuid;
    std::vector<std::wstring> args;
    auto lower = [](std::wstring s) { for (auto& ch : s) ch = (wchar_t)towlower(ch); return s; };
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        args.push_back(a);
        std::wstring low = lower(a);
        if (low == L"--logictest") {
            logic = true;
            if (i + 1 < argc && std::wstring(argv[i + 1]).rfind(L"--", 0) != 0) logicFilter = Narrow(argv[++i]);
        } else if (low == L"--probe-gpu" && i + 1 < argc) {
            probeOut = argv[++i];
        } else if (low == L"--headless-check") {
            o.headless = true;
        } else if (low.rfind(L"--result=", 0) == 0) {
            o.resultPath = a.substr(9);
        } else if (low == L"--help" || low == L"-h" || low == L"/?" || low == L"-?") {
            help = true;
        } else if (low == L"--console") {
            ls.console = true;
        } else if (low == L"--planner=off") {
            o.plannerOff = true;
        } else if (low == L"--d3d11-debug") {
            o.d3d11Debug = true;
        } else if (low.rfind(L"--d3d11-force-caps=", 0) == 0) {
            o.d3d11ForceCaps = a.substr(19);
        } else if (low.rfind(L"--d3d11-route=", 0) == 0) {
            o.d3d11Route = low.substr(14);
            if (o.d3d11Route != L"vp" && o.d3d11Route != L"shader") badArg = L"invalid --d3d11-route value: " + a.substr(14);
        } else if (low.rfind(L"--test-ui-device-loss=", 0) == 0) {
            o.testUiLossMs = _wtoi(a.c_str() + 22);
        } else if (low == L"--no-cuda") {
            o.noCuda = true;
        } else if (low.rfind(L"--adapter-luid=", 0) == 0) {
            probeLuid = a.substr(15);
        } else if (low == L"--stats") {
            ls.stats = true;
            o.stats = true;
        } else if (low.rfind(L"--log-level=", 0) == 0 || (low == L"--log-level" && i + 1 < argc)) {
            std::wstring v = low == L"--log-level" ? std::wstring(argv[++i]) : a.substr(12);
            auto lv = app::log::ParseLevel(v);
            if (!lv) { badArg = L"invalid --log-level value: " + v; } else ls.level = lv;
        } else if (low.rfind(L"--log-file=", 0) == 0) {
            ls.file = a.substr(11);
        } else if (low == L"--log-file" && i + 1 < argc) {
            ls.file = argv[++i];
        } else if (a.rfind(L"--", 0) == 0) {
        } else {
            o.inputs.push_back(a);
        }
    }
    if (argv) LocalFree(argv);
    if (logic || o.headless) wvp::SilenceFailureDialogs();   // test modes never show CRT / RTC / error dialogs

    // Internal child mode of the GPU probe (App::Create): the drivers keep tens of MB of per-process state after the
    // probes, so they run here and the app only reads the answers. The answer is one JSON line: the capabilities.
    if (!probeOut.empty()) {
        wvp::FfPlayer::SetProbeOptions(Narrow(app::ProbeOptions(probeLuid, o)));
        auto oneLine = [](std::string j) { for (char& c : j) if (c == '\r' || c == '\n') c = ' '; return j; };
        const std::string text = oneLine(wvp::FfPlayer::ProbeCapsJson()) + "\n";
        FILE* f = nullptr;
        if (_wfopen_s(&f, probeOut.c_str(), L"wb") != 0 || !f) return 3;
        const bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
        fclose(f);
        return ok ? 0 : 3;
    }

    app::log::Configure(ls);
    if (help || !badArg.empty()) {
        std::wstring text = (badArg.empty() ? L"" : badArg + L"\r\n\r\n") + app::log::UsageText();
        app::log::PrintToConsole(text);
        return badArg.empty() ? 0 : 2;
    }
    // The app is a GUI-subsystem exe: attach to the parent console so --logictest / --headless-check / --console output is
    // visible to callers (streams redirected to a file or pipe are left alone).
    if (ls.console || logic || o.headless) app::log::OpenConsole();

    if (logic) return app::lt::RunAll(logicFilter);

    if (o.resultPath.empty()) {
        wchar_t p[MAX_PATH * 2];
        DWORD n = GetEnvironmentVariableW(L"WVP_HEADLESS_RESULT", p, ARRAYSIZE(p));
        if (n > 0 && n < ARRAYSIZE(p)) o.resultPath = p;
    }
    if (!o.headless && app::ForwardToExistingInstance(args)) return 0;
    // OLE drag/drop is apartment-threaded. wWinMain is the UI thread, and no earlier COM init changes its mode.
    if (FAILED(OleInitialize(nullptr))) {
        app::log::PrintToConsole(L"Could not initialize OLE drag and drop.");
        return 2;
    }
    int rc = 0;
    {
        app::App application(o);
        rc = o.headless ? application.RunHeadlessCheck() : application.RunGui();
    }
    OleUninitialize();
    return rc;
}
