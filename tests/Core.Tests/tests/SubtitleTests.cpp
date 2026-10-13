// libass text subtitles (embedded, external file) and PGS bitmap subtitles on the recording presenter, checked through
// paused snapshots with and without the subtitle layer.
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>

#include "wvp/FfPlayer.h"
#include "wvp/win32/Utf.h"
#include "TestPlayer.h"
#include "wvptest.h"

using namespace wvp;
namespace fs = std::filesystem;

namespace {

fs::path SubtitleDir() {
    wchar_t buf[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, buf, (DWORD)std::size(buf));
    for (fs::path d = fs::path(buf).parent_path(); !d.empty() && d != d.parent_path(); d = d.parent_path())
        if (fs::exists(d / L"CMakePresets.json")) return d / L"testmedia" / L"subtitles";
    return fs::current_path() / L"testmedia" / L"subtitles";
}

struct SubSession {
    std::unique_ptr<FfPlayer> player;
    int prepared = 0, errors = 0;

    explicit SubSession(const wchar_t* file) {
        const fs::path path = SubtitleDir() / file;
        if (!fs::exists(path)) throw wvptest::SkipFailure{ "subtitle fixture is not generated (scripts/make-testmedia.ps1)" };
        int err = 0;
        player = wvptest::CreateTestPlayer(nullptr, &err);
        REQUIRE_MSG(player != nullptr, "FfPlayer::Create failed: " + std::to_string(err));
        player->Prepared.add([this](const MediaPreparedInfo&) { prepared++; });
        player->Error.add([this](const PlayerErrorInfo&) { errors++; });
        player->SetVolume(0);
        REQUIRE_MSG(player->Open(ToUtf8(path.wstring()), { { "an", "1" }, { "start_paused", "1" } }) == 0, "open rejected");
        REQUIRE_MSG(wvptest::WaitFor([&] { return prepared > 0 || errors > 0; }, 20), "no PREPARED event");
        REQUIRE_MSG(errors == 0, "open raised an error");
    }

    // Pixels where the subtitle snapshot differs from the plain one of the same paused frame.
    int SubtitlePixels() {
        auto plain = player->Snapshot(false);
        auto with = player->Snapshot(true);
        if (!plain || !with || plain->Width != with->Width || plain->Height != with->Height) return -1;
        int n = 0;
        for (int y = 0; y < plain->Height; y++)
            for (int x = 0; x < plain->Width; x++) {
                const std::uint8_t* a = plain->Data.data() + (size_t)y * plain->Stride + (size_t)x * 4;
                const std::uint8_t* b = with->Data.data() + (size_t)y * with->Stride + (size_t)x * 4;
                if (std::abs(a[0] - b[0]) > 24 || std::abs(a[1] - b[1]) > 24 || std::abs(a[2] - b[2]) > 24) n++;
            }
        return n;
    }

    void PlayTo(double seconds) {
        REQUIRE_MSG(player->Play() == 0, "play rejected");
        REQUIRE_MSG(wvptest::WaitFor([&] { return player->Position() >= seconds + 0.1; }, 15), "position not reached");
        REQUIRE_MSG(player->Pause() == 0, "pause rejected");
        wvptest::Delay(500);
    }
    bool WaitPixels(double seconds) {
        return wvptest::WaitFor([&] { return SubtitlePixels() > 200; }, seconds);
    }
    bool StaysEmpty() {
        wvptest::Delay(300);
        return SubtitlePixels() == 0;
    }
    const StreamInfo* FindSubtitle(const MediaInfo& mi, const std::string& codec, bool external) {
        for (const auto& s : mi.Streams)
            if (s.Type == StreamType::Subtitle && s.Codec == codec && s.external == external) return &s;
        return nullptr;
    }
};

}

TEST_CASE_INTEGRATION(Subtitle_embedded_srt_draws_inside_cue_only) {
    SubSession s(L"video-subs.mkv");
    auto mi = s.player->GetMediaInfo();
    REQUIRE(mi.has_value());
    const StreamInfo* srt = s.FindSubtitle(*mi, "subrip", false);
    REQUIRE_MSG(srt != nullptr, "no embedded subrip stream");
    CHECK(srt->isText);
    CHECK(srt->path.empty());

    s.PlayTo(2.8);
    CHECK_MSG(s.WaitPixels(5), "no subtitle pixels inside cue 2-4 s");
    s.PlayTo(4.6);
    CHECK_MSG(s.StaysEmpty(), "subtitle pixels between cues");
}

TEST_CASE_INTEGRATION(Subtitle_external_file_adds_selectable_stream) {
    SubSession s(L"video-subs.mkv");
    const std::string path = ToUtf8((SubtitleDir() / L"cues.srt").wstring());
    const int idx = s.player->AddSubtitleFile(path);
    REQUIRE_MSG(idx >= 0, "AddSubtitleFile failed: " + std::to_string(idx));
    REQUIRE(s.player->SelectStream(StreamType::Subtitle, idx) == 0);

    auto mi = s.player->GetMediaInfo();
    REQUIRE(mi.has_value());
    const StreamInfo* ext = nullptr;
    for (const auto& st : mi->Streams) if (st.Type == StreamType::Subtitle && st.Index == idx) ext = &st;
    REQUIRE_MSG(ext != nullptr, "external stream missing from media info");
    CHECK(ext->external);
    CHECK(ext->isText);
    CHECK(ext->Selected);
    CHECK_EQ(ext->path, path);

    REQUIRE(s.player->Seek(6.8) == 0);
    wvptest::Delay(700);
    CHECK_MSG(s.WaitPixels(5), "no subtitle pixels from the external track inside cue 6-8 s");

    REQUIRE(s.player->SelectStream(StreamType::Subtitle, -1) == 0);
    wvptest::Delay(400);
    CHECK_MSG(s.StaysEmpty(), "subtitle pixels after turning subtitles off");
}

TEST_CASE_INTEGRATION(Subtitle_pgs_bitmap_draws_inside_cue_only) {
    SubSession s(L"video-pgs.mkv");
    auto mi = s.player->GetMediaInfo();
    REQUIRE(mi.has_value());
    const StreamInfo* pgs = s.FindSubtitle(*mi, "hdmv_pgs_subtitle", false);
    REQUIRE_MSG(pgs != nullptr, "no PGS stream");
    CHECK(!pgs->isText);

    s.PlayTo(2.8);
    CHECK_MSG(s.WaitPixels(5), "no PGS pixels inside cue 2-4 s");
    s.PlayTo(4.6);
    CHECK_MSG(s.StaysEmpty(), "PGS pixels between cues");
}

TEST_CASE_INTEGRATION(Subtitle_delay_shifts_visibility) {
    SubSession s(L"video-subs.mkv");
    s.PlayTo(4.6);
    REQUIRE_MSG(s.StaysEmpty(), "unexpected subtitle pixels between cues");
    CHECK_EQ(s.player->SetSubtitleDelay(1500), 0);
    CHECK_EQ(s.player->SubtitleDelay(), 1500);
    CHECK_MSG(s.WaitPixels(5), "delay +1.5 s did not show cue 2-4 s at 4.6 s");
    CHECK_EQ(s.player->SetSubtitleDelay(0), 0);
    CHECK_MSG(wvptest::WaitFor([&] { return s.SubtitlePixels() == 0; }, 5), "subtitle stayed after the delay was cleared");
}

TEST_CASE_INTEGRATION(Subtitle_style_fallback_reaches_plain_text_tracks_only_through_its_flags) {
    SubSession s(L"video-subs.mkv");
    s.PlayTo(2.8);
    REQUIRE_MSG(s.WaitPixels(5), "no subtitle pixels inside cue 2-4 s");
    wvptest::Delay(300);
    const int plain = s.SubtitlePixels();

    SubtitleRenderStyle box;
    box.fallbackFlags = kSubBackground;
    box.backRgba = 0x2040C0FFu;
    REQUIRE(s.player->SetSubtitleStyle(box) == 0);
    CHECK_MSG(wvptest::WaitFor([&] { return s.SubtitlePixels() > plain * 3 / 2; }, 5),
              "the background box did not enlarge the subtitle area: " + std::to_string(plain) + " -> " +
              std::to_string(s.SubtitlePixels()));

    box.fallbackFlags = 0;   // the values alone change nothing
    REQUIRE(s.player->SetSubtitleStyle(box) == 0);
    CHECK_MSG(wvptest::WaitFor([&] { return std::abs(s.SubtitlePixels() - plain) <= plain / 10; }, 5),
              "the subtitle did not return to its own style: " + std::to_string(plain) + " -> " + std::to_string(s.SubtitlePixels()));
}
