// ARIB media/settings contracts and a libffplay fixture integration test on the recording presenter.
#include <chrono>
#include <cmath>
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

const json::Value* FindKey(const json::Value& v, std::string_view key) {
    if (v.kind == json::Value::Kind::Object) {
        for (size_t i = 0; i < v.keys.size() && i < v.vals.size(); i++) {
            if (v.keys[i] == key) return &v.vals[i];
            if (auto f = FindKey(v.vals[i], key)) return f;
        }
    } else if (v.kind == json::Value::Kind::Array) {
        for (const auto& item : v.arr)
            if (auto f = FindKey(item, key)) return f;
    }
    return nullptr;
}

struct ShownCue { double start; std::optional<double> end; };

std::optional<ShownCue> ShownCaptionCue(FfPlayer& player) {
    auto text = player.GetDebugInfoJson();
    if (!text) return std::nullopt;
    auto root = json::Parse(*text);
    if (!root) return std::nullopt;
    const json::Value* cue = FindKey(*root, "active_cue");
    if (!cue) return std::nullopt;
    auto start = FindNumber(*cue, "start");
    if (!start) return std::nullopt;
    return ShownCue{ *start, FindNumber(*cue, "end") };
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

    AppSettings old;
    REQUIRE(SettingsStore::FromJson("{\"volume\":42}", old));
    CHECK(old.showCaptions);
    CHECK(old.showSuperimpose);

    AppSettings saved;
    saved.showCaptions = false;
    saved.showSuperimpose = false;
    AppSettings restored;
    REQUIRE(SettingsStore::FromJson(SettingsStore::ToJson(saved), restored));
    CHECK_EQ(restored.showCaptions, false);
    CHECK_EQ(restored.showSuperimpose, false);
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

TEST_CASE_INTEGRATION(Arib_caption_text_under_the_point_without_ruby) {
    if (!fs::exists(AribFixture())) throw wvptest::SkipFailure{ "gitignored ARIB fixture is not present" };
    AribSession s;
    OpenOptions options = {
        { "superimpose", "0" },
        { "an", "1" },
        { "arib_font", AribFontOption(wvp::ToUtf8(AribFont().wstring())) },
    };
    REQUIRE_MSG(s.player->Open(wvp::ToUtf8(AribFixture().wstring()), options) == 0, "ARIB fixture open rejected");
    REQUIRE_MSG(WaitArib([&] { return s.prepared > 0 || s.errors > 0; }, 20) && s.errors == 0, "fixture did not open");

    std::optional<std::string> text;
    int misses = 0;
    auto sweep = [&] {
        text.reset();
        misses = 0;
        for (int gy = 0; gy < 25; gy++)
            for (int gx = 0; gx < 25; gx++) {
                auto t = s.player->CaptionTextAt((gx + 0.5) / 25, (gy + 0.5) / 25);
                if (t) { if (!text) text = t; }
                else misses++;
            }
        return text.has_value();
    };
    REQUIRE_MSG(WaitArib(sweep, 25), "no caption text answered while the fixture played");
    CHECK_EQ(s.player->Pause(), 0);
    REQUIRE_MSG(WaitArib([&] { return s.player->State() == PlayerState::Paused; }, 5), "player did not pause");
    REQUIRE_MSG(WaitArib(sweep, 5), "no caption text answered while paused");
    CHECK_MSG(!text->empty() && text->front() != '\n' && text->back() != '\n', "caption text is not trimmed");
    CHECK_MSG(misses > 0, "every point of the video area answered with the caption text");
    CHECK(!s.player->CaptionTextAt(0.005, 0.005).has_value());

    CHECK_EQ(s.player->SetSubtitleVisible(SubtitleLayer::Caption, false), 0);
    CHECK_MSG(WaitArib([&] { return !sweep(); }, 5), "caption text answered while the layer is hidden");
}

TEST_CASE_INTEGRATION(Arib_caption_follows_the_subtitle_delay_paused_and_playing) {
    if (!fs::exists(AribFixture())) throw wvptest::SkipFailure{ "gitignored ARIB fixture is not present" };
    AribSession s;
    OpenOptions options = {
        { "superimpose", "0" },
        { "an", "1" },
        { "arib_font", AribFontOption(wvp::ToUtf8(AribFont().wstring())) },
    };
    REQUIRE_MSG(s.player->Open(wvp::ToUtf8(AribFixture().wstring()), options) == 0, "ARIB fixture open rejected");
    REQUIRE_MSG(WaitArib([&] { return s.prepared > 0 || s.errors > 0; }, 20) && s.errors == 0, "fixture did not open");

    // playing without delay: how far the position is past a cue's own time when it is first seen
    auto firstSeenLag = [&](double seconds) {
        std::optional<double> lag;
        bool ok = WaitArib([&] {
            auto cue = ShownCaptionCue(*s.player);
            if (!cue) return false;
            lag = s.player->Position() - cue->start;
            return true;
        }, seconds);
        REQUIRE_MSG(ok, "no caption cue was shown");
        return *lag;
    };
    std::optional<ShownCue> shown;
    REQUIRE_MSG(WaitArib([&] { shown = ShownCaptionCue(*s.player); return shown.has_value(); }, 25), "no caption cue was shown");
    CHECK_EQ(s.player->Pause(), 0);
    REQUIRE_MSG(WaitArib([&] { return s.player->State() == PlayerState::Paused; }, 5), "player did not pause");
    shown = ShownCaptionCue(*s.player);
    REQUIRE(shown.has_value());

    const double cueStart = shown->start;
    const double delay = shown->end ? *shown->end - shown->start + 1.0 : 60.0;
    CHECK_EQ(s.player->SetSubtitleDelay((std::int64_t)(delay * 1000)), 0);
    CHECK_MSG(WaitArib([&] {
        auto now = ShownCaptionCue(*s.player);
        return !now || now->start < cueStart - 0.5;
    }, 5), "the paused caption stayed on screen after a positive delay longer than the cue");
    CHECK_EQ(s.player->SetSubtitleDelay(0), 0);
    std::optional<ShownCue> back;
    CHECK_MSG(WaitArib([&] {
        back = ShownCaptionCue(*s.player);
        return back && std::abs(back->start - cueStart) < 0.01;
    }, 5), "the paused caption did not return after the delay was reset: " + (back ? std::to_string(back->start) : std::string("none")) + " vs " + std::to_string(cueStart) + " delay " + std::to_string(delay));

    // playing: a cue is due its delay later
    CHECK_EQ(s.player->Seek(0), 0);
    CHECK_EQ(s.player->Play(), 0);
    REQUIRE_MSG(WaitArib([&] { return !ShownCaptionCue(*s.player); }, 5), "the caption stayed on screen after the seek to the start");
    const double lagPlain = firstSeenLag(25);
    CHECK_EQ(s.player->SetSubtitleDelay(2000), 0);
    CHECK_EQ(s.player->Seek(0), 0);
    REQUIRE_MSG(WaitArib([&] { return !ShownCaptionCue(*s.player); }, 5), "the caption stayed on screen after the seek to the start");
    const double lagDelayed = firstSeenLag(25);
    CHECK_MSG(std::abs((lagDelayed - lagPlain) - 2.0) < 0.3,
              "a delayed cue did not appear 2 s later: " + std::to_string(lagPlain) + " vs " + std::to_string(lagDelayed));
}

// testmedia/arib/tbs-captions-wrap.ts (ffmpeg/tools/make_arib_wrap_fixture.py): the 33-bit clock wraps 9 s in (media
// time 0 once libavformat unwraps it) and the caption component joins the PMT only after the first video packet.
TEST_CASE_INTEGRATION(Arib_caption_after_the_33_bit_clock_wrap_is_shown_at_its_time) {
    const fs::path wrapFixture = RepoRoot() / L"testmedia" / L"arib" / L"tbs-captions-wrap.ts";
    if (!fs::exists(wrapFixture)) throw wvptest::SkipFailure{ "generated wrap fixture is not present (ffmpeg/tools/make_arib_wrap_fixture.py)" };
    AribSession s;
    OpenOptions options = {
        { "superimpose", "0" },
        { "an", "1" },
        { "arib_font", AribFontOption(wvp::ToUtf8(AribFont().wstring())) },
    };
    REQUIRE_MSG(s.player->Open(wvp::ToUtf8(wrapFixture.wstring()), options) == 0, "wrap fixture open rejected");
    REQUIRE_MSG(WaitArib([&] { return s.prepared > 0 || s.errors > 0; }, 20) && s.errors == 0, "wrap fixture did not open");

    // cue start is on the stream timeline, Position() counts from the first timestamp: their difference is constant
    std::optional<double> offsetBefore, offsetAfter;
    bool ok = WaitArib([&] {
        auto cue = ShownCaptionCue(*s.player);
        if (cue && cue->start <= 0 && !offsetBefore) offsetBefore = s.player->Position() - cue->start;
        if (cue && cue->start > 0 && !offsetAfter) offsetAfter = s.player->Position() - cue->start;
        return offsetAfter.has_value() || s.errors > 0;
    }, 30);
    REQUIRE_MSG(offsetBefore.has_value(), "no caption cue before the clock wrap was shown");
    REQUIRE_MSG(ok && offsetAfter.has_value(), "no caption cue after the clock wrap was shown");
    CHECK_MSG(std::abs(*offsetAfter - *offsetBefore) < 5.0,
              "a cue after the wrap is off the clock: " + std::to_string(*offsetBefore) + " before, " + std::to_string(*offsetAfter) + " after");
}
