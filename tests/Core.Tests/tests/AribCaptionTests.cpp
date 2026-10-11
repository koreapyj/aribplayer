// ARIB media/settings contracts and a libffplay fixture integration test on the recording presenter.
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>

#include "wvp/AppSettings.h"
#include "wvp/FfPlayer.h"
#include "wvp/JsonLite.h"
#include "wvp/win32/Utf.h"
#include "TestPlayer.h"
#include "wvptest.h"

using namespace wvp;
namespace fs = std::filesystem;

namespace {

fs::path RepoRoot() {
    static fs::path root = [] {
        wchar_t buf[MAX_PATH * 2];
        GetModuleFileNameW(nullptr, buf, (DWORD)std::size(buf));
        fs::path d = fs::path(buf).parent_path();
        while (!d.empty()) {
            if (fs::exists(d / L"CMakePresets.json")) return d;
            auto parent = d.parent_path();
            if (parent == d) break;
            d = parent;
        }
        return fs::current_path();
    }();
    return root;
}

fs::path AribFixture() { return RepoRoot() / L"testmedia" / L"arib" / L"tbs-captions.ts"; }
fs::path AribFont() { return RepoRoot() / L"assets" / L"fonts" / L"WLCMARU2004ARIBU.TTF"; }

template <class F>
bool WaitArib(F cond, double seconds = 10) { return wvptest::WaitFor(cond, seconds); }

std::optional<double> FindNumber(const json::Value& v, std::string_view key) {
    if (v.kind == json::Value::Kind::Object) {
        for (size_t i = 0; i < v.keys.size() && i < v.vals.size(); i++) {
            if (v.keys[i] == key && v.vals[i].kind == json::Value::Kind::Number) return v.vals[i].n;
            if (auto n = FindNumber(v.vals[i], key)) return n;
        }
    } else if (v.kind == json::Value::Kind::Array) {
        for (const auto& item : v.arr)
            if (auto n = FindNumber(item, key)) return n;
    }
    return std::nullopt;
}

bool SamePixels(const BgraFrame& a, const BgraFrame& b) {
    if (a.Width != b.Width || a.Height != b.Height || a.Data.size() < (size_t)a.Stride * a.Height ||
        b.Data.size() < (size_t)b.Stride * b.Height) return false;
    for (int y = 0; y < a.Height; y++)
        if (std::memcmp(a.Data.data() + (size_t)y * a.Stride, b.Data.data() + (size_t)y * b.Stride,
                        (size_t)a.Width * 4) != 0) return false;
    return true;
}

size_t DifferentPixels(const BgraFrame& a, const BgraFrame& b) {
    if (a.Width != b.Width || a.Height != b.Height || a.Data.size() < (size_t)a.Stride * a.Height ||
        b.Data.size() < (size_t)b.Stride * b.Height) return 0;
    size_t changed = 0;
    for (int y = 0; y < a.Height; y++)
        for (int x = 0; x < a.Width; x++) {
            const size_t ao = (size_t)y * a.Stride + (size_t)x * 4;
            const size_t bo = (size_t)y * b.Stride + (size_t)x * 4;
            if (std::memcmp(a.Data.data() + ao, b.Data.data() + bo, 4) != 0) changed++;
        }
    return changed;
}

struct AribSession {
    std::unique_ptr<FfPlayer> player;
    int prepared = 0, errors = 0;

    AribSession() {
        int err = 0;
        player = wvptest::CreateTestPlayer(nullptr, &err);
        REQUIRE_MSG(player != nullptr, "FfPlayer::Create failed: " + std::to_string(err));
        player->Prepared.add([this](const MediaPreparedInfo&) { prepared++; });
        player->Error.add([this](const PlayerErrorInfo&) { errors++; });
    }
    ~AribSession() { player.reset(); }

    void OpenFixture() {
        int before = prepared;
        OpenOptions options = {
            { "superimpose", "0" },
            { "arib_font", AribFontOption(wvp::ToUtf8(AribFont().wstring())) },
        };
        REQUIRE_MSG(player->Open(wvp::ToUtf8(AribFixture().wstring()), options) == 0, "ARIB fixture open rejected");
        REQUIRE_MSG(WaitArib([&] { return prepared > before || errors > 0; }, 20), "fixture produced no PREPARED event");
        REQUIRE_MSG(errors == 0, "fixture open raised an error");
    }

    std::optional<BgraFrame> WaitSnapshot(bool withSubtitles = false, double seconds = 10) {
        std::optional<BgraFrame> frame;
        bool ok = WaitArib([&] { frame = player->Snapshot(withSubtitles); return frame.has_value(); }, seconds);
        REQUIRE_MSG(ok, "no snapshot frame became available");
        return frame;
    }
};

}

TEST_CASE(Arib_media_info_preserves_caption_and_superimpose_classification) {
    const char* js = R"({"url":"arib.ts","streams":[
        {"index":3,"type":"subtitle","codec":"arib_caption","selected":true,"arib_type":"caption","component_tag":56},
        {"index":4,"type":"subtitle","codec":"arib_caption","selected":false,"arib_type":"superimpose","component_tag":136},
        {"index":5,"type":"subtitle","codec":"subrip","selected":false,"language":"eng"}]})";
    auto mi = MediaInfo::Parse(js);
    REQUIRE(mi.has_value());
    const auto subs = mi->Of(StreamType::Subtitle);
    REQUIRE(subs.size() == 3);
    CHECK_EQ(subs[0].AribType, std::string("caption"));
    CHECK_EQ(subs[0].ComponentTag, 56);
    CHECK_EQ(subs[1].AribType, std::string("superimpose"));
    CHECK_EQ(subs[1].ComponentTag, 136);
    CHECK(subs[2].AribType.empty());
    CHECK_EQ(subs[2].ComponentTag, -1);
}

TEST_CASE(Arib_settings_default_on_migrate_and_round_trip) {
    AppSettings defaults;
    CHECK(defaults.showCaptions);
    CHECK(defaults.showSuperimpose);
    CHECK(defaults.aribFont.empty());

    AppSettings old;
    REQUIRE(SettingsStore::FromJson("{\"volume\":42}", old));
    CHECK(old.showCaptions);
    CHECK(old.showSuperimpose);
    CHECK(old.aribFont.empty());

    AppSettings saved;
    saved.showCaptions = false;
    saved.showSuperimpose = false;
    saved.aribFont = "D:\\Fonts\\WLCMARU2004ARIBU.TTF";
    AppSettings restored;
    REQUIRE(SettingsStore::FromJson(SettingsStore::ToJson(saved), restored));
    CHECK_EQ(restored.showCaptions, false);
    CHECK_EQ(restored.showSuperimpose, false);
    CHECK_EQ(restored.aribFont, saved.aribFont);
}

TEST_CASE_INTEGRATION(Arib_fixture_caption_classification_visibility_and_snapshot) {
    if (!fs::exists(AribFixture())) throw wvptest::SkipFailure{ "gitignored ARIB fixture is not present" };
    REQUIRE_MSG(fs::exists(AribFont()), "bundled WLC Maru ARIB font is missing");

    AribSession s;
    s.OpenFixture();
    auto mi = s.player->GetMediaInfo();
    REQUIRE(mi.has_value());
    const StreamInfo* caption = nullptr;
    for (const auto& stream : mi->Streams) {
        if (stream.Type == StreamType::Subtitle && stream.AribType == "caption") { caption = &stream; break; }
    }
    REQUIRE_MSG(caption != nullptr, "media info did not classify an ARIB caption stream");
    CHECK_EQ(caption->Codec, std::string("arib_caption"));

    bool decodedCue = WaitArib([&] {
        auto text = s.player->GetDebugInfoJson();
        if (!text) return false;
        auto root = json::Parse(*text);
        if (!root) return false;
        auto shown = FindNumber(*root, "cues_shown");
        return shown && *shown > 0;
    }, 25);
    REQUIRE_MSG(decodedCue, "debug info did not report a decoded ARIB caption cue");

    CHECK_EQ(s.player->Pause(), 0);
    REQUIRE_MSG(WaitArib([&] { return s.player->State() == PlayerState::Paused; }, 5), "player did not pause");
    auto videoOnly = s.WaitSnapshot(false);
    auto withCaption = s.WaitSnapshot(true);
    REQUIRE(videoOnly.has_value() && withCaption.has_value());
    CHECK_EQ(videoOnly->Width, withCaption->Width);
    CHECK_EQ(videoOnly->Height, withCaption->Height);
    CHECK_MSG(DifferentPixels(*videoOnly, *withCaption) > 0, "subtitle snapshot contained no caption pixels");

    CHECK_EQ(s.player->SetSubtitleVisible(SubtitleLayer::Caption, false), 0);
    auto hidden = s.WaitSnapshot(true);
    REQUIRE(hidden.has_value());
    CHECK_MSG(SamePixels(*videoOnly, *hidden), "caption visibility off still changed the video-only snapshot");

    CHECK_EQ(s.player->SetSubtitleVisible(SubtitleLayer::Caption, true), 0);
    auto shownAgain = s.WaitSnapshot(true);
    REQUIRE(shownAgain.has_value());
    CHECK_MSG(DifferentPixels(*videoOnly, *shownAgain) > 0, "active caption did not reappear after hide/show");
}
