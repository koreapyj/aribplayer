#include "wvptest.h"

#include <algorithm>
#include <deque>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "wvp/PlaybackController.h"
#include "wvp/PlaylistStore.h"

using namespace wvp;

namespace {

struct OpenedEntry { std::string url; OpenOptions options; };

std::string OptGet(const OpenOptions& o, const std::string& k, bool* found = nullptr) {
    for (auto& e : o) if (e.first == k) { if (found) *found = true; return e.second; }
    if (found) *found = false;
    return "";
}

class FakePlayer : public IMediaPlayer {
public:
    std::vector<std::string> Calls;
    std::vector<OpenedEntry> Opened;
    std::vector<std::vector<std::string>> Commands;
    int* disposedCounter = nullptr;
    std::string LastVf, LastAf;
    PlayerState state = PlayerState::Idle;
    int volume = 100;
    bool muted = false;

    ~FakePlayer() override { if (disposedCounter) ++*disposedCounter; }

    PlayerState State() const override { return state; }
    int Volume() const override { return volume; }
    void SetVolume(int v) override { volume = v; Calls.push_back("volume " + std::to_string(v)); }
    bool IsMuted() const override { return muted; }
    void SetMuted(bool m) override { muted = m; Calls.push_back(std::string("mute ") + (m ? "True" : "False")); }
    double Position() override { return 0; }
    double Duration() override { return 10; }

    int Open(const std::string& url, const OpenOptions& options) override {
        Calls.push_back("open " + url);
        Opened.push_back({url, options});
        bool startPaused = false;
        for (auto& o : options) if (o.first == "start_paused" && o.second == "1") startPaused = true;
        state = startPaused && honorsStartPaused ? PlayerState::Paused : PlayerState::Playing;
        return 0;
    }
    int Stop() override { Calls.push_back("stop"); state = PlayerState::Stopped; return 0; }
    int Play() override { Calls.push_back("play"); state = PlayerState::Playing; return 0; }
    int Pause() override { Calls.push_back("pause"); state = PlayerState::Paused; return 0; }
    bool honorsStartPaused = false;   // like libffplay: start_paused=1 ends the open paused
    int TogglePause() override { Calls.push_back("toggle"); return 0; }
    int Seek(double s, bool rel) override {
        Calls.push_back("seek " + std::to_string((int)s) + (rel ? " True" : " False"));
        return 0;
    }
    int CycleStream(StreamType) override { return 0; }
    int SetVideoFilter(const std::string& vf) override { LastVf = vf; Calls.push_back("vf " + vf); return 0; }
    int SetAudioFilter(const std::string& af) override { LastAf = af; Calls.push_back("af " + af); return 0; }
    int SendFilterCommand(const std::string& t, const std::string& c, const std::string& a) override {
        Commands.push_back({t, c, a});
        return 0;
    }
    std::optional<MediaInfo> GetMediaInfo() override { return std::nullopt; }
    std::optional<BgraFrame> Snapshot() override { return std::nullopt; }

    void RaiseEnded() { state = PlayerState::Ended; Ended.raise(); }
    std::vector<std::string> OpenedUrls() const {
        std::vector<std::string> r;
        for (auto& o : Opened) r.push_back(o.url);
        return r;
    }
    bool HasCall(const std::string& c) const { return std::find(Calls.begin(), Calls.end(), c) != Calls.end(); }
    int LastIndexOf(const std::string& c) const {
        for (int i = (int)Calls.size() - 1; i >= 0; --i) if (Calls[i] == c) return i;
        return -1;
    }
};

using Urls = std::vector<std::string>;

std::vector<MediaItem> Items(int n) {
    std::vector<MediaItem> v;
    for (int i = 1; i <= n; ++i) v.emplace_back("m" + std::to_string(i) + ".mp4", "T" + std::to_string(i));
    return v;
}

// A pipeline plan that needs no options (software decoding, no filters); opens wait without a plan.
FilterProvider Plain() {
    FilterProvider fp;
    fp.planOptions = [] { return std::optional<OpenOptions>(OpenOptions{}); };
    return fp;
}

struct Fixture {
    FakePlayer p;
    std::unique_ptr<PlaybackController> c;
    explicit Fixture(int n = 3, unsigned seed = 1) {
        c = std::make_unique<PlaybackController>(p, PlaybackController::PostFn{}, seed);
        c->SetFilters(Plain());
        c->Enqueue(Items(n));
    }
};

Urls UrlsOf(const std::vector<MediaItem>& v) {
    Urls r;
    for (auto& i : v) r.push_back(i.url);
    return r;
}

Urls Insertion(int n) {
    Urls r;
    for (int i = 1; i <= n; ++i) r.push_back("m" + std::to_string(i) + ".mp4");
    return r;
}

}

TEST_CASE(PC_PlayOpensFirstItemAndStopsBeforeOpen) {
    Fixture f;
    f.c->Play();
    REQUIRE(f.c->Current() != nullptr);
    CHECK_EQ(f.c->Current()->url, std::string("m1.mp4"));
    CHECK_EQ(f.c->CurrentIndex(), 0);
    CHECK(f.p.HasCall("stop"));
    CHECK(f.p.HasCall("open m1.mp4"));
    auto it = std::find(f.p.Calls.begin(), f.p.Calls.end(), "stop");
    auto io = std::find(f.p.Calls.begin(), f.p.Calls.end(), "open m1.mp4");
    CHECK(it < io);
}

TEST_CASE(PC_NextAndPreviousWithoutRepeat) {
    Fixture f;
    f.c->Play();
    CHECK(f.c->Next());
    CHECK(f.c->Next());
    CHECK(!f.c->Next());
    CHECK_EQ(f.c->Current()->url, std::string("m3.mp4"));
    CHECK(f.c->Previous());
    CHECK_EQ(f.c->Current()->url, std::string("m2.mp4"));
    CHECK(f.c->Previous());
    CHECK(!f.c->Previous());
    CHECK_EQ(f.c->Current()->url, std::string("m1.mp4"));
    CHECK(f.p.HasCall("seek 0 False"));
    CHECK(f.p.OpenedUrls() == (Urls{"m1.mp4", "m2.mp4", "m3.mp4", "m2.mp4", "m1.mp4"}));
}

TEST_CASE(PC_RepeatAllWrapsBothWays) {
    Fixture f;
    f.c->SetRepeat(RepeatMode::All);
    f.c->Play();
    CHECK(f.c->Previous());
    CHECK_EQ(f.c->Current()->url, std::string("m3.mp4"));
    CHECK(f.c->Next());
    CHECK_EQ(f.c->Current()->url, std::string("m1.mp4"));
}

TEST_CASE(PC_AutoAdvanceOnEndedPerRepeatMode) {
    Fixture f(2);
    bool queueEnded = false;
    f.c->QueueEnded.add([&] { queueEnded = true; });
    f.c->Play();

    f.p.RaiseEnded();
    CHECK_EQ(f.c->Current()->url, std::string("m2.mp4"));
    f.p.RaiseEnded();
    CHECK(queueEnded);
    CHECK_EQ(f.c->Current()->url, std::string("m2.mp4"));
    CHECK(f.p.OpenedUrls() == (Urls{"m1.mp4", "m2.mp4"}));

    f.c->SetRepeat(RepeatMode::One);
    f.p.RaiseEnded();
    CHECK(f.p.OpenedUrls() == (Urls{"m1.mp4", "m2.mp4", "m2.mp4"}));
    CHECK(!f.c->Next());
    f.c->SetRepeat(RepeatMode::All);
    f.p.RaiseEnded();
    CHECK_EQ(f.c->Current()->url, std::string("m1.mp4"));
}

TEST_CASE(PC_RepeatOneManualNextAdvancesButEndedRepeats) {
    Fixture f;
    f.c->SetRepeat(RepeatMode::One);
    f.c->Play();
    f.c->Next();
    CHECK_EQ(f.c->Current()->url, std::string("m2.mp4"));
    f.p.RaiseEnded();
    CHECK_EQ(f.c->Current()->url, std::string("m2.mp4"));
    CHECK(f.p.OpenedUrls() == (Urls{"m1.mp4", "m2.mp4", "m2.mp4"}));
}

TEST_CASE(PC_EndedIsPostedThroughThePostFunction) {
    FakePlayer p;
    std::deque<std::function<void()>> q;
    PlaybackController c(p, [&](std::function<void()> fn) { q.push_back(std::move(fn)); });
    c.SetFilters(Plain());
    c.Enqueue(Items(2));
    c.Play();
    p.RaiseEnded();
    CHECK_EQ(p.Opened.size(), (size_t)1);   // not yet: waits for the context
    while (!q.empty()) { auto fn = std::move(q.front()); q.pop_front(); fn(); }
    CHECK_EQ(p.Opened.size(), (size_t)2);
}

TEST_CASE(PC_PostedEndedIsDroppedAfterStopOrControllerDestruction) {
    FakePlayer p;
    std::deque<std::function<void()>> q;
    {
        PlaybackController c(p, [&](std::function<void()> fn) { q.push_back(std::move(fn)); });
        c.SetFilters(Plain());
        c.Enqueue(Items(2));
        c.Play();
        p.RaiseEnded();
        c.Stop();                           // generation bump invalidates the posted advance
        while (!q.empty()) { auto fn = std::move(q.front()); q.pop_front(); fn(); }
        CHECK_EQ(p.Opened.size(), (size_t)1);
        p.RaiseEnded();
    }                                       // destroyed with one advance still queued
    while (!q.empty()) { auto fn = std::move(q.front()); q.pop_front(); fn(); }
    CHECK_EQ(p.Opened.size(), (size_t)1);
}

TEST_CASE(PC_ShuffleKeepsCurrentAndIsStable) {
    Fixture f(8, 42);
    f.c->PlayAt(3);
    f.c->SetShuffle(true);
    CHECK_EQ(f.c->CurrentIndex(), 3);
    Urls order1 = UrlsOf(f.c->PlayOrder());
    CHECK_EQ(order1[0], std::string("m4.mp4"));
    Urls sorted1 = order1, expectSorted = Insertion(8);
    std::sort(sorted1.begin(), sorted1.end());
    std::sort(expectSorted.begin(), expectSorted.end());
    CHECK(sorted1 == expectSorted);

    CHECK(order1 == UrlsOf(f.c->PlayOrder()));
    Urls visited{f.c->Current()->url};
    while (f.c->Next()) visited.push_back(f.c->Current()->url);
    CHECK(order1 == visited);

    f.c->SetShuffle(false);
    CHECK(UrlsOf(f.c->PlayOrder()) == Insertion(8));
    CHECK_EQ(f.c->Current()->url, order1.back());
}

TEST_CASE(PC_ShuffleRepeatAllCyclesThroughEveryItem) {
    Fixture f(5, 7);
    f.c->SetShuffle(true);
    f.c->SetRepeat(RepeatMode::All);
    f.c->Play();
    for (int i = 0; i < 4; i++) f.c->Next();
    auto urls = f.p.OpenedUrls();
    CHECK_EQ(std::set<std::string>(urls.begin(), urls.end()).size(), (size_t)5);
    f.c->Next();
    CHECK_EQ(f.p.Opened.front().url, f.p.Opened.back().url);
}

TEST_CASE(PC_EnqueueWhileShuffledKeepsHistoryStable) {
    Fixture f(4, 3);
    f.c->SetShuffle(true);
    f.c->Play();
    f.c->Next();
    Urls played = UrlsOf(f.c->PlayOrder());
    played.resize(2);
    f.c->Enqueue(MediaItem("extra.mp4"));
    Urls after = UrlsOf(f.c->PlayOrder());
    CHECK(Urls(after.begin(), after.begin() + 2) == played);
    CHECK_EQ(after.size(), (size_t)5);
}

TEST_CASE(PC_VolumeAndMutePersistAcrossItems) {
    Fixture f;
    f.c->SetVolume(35);
    f.c->SetMuted(true);
    f.c->Play();
    f.c->Next();
    CHECK_EQ(f.p.volume, 35);
    CHECK(f.p.muted);
    int idxOpen = f.p.LastIndexOf("open m2.mp4");
    REQUIRE(idxOpen >= 0);
    Urls tail(f.p.Calls.begin() + idxOpen, f.p.Calls.end());
    CHECK(std::find(tail.begin(), tail.end(), "volume 35") != tail.end());
    CHECK(std::find(tail.begin(), tail.end(), "mute True") != tail.end());
    CHECK_EQ(f.c->Volume(), 35);
    f.c->SetVolume(250);
    CHECK_EQ(f.c->Volume(), 100);
}

TEST_CASE(PC_FiltersArePassedOnEveryOpenAndChangesAreForwarded) {
    Fixture f(3);
    std::string vf = "bwdif", af = "dynaudnorm";
    FilterProvider fp;
    fp.planOptions = [&] { return std::optional<OpenOptions>(vf.empty() ? OpenOptions{} : OpenOptions{ { "vf", vf } }); };
    fp.audioFilter = [&] { return af; };
    f.c->SetFilters(fp);
    f.c->SetBaseOption("hwaccel", "d3d11va");
    f.c->Play();
    CHECK_EQ(OptGet(f.p.Opened[0].options, "vf"), vf);
    CHECK_EQ(OptGet(f.p.Opened[0].options, "af"), std::string("dynaudnorm"));
    CHECK_EQ(OptGet(f.p.Opened[0].options, "hwaccel"), std::string("d3d11va"));

    f.c->ApplyFilterChange(FilterChange{});
    bool anyVf = false;
    for (auto& c : f.p.Calls) if (c.rfind("vf ", 0) == 0) anyVf = true;
    CHECK(!anyVf);

    vf = "bwdif,setpts=PTS/2";
    af = "dynaudnorm,atempo=2";
    FilterChange structural;
    structural.videoRebuild = structural.audioRebuild = true;
    structural.videoFilter = vf; structural.audioFilter = af;
    f.c->ApplyFilterChange(structural);
    CHECK_EQ(f.p.LastVf, vf);
    CHECK_EQ(f.p.LastAf, std::string("dynaudnorm,atempo=2"));
    f.c->Next();
    CHECK_EQ(OptGet(f.p.Opened[1].options, "vf"), vf);

    vf.clear(); af.clear();
    FilterChange reset;
    reset.videoRebuild = reset.audioRebuild = true;
    f.c->ApplyFilterChange(reset);
    CHECK_EQ(f.p.LastVf, std::string(""));
    f.c->Next();
    bool foundVf = true, foundAf = true;
    OptGet(f.p.Opened[2].options, "vf", &foundVf);
    OptGet(f.p.Opened[2].options, "af", &foundAf);
    CHECK(!foundVf);
    CHECK(!foundAf);
}

TEST_CASE(PC_EventsAndQueueEditing) {
    Fixture f(3);
    std::vector<std::pair<std::string, int>> cur;
    int queueChanges = 0;
    f.c->CurrentChanged.add([&](const MediaItem* i, int idx) { cur.emplace_back(i ? i->url : "", idx); });
    f.c->QueueChanged.add([&] { queueChanges++; });

    f.c->PlayAt(1);
    f.c->Next();
    REQUIRE(cur.size() == 2);
    CHECK(cur[0] == (std::pair<std::string, int>{"m2.mp4", 1}));
    CHECK(cur[1] == (std::pair<std::string, int>{"m3.mp4", 2}));

    f.c->RemoveAt(0);
    CHECK_EQ(f.c->Current()->url, std::string("m3.mp4"));
    CHECK_EQ(f.c->CurrentIndex(), 1);
    CHECK_EQ(queueChanges, 1);

    f.c->RemoveAt(1);
    CHECK_EQ(f.p.Calls.back(), std::string("stop"));
    CHECK_EQ(queueChanges, 2);

    f.c->Clear();
    CHECK(f.c->Current() == nullptr);
    CHECK(f.c->Items().empty());

    bool threw = false;
    try { f.c->RemoveAt(5); } catch (const std::out_of_range&) { threw = true; }
    CHECK(threw);
}

TEST_CASE(PC_SetQueueAndPlayAtApplyResumeOffsets) {
    FakePlayer p;
    PlaybackController c(p);
    c.SetFilters(Plain());
    c.SetQueue(Items(2), 1, true, 12.5);
    REQUIRE(p.Opened.size() == 1);
    CHECK_EQ(p.Opened[0].url, "m2.mp4");
    CHECK(OptGet(p.Opened[0].options, "ss").rfind("12.5", 0) == 0);
    CHECK(c.PlayAt(0, 20.0));
    REQUIRE(p.Opened.size() == 2);
    CHECK_EQ(p.Opened[1].url, "m1.mp4");
    CHECK(OptGet(p.Opened[1].options, "ss").rfind("20", 0) == 0);
}

TEST_CASE(PC_ResumeProviderResolvesEveryNewOpenPath) {
    FakePlayer p;
    PlaybackController c(p);
    c.SetFilters(Plain());
    std::vector<MediaItem> items = {
        MediaItem("resume.mp4"), MediaItem("played.mp4"), MediaItem("live.ts"), MediaItem("near-end.mp4")};
    std::vector<std::pair<std::string, double>> requests;
    c.resolveResumePosition = [&](const MediaItem& item, double requested) {
        requests.emplace_back(item.url, requested);
        PlaylistStore::ItemPlaybackState state;
        if (item.url == "resume.mp4") state = {42, 120, false, true};
        else if (item.url == "played.mp4") state = {70, 120, true, true};
        else if (item.url == "live.ts") state = {60, 120, false, false};
        else state = {118, 120, false, true};
        if (requested >= 0) state.position = requested;  // playlist-resume preferred position
        return PlaylistStore::ResumePosition(state);
    };

    c.SetQueue(items, 0, true);
    REQUIRE(p.Opened.size() == 1);
    CHECK_EQ(OptGet(p.Opened[0].options, "ss"), "42.000000");
    CHECK(c.Next());
    REQUIRE(p.Opened.size() == 2);
    bool found = false;
    OptGet(p.Opened[1].options, "ss", &found);
    CHECK(!found);
    CHECK(c.Next());
    REQUIRE(p.Opened.size() == 3);
    OptGet(p.Opened[2].options, "ss", &found);
    CHECK(!found);
    CHECK(c.Next());                            // a resume point within 5 s of the end is ignored
    REQUIRE(p.Opened.size() == 4);
    OptGet(p.Opened[3].options, "ss", &found);
    CHECK(!found);

    c.SetRepeat(RepeatMode::All);
    CHECK(c.Next());
    CHECK_EQ(p.Opened.back().url, "resume.mp4");
    CHECK_EQ(OptGet(p.Opened.back().options, "ss"), "42.000000");
    p.RaiseEnded();
    CHECK_EQ(p.Opened.back().url, "played.mp4");
    OptGet(p.Opened.back().options, "ss", &found);
    CHECK(!found);

    CHECK(c.Previous());
    CHECK_EQ(p.Opened.back().url, "resume.mp4");
    CHECK_EQ(OptGet(p.Opened.back().options, "ss"), "42.000000");
    CHECK(c.PlayAt(0, 80));
    CHECK_EQ(OptGet(p.Opened.back().options, "ss"), "80.000000");
    const size_t opensBeforeReopen = p.Opened.size();
    CHECK(c.ReopenCurrent(77, false));          // settings reopen preserves the live position, not resume history
    CHECK_EQ(p.Opened.size(), opensBeforeReopen + 1);
    CHECK_EQ(OptGet(p.Opened.back().options, "ss"), "77.000000");
    CHECK_EQ(requests.size(), (size_t)8);

    c.SetShuffle(true);
    c.SetRepeat(RepeatMode::All);
    for (int i = 0; i < 4; ++i) {
        const size_t before = requests.size();
        CHECK(c.Next());
        CHECK_EQ(requests.size(), before + 1);
        CHECK_EQ(p.Opened.back().url, c.Current()->url);
    }
}

TEST_CASE(PC_BeforeLeavingCurrentSavesBeforeTransportAndQueueChanges) {
    Fixture f(3);
    f.c->SetQueue(Items(3), 0, true);
    f.p.Calls.clear();
    std::vector<std::string> saved;
    f.c->beforeLeavingCurrent = [&] {
        REQUIRE(f.c->Current() != nullptr);
        saved.push_back(f.c->Current()->url);
        f.p.Calls.push_back("save " + f.c->Current()->url);
    };
    auto checkSaveBeforeStopOpen = [&](const std::string& oldUrl, const std::string& newUrl) {
        int saveAt = f.p.LastIndexOf("save " + oldUrl);
        int stopAt = f.p.LastIndexOf("stop");
        int openAt = f.p.LastIndexOf("open " + newUrl);
        CHECK(saveAt >= 0 && stopAt > saveAt && openAt > stopAt);
    };
    CHECK(f.c->Next());
    CHECK_EQ(saved.back(), "m1.mp4");
    checkSaveBeforeStopOpen("m1.mp4", "m2.mp4");
    f.p.Calls.clear();
    CHECK(f.c->Previous());
    CHECK_EQ(saved.back(), "m2.mp4");
    checkSaveBeforeStopOpen("m2.mp4", "m1.mp4");
    f.p.Calls.clear();
    f.p.RaiseEnded();
    CHECK_EQ(saved.back(), "m1.mp4");
    checkSaveBeforeStopOpen("m1.mp4", "m2.mp4");

    f.p.Calls.clear();
    f.c->Stop();
    CHECK_EQ(saved.back(), "m2.mp4");
    CHECK_EQ(f.p.Calls.front(), "save m2.mp4");
    CHECK_EQ(f.p.Calls.back(), "stop");
    f.p.Calls.clear();
    f.c->SetQueue(Items(3), 1, false);
    CHECK_EQ(saved.back(), "m2.mp4");
    f.p.Calls.clear();
    f.c->RemoveAt(f.c->CurrentIndex());
    CHECK_EQ(saved.back(), "m2.mp4");
    f.p.Calls.clear();
    f.c->Clear();
    CHECK_EQ(saved.back(), "m3.mp4");
}

TEST_CASE(PC_SetQueueAutoplaysStartItemAndDisposeUnhooks) {
    int disposed = 0;
    auto owned = std::make_unique<FakePlayer>();
    FakePlayer* p = owned.get();
    p->disposedCounter = &disposed;
    auto c = std::make_unique<PlaybackController>(std::move(owned));
    c->SetFilters(Plain());
    c->SetQueue(Items(3), 1);
    CHECK(p->OpenedUrls() == (Urls{"m2.mp4"}));
    CHECK_EQ(disposed, 0);
    c.reset();
    CHECK_EQ(disposed, 1);
}

TEST_CASE(PC_DisposeStopsAutoAdvance) {
    FakePlayer p;
    PlaybackController c(p);
    c.SetFilters(Plain());
    c.SetQueue(Items(3), 1);
    c.Dispose();
    p.RaiseEnded();
    CHECK_EQ(p.Opened.size(), (size_t)1);
}

TEST_CASE(PC_PlayOnEmptyQueueAndStaleEndedAfterStop) {
    FakePlayer p;
    PlaybackController c(p);
    c.SetFilters(Plain());
    c.Play();
    CHECK(p.Opened.empty());

    c.Enqueue(Items(2));
    c.Play();
    c.Stop();
    // delivered synchronously, advance is still legal: just assert consistency
    p.RaiseEnded();
    CHECK(c.Current() != nullptr);
}

TEST_CASE(PC_MoveKeepsCurrentAndDoesNotReopen) {
    Fixture f(5);
    f.c->PlayAt(1);
    size_t calls = f.p.Calls.size();
    int queueChanged = 0, currentChanged = 0;
    f.c->QueueChanged.add([&] { queueChanged++; });
    f.c->CurrentChanged.add([&](const MediaItem*, int) { currentChanged++; });
    f.c->Move({0, 1}, 4);
    CHECK(UrlsOf(f.c->Items()) == (Urls{"m3.mp4", "m4.mp4", "m1.mp4", "m2.mp4", "m5.mp4"}));
    REQUIRE(f.c->Current() != nullptr);
    CHECK_EQ(f.c->Current()->url, std::string("m2.mp4"));
    CHECK_EQ(f.c->CurrentIndex(), 3);
    CHECK_EQ(f.p.Calls.size(), calls);
    CHECK_EQ(queueChanged, 1);
    CHECK_EQ(currentChanged, 0);
    CHECK(f.c->Next());
    CHECK_EQ(f.c->Current()->url, std::string("m5.mp4"));
    f.c->Move({4}, 4);
    CHECK_EQ(queueChanged, 1);
    f.c->Move({4}, 0);
    CHECK_EQ(f.c->Items()[0].url, std::string("m5.mp4"));
    CHECK_EQ(f.c->Current()->url, std::string("m5.mp4"));
}

TEST_CASE(PC_MoveWithShuffleKeepsCurrentAndOrder) {
    Fixture f(6, 7);
    f.c->PlayAt(2);
    f.c->SetShuffle(true);
    std::string cur = f.c->Current()->url;
    Urls orderBefore = UrlsOf(f.c->PlayOrder());
    size_t calls = f.p.Calls.size();
    f.c->Move({5}, 0);
    CHECK_EQ(f.c->Current()->url, cur);
    CHECK(UrlsOf(f.c->PlayOrder()) == orderBefore);
    CHECK_EQ(f.c->Items()[0].url, std::string("m6.mp4"));
    CHECK_EQ(f.p.Calls.size(), calls);
}

TEST_CASE(PC_OnOpeningHookSeesFinalOptionsBeforeOpen) {
    Fixture f(2);
    f.c->SetBaseOption("hwaccel", "d3d11va");
    FilterProvider fp;
    fp.planOptions = [] { return std::optional<OpenOptions>(OpenOptions{ { "vf", "yadif" } }); };
    fp.audioFilter = [] { return std::string("atempo=1.5"); };
    f.c->SetFilters(fp);
    std::vector<std::pair<std::string, OpenOptions>> seen;
    size_t openedAtCall = 99;
    f.c->onOpening = [&](const std::string& url, const OpenOptions& o) { openedAtCall = f.p.Opened.size(); seen.emplace_back(url, o); };
    f.c->Play();
    CHECK_EQ(seen.size(), (size_t)1);
    CHECK_EQ(openedAtCall, (size_t)0);
    CHECK_EQ(seen[0].first, std::string("m1.mp4"));
    CHECK_EQ(OptGet(seen[0].second, "hwaccel"), std::string("d3d11va"));
    CHECK_EQ(OptGet(seen[0].second, "vf"), std::string("yadif"));
    CHECK_EQ(OptGet(seen[0].second, "af"), std::string("atempo=1.5"));
    CHECK(seen[0].second == f.p.Opened[0].options);
    f.c->Next();
    CHECK_EQ(seen.size(), (size_t)2);
    FilterChange fc;
    fc.videoRebuild = true;
    fc.videoFilter = "bwdif";
    const FilterChange* got = nullptr;
    f.c->onFilterChange = [&](const FilterChange& c) { got = &c; };
    f.c->ApplyFilterChange(fc);
    CHECK(got == &fc);
}


TEST_CASE(PC_ReopenCurrentReopensAtPositionAndRestoresPause) {
    Fixture f(2);
    f.c->Play();
    CHECK(f.c->ReopenCurrent(12.5, true));
    REQUIRE_MSG(f.p.Opened.size() == 2, "reopened");
    CHECK_EQ(f.p.Opened[1].url, std::string("m1.mp4"));
    CHECK(OptGet(f.p.Opened[1].options, "ss").rfind("12.5", 0) == 0);
    bool found = true;
    OptGet(f.p.Opened[1].options, "hwaccel", &found);
    CHECK(!found);
    CHECK(!f.p.HasCall("pause"));            // not before the new stream is prepared
    f.p.Prepared.raise(MediaPreparedInfo{});
    CHECK_EQ(OptGet(f.p.Opened[1].options, "start_paused"), std::string("1"));
    CHECK(f.p.HasCall("pause"));             // a player that ignores start_paused is paused at Prepared
    CHECK(f.p.HasCall("volume 100"));
}

TEST_CASE(PC_ReopenCurrentPausedOpensWithStartPaused) {
    Fixture f(1);
    f.p.honorsStartPaused = true;
    f.c->Play();
    bool found = true;
    OptGet(f.p.Opened[0].options, "start_paused", &found);
    CHECK(!found);                            // a normal open plays
    CHECK(f.c->ReopenCurrent(12.5, true));
    CHECK_EQ(OptGet(f.p.Opened[1].options, "start_paused"), std::string("1"));
    f.p.Prepared.raise(MediaPreparedInfo{});
    CHECK(!f.p.HasCall("pause"));            // libffplay already ended the open paused, with the landing frame shown
    CHECK(f.p.State() == PlayerState::Paused);
    // ReopenCurrentHere (settings / planner / GPU reopens) takes the same path while paused; the option is one-shot
    CHECK(f.c->ReopenCurrentHere());
    CHECK_EQ(OptGet(f.p.Opened[2].options, "start_paused"), std::string("1"));
    f.c->PlayAt(0);
    found = true;
    OptGet(f.p.Opened[3].options, "start_paused", &found);
    CHECK(!found);
}

TEST_CASE(PC_ReopenCurrentNotPausedDoesNotPauseAndNothingCurrentIsNoop) {
    Fixture f(1);
    CHECK(!f.c->ReopenCurrent(5, false));
    f.c->Play();
    CHECK(f.c->ReopenCurrent(5, false));
    f.p.Prepared.raise(MediaPreparedInfo{});
    CHECK(!f.p.HasCall("pause"));
    // a later open cancels a pending pause
    CHECK(f.c->ReopenCurrent(7, true));
    f.c->PlayAt(0);
    f.p.Prepared.raise(MediaPreparedInfo{});
    CHECK(!f.p.HasCall("pause"));
}

TEST_CASE(PC_ReorderRejectsNonPermutationAndKeepsCurrent) {
    Fixture f(4, 3);
    f.c->PlayAt(1);
    f.c->SetShuffle(true);
    Urls orderBefore = UrlsOf(f.c->PlayOrder());
    int queueChanged = 0;
    f.c->QueueChanged.add([&] { queueChanged++; });
    CHECK(!f.c->Reorder({0, 0, 1, 2}));
    CHECK(!f.c->Reorder({0, 1}));
    CHECK_EQ(queueChanged, 0);
    CHECK(f.c->Reorder({3, 2, 1, 0}));
    CHECK_EQ(queueChanged, 1);
    CHECK_EQ(f.c->Items()[0].url, std::string("m4.mp4"));
    CHECK_EQ(f.c->Current()->url, std::string("m2.mp4"));
    CHECK(UrlsOf(f.c->PlayOrder()) == orderBefore);
}

TEST_CASE(PC_RemoveManyKeepsOrRemovesCurrent) {
    Fixture f(6);
    f.c->PlayAt(4);
    int currentChanged = 0;
    f.c->CurrentChanged.add([&](const MediaItem*, int) { currentChanged++; });
    f.c->RemoveMany({0, 2, 2, 99});
    CHECK(UrlsOf(f.c->Items()) == (Urls{"m2.mp4", "m4.mp4", "m5.mp4", "m6.mp4"}));
    CHECK_EQ(f.c->Current()->url, std::string("m5.mp4"));
    CHECK_EQ(currentChanged, 0);
    f.c->RemoveMany({2, 1});   // removes the current item (m5) and m4
    CHECK(UrlsOf(f.c->Items()) == (Urls{"m2.mp4", "m6.mp4"}));
    CHECK_EQ(currentChanged, 1);
    CHECK_EQ(f.c->Current()->url, std::string("m6.mp4"));
}

TEST_CASE(PC_MoveSelectionOps) {
    Fixture f(5);
    f.c->PlayAt(3);
    CHECK((f.c->MoveSelection({3, 4}, MoveOp::Top) == std::vector<int>{0, 1}));
    CHECK(UrlsOf(f.c->Items()) == (Urls{"m4.mp4", "m5.mp4", "m1.mp4", "m2.mp4", "m3.mp4"}));
    CHECK_EQ(f.c->Current()->url, std::string("m4.mp4"));
    CHECK((f.c->MoveSelection({0, 1}, MoveOp::Up) == std::vector<int>{0, 1}));   // already at the top
    CHECK((f.c->MoveSelection({0, 3}, MoveOp::Down) == std::vector<int>{1, 4}));
    CHECK(UrlsOf(f.c->Items()) == (Urls{"m5.mp4", "m4.mp4", "m1.mp4", "m3.mp4", "m2.mp4"}));
    CHECK((f.c->MoveSelection({1}, MoveOp::Bottom) == std::vector<int>{4}));
    CHECK_EQ(f.c->Items()[4].url, std::string("m4.mp4"));
    CHECK_EQ(f.c->Current()->url, std::string("m4.mp4"));
}
