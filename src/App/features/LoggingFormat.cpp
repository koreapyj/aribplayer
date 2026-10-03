#include "features/LoggingFormat.h"

#include <algorithm>
#include <cwctype>
#include <set>

namespace app::log {

namespace {
const std::set<std::wstring>& ValueKeys() {
    static const std::set<std::wstring> s = {L"vf", L"af", L"ss", L"t", L"hwaccel", L"volume", L"loop", L"sync", L"codec", L"vcodec",
                                             L"acodec", L"scodec", L"f", L"x", L"y", L"ast", L"vst", L"sst", L"drp", L"lowres",
                                             L"filter_threads", L"vulkan_params", L"seek_interval", L"bytes", L"window_title",
                                             L"showmode", L"left", L"top", L"rdftspeed", L"max_alloc", L"cpuflags", L"cpucount",
                                             L"loglevel", L"v"};
    return s;
}
const std::set<std::wstring>& FlagKeys() {
    static const std::set<std::wstring> s = {L"stats", L"fast", L"genpts", L"framedrop", L"infbuf", L"autoexit", L"an", L"vn", L"sn",
                                             L"nodisp", L"noborder", L"fs", L"autorotate", L"enable_vulkan", L"find_stream_info",
                                             L"alwaysontop", L"exitonkeydown", L"exitonmousedown", L"hide_banner"};
    return s;
}
bool IsFilterKey(const std::wstring& k) { return k == L"vf" || k == L"af" || k == L"vulkan_params" || k == L"window_title"; }

bool Truthy(const std::wstring& v) {
    std::wstring l = v;
    for (auto& c : l) c = (wchar_t)towlower(c);
    return l.empty() || l == L"1" || l == L"true" || l == L"yes" || l == L"on";
}
bool Falsy(const std::wstring& v) {
    std::wstring l = v;
    for (auto& c : l) c = (wchar_t)towlower(c);
    return l == L"0" || l == L"false" || l == L"no" || l == L"off";
}
}

std::wstring FormatOptionsLine(const wvp::OpenOptions& options) {
    if (options.empty()) return L"(none)";
    std::wstring s;
    for (const auto& [k, v] : options) {
        if (!s.empty()) s += L' ';
        bool q = v.find_first_of(L" \t\"") != std::wstring::npos;
        s += k + L"=" + (q ? QuoteArg(v, true) : v);
    }
    return s;
}

wvp::OpenOptions EffectiveOptions(const wvp::OpenOptions& open, const std::wstring& vf, const std::wstring& af) {
    wvp::OpenOptions out;
    bool haveVf = false, haveAf = false;
    for (const auto& [k, v] : open) {
        if (k == L"vf" || k == L"af") {
            bool isVf = k == L"vf";
            bool& have = isVf ? haveVf : haveAf;
            const std::wstring& live = isVf ? vf : af;
            if (have) continue;                       // duplicate key: first one wins, as in the player
            have = true;
            if (!live.empty()) out.emplace_back(k, live);
            continue;
        }
        out.emplace_back(k, v);
    }
    if (!haveVf && !vf.empty()) out.emplace_back(L"vf", vf);
    if (!haveAf && !af.empty()) out.emplace_back(L"af", af);
    return out;
}

std::wstring QuoteArg(const std::wstring& v, bool force) {
    bool needs = force || v.empty() || v.find_first_of(L" \t\"&|<>^%,;()$'`!") != std::wstring::npos;
    if (!needs) return v;
    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (wchar_t c : v) {
        if (c == L'\\') { backslashes++; continue; }
        if (c == L'"') { out.append(backslashes * 2 + 1, L'\\'); out += L'"'; backslashes = 0; continue; }
        out.append(backslashes, L'\\');
        backslashes = 0;
        out += c;
    }
    out.append(backslashes * 2, L'\\');   // trailing backslashes must not escape the closing quote
    out += L'"';
    return out;
}

CliKind ClassifyOption(const std::wstring& key) {
    if (key == L"filter_hw_device") return CliKind::NoCli;
    if (FlagKeys().count(key)) return CliKind::Flag;
    if (ValueKeys().count(key)) return CliKind::Value;
    return CliKind::Value;   // AVFormat / AVCodec / protocol option: -<key> <value>
}

std::wstring FfplayCommandLine(const std::wstring& url, const wvp::OpenOptions& options, const std::wstring& exe) {
    std::wstring line = exe;
    std::wstring comments;
    for (const auto& [k, v] : options) {
        switch (ClassifyOption(k)) {
        case CliKind::NoCli: {
            std::wstring kv = k + L"=" + v;
            std::replace(kv.begin(), kv.end(), L'"', L'\'');
            comments += L" # no CLI equivalent: " + kv;
            break;
        }
        case CliKind::Flag:
            if (Truthy(v)) line += L" -" + k;
            else if (Falsy(v)) line += L" -no" + k;
            else line += L" -" + k + L" " + QuoteArg(v);
            break;
        case CliKind::Value:
            line += L" -" + k + L" " + QuoteArg(v, IsFilterKey(k));
            break;
        }
    }
    line += L" " + QuoteArg(url, true);
    line += comments;
    return line;
}

}
