#include "features/LoggingFormat.h"


namespace app::log {

std::string FormatOptionsLine(const wvp::OpenOptions& options) {
    if (options.empty()) return "(none)";
    std::string s;
    for (const auto& [k, v] : options) {
        if (!s.empty()) s += ' ';
        bool q = v.find_first_of(" \t\"") != std::string::npos;
        s += k + "=" + (q ? QuoteArg(v, true) : v);
    }
    return s;
}

wvp::OpenOptions EffectiveOptions(const wvp::OpenOptions& open, const std::string& vf, const std::string& af) {
    wvp::OpenOptions out;
    bool haveVf = false, haveAf = false;
    for (const auto& [k, v] : open) {
        if (k == "vf" || k == "af") {
            bool isVf = k == "vf";
            bool& have = isVf ? haveVf : haveAf;
            const std::string& live = isVf ? vf : af;
            if (have) continue;                       // duplicate key: first one wins, as in the player
            have = true;
            if (!live.empty()) out.emplace_back(k, live);
            continue;
        }
        out.emplace_back(k, v);
    }
    if (!haveVf && !vf.empty()) out.emplace_back("vf", vf);
    if (!haveAf && !af.empty()) out.emplace_back("af", af);
    return out;
}

std::string QuoteArg(const std::string& v, bool force) {
    bool needs = force || v.empty() || v.find_first_of(" \t\"&|<>^%,;()$'`!") != std::string::npos;
    if (!needs) return v;
    std::string out = "\"";
    size_t backslashes = 0;
    for (char c : v) {
        if (c == '\\') { backslashes++; continue; }
        if (c == '"') { out.append(backslashes * 2 + 1, '\\'); out += '"'; backslashes = 0; continue; }
        out.append(backslashes, '\\');
        backslashes = 0;
        out += c;
    }
    out.append(backslashes * 2, '\\');   // trailing backslashes must not escape the closing quote
    out += '"';
    return out;
}

}
