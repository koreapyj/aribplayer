#include "wvp/PlaybackController.h"

#include <algorithm>
#include <iterator>
#include <set>
#include <stdexcept>

namespace wvp {

namespace {
void SetOption(OpenOptions& o, const std::string& k, const std::string& v) {
    for (auto& e : o) if (e.first == k) { e.second = v; return; }
    o.emplace_back(k, v);
}
}

PlaybackController::PlaybackController(IMediaPlayer& player, PostFn post, unsigned seed) : player_(&player) {
    Init(std::move(post), seed);
}

PlaybackController::PlaybackController(std::unique_ptr<IMediaPlayer> ownedPlayer, PostFn post, unsigned seed)
    : owned_(std::move(ownedPlayer)), player_(owned_.get()) {
    if (!player_) throw std::invalid_argument("player");
    Init(std::move(post), seed);
}

void PlaybackController::Init(PostFn post, unsigned seed) {
    post_ = std::move(post);
    rng_.seed(seed);
    volume_ = player_->Volume();
    muted_ = player_->IsMuted();
    endedToken_ = player_->Ended.add([this] { OnEnded(); });
}

PlaybackController::~PlaybackController() { Dispose(); }

void PlaybackController::Dispose() {
    if (disposed_) return;
    disposed_ = true;
    *alive_ = false;
    player_->Ended.remove(endedToken_);
    filters_ = {};
    owned_.reset();
}

int PlaybackController::Rand(int maxExclusive) {
    return maxExclusive <= 1 ? 0 : (int)(rng_() % (unsigned)maxExclusive);
}

void PlaybackController::SetBaseOption(const std::string& key, const std::string& value) { SetOption(BaseOptions, key, value); }

void PlaybackController::ApplyFilterChange(const FilterChange& c) {
    if (onFilterChange) onFilterChange(c);
    if (pending_ && (OpenPending() || pending_)) return;   // opened with the new plan, or still waiting for one
    if (c.reopen) {
        // planner: decoder / hwframes / filter device changed -> a live filter change cannot express it. Only an item
        // that is actually open is reopened (at its position, pause kept); otherwise the next open uses the new plan anyway.
        PlayerState st = player_->State();
        if (Current() && (st == PlayerState::Opening || st == PlayerState::Playing || st == PlayerState::Paused)) {
            ReopenCurrentHere();
            return;
        }
    }
    if (c.videoRebuild) player_->SetVideoFilter(c.videoFilter);
    if (c.audioRebuild) player_->SetAudioFilter(c.audioFilter);
}

std::vector<MediaItem> PlaybackController::PlayOrder() const {
    std::vector<MediaItem> r;
    r.reserve(order_.size());
    for (int i : order_) r.push_back(items_[i]);
    return r;
}

void PlaybackController::SetShuffle(bool v) {
    if (shuffle_ == v) return;
    shuffle_ = v;
    RebuildOrder(true);
    ModeChanged.raise();
}

void PlaybackController::SetRepeat(RepeatMode m) {
    if (repeat_ == m) return;
    repeat_ = m;
    ModeChanged.raise();
}

void PlaybackController::SetVolume(int v) {
    volume_ = std::clamp(v, 0, 100);
    player_->SetVolume(volume_);
}

void PlaybackController::SetMuted(bool m) {
    muted_ = m;
    player_->SetMuted(m);
}

void PlaybackController::Enqueue(const MediaItem& item) { Enqueue(std::vector<MediaItem>{item}); }

void PlaybackController::Enqueue(const std::vector<MediaItem>& items) {
    int before = (int)items_.size();
    items_.insert(items_.end(), items.begin(), items.end());
    if ((int)items_.size() == before) return;
    ExtendOrder(before);
    QueueChanged.raise();
}

void PlaybackController::SetQueue(const std::vector<MediaItem>& items, int startIndex, bool autoplay, double startAt) {
    generation_++;
    NotifyLeavingCurrent();
    items_ = items;
    pos_ = -1;
    RebuildOrder(false);
    if (startIndex >= 0 && startIndex < (int)items_.size()) {
        auto it = std::find(order_.begin(), order_.end(), startIndex);
        pos_ = (int)(it - order_.begin());
        if (shuffle_ && pos_ > 0) {
            order_.erase(order_.begin() + pos_);
            order_.insert(order_.begin(), startIndex);
            pos_ = 0;
        }
    }
    QueueChanged.raise();
    RaiseCurrentChanged();
    if (autoplay && Current()) OpenCurrent(startAt);
}

void PlaybackController::Clear() {
    generation_++;
    bool hadCurrent = pos_ >= 0;
    if (hadCurrent) NotifyLeavingCurrent();
    items_.clear();
    order_.clear();
    pos_ = -1;
    if (hadCurrent) player_->Stop();
    QueueChanged.raise();
    if (hadCurrent) CurrentChanged.raise(nullptr, -1);
}

void PlaybackController::RemoveAt(int index) {
    if (index < 0 || index >= (int)items_.size()) throw std::out_of_range("index");
    int cur = CurrentIndex();
    if (cur == index) NotifyLeavingCurrent();
    items_.erase(items_.begin() + index);
    order_.erase(std::remove(order_.begin(), order_.end(), index), order_.end());
    for (int& o : order_) if (o > index) o--;
    if (cur == index) {
        // current item removed: stop and keep position pointing at the following entry (not played)
        generation_++;
        player_->Stop();
        if (pos_ >= (int)order_.size()) pos_ = (int)order_.size() - 1;
        QueueChanged.raise();
        RaiseCurrentChanged();
    } else {
        if (cur < 0) pos_ = -1;
        else {
            int target = cur > index ? cur - 1 : cur;
            pos_ = (int)(std::find(order_.begin(), order_.end(), target) - order_.begin());
        }
        QueueChanged.raise();
    }
}

bool PlaybackController::PlayAt(int index, double startAt) {
    if (index < 0 || index >= (int)items_.size()) return false;
    auto it = std::find(order_.begin(), order_.end(), index);
    if (it == order_.end()) return false;
    if (CurrentIndex() != index) NotifyLeavingCurrent();
    pos_ = (int)(it - order_.begin());
    RaiseCurrentChanged();
    OpenCurrent(startAt);
    return true;
}

void PlaybackController::Play() {
    if (pos_ < 0) {
        if (order_.empty()) return;
        pos_ = 0;
        RaiseCurrentChanged();
        OpenCurrent();
        return;
    }
    PlayerState s = player_->State();
    if (s == PlayerState::Idle || s == PlayerState::Stopped || s == PlayerState::Ended || s == PlayerState::Error) OpenCurrent();
    else player_->Play();
}

void PlaybackController::Stop() {
    generation_++;
    NotifyLeavingCurrent();
    player_->Stop();
}

bool PlaybackController::Next() {
    int p = NextPosition(false);
    if (p < 0) return false;
    if (p != pos_) NotifyLeavingCurrent();
    pos_ = p;
    RaiseCurrentChanged();
    OpenCurrent();
    return true;
}

bool PlaybackController::Previous() {
    int p = PreviousPosition();
    if (p < 0) {
        if (pos_ >= 0) player_->Seek(0, false);
        return false;
    }
    if (p != pos_) NotifyLeavingCurrent();
    pos_ = p;
    RaiseCurrentChanged();
    OpenCurrent();
    return true;
}

int PlaybackController::NextPosition(bool autoAdvance) const {
    if (order_.empty()) return -1;
    if (pos_ < 0) return 0;
    if (autoAdvance && repeat_ == RepeatMode::One) return pos_;
    if (pos_ + 1 < (int)order_.size()) return pos_ + 1;
    return repeat_ == RepeatMode::All ? 0 : -1;
}

int PlaybackController::PreviousPosition() const {
    if (order_.empty() || pos_ < 0) return -1;
    if (pos_ > 0) return pos_ - 1;
    return repeat_ == RepeatMode::All ? (int)order_.size() - 1 : -1;
}

void PlaybackController::OnEnded() {
    if (disposed_) return;
    int gen = generation_;
    if (!post_) { AdvanceAfterEnd(gen); return; }
    std::weak_ptr<bool> weakAlive = alive_;
    post_([this, gen, weakAlive] {
        auto a = weakAlive.lock();
        if (!a || !*a) return;
        AdvanceAfterEnd(gen);
    });
}

void PlaybackController::AdvanceAfterEnd(int gen) {
    if (disposed_ || gen != generation_) return;
    int p = NextPosition(true);
    if (p < 0) { QueueEnded.raise(); return; }
    bool same = p == pos_;
    if (!same) NotifyLeavingCurrent();
    pos_ = p;
    if (!same) RaiseCurrentChanged();
    OpenCurrent();
}

bool PlaybackController::ReopenCurrent(double position, bool paused) {
    if (!Current()) return false;
    if (reopenToken_) { player_->Prepared.remove(reopenToken_); reopenToken_ = 0; }
    if (paused) {
        int gen = generation_ + 1;   // OpenCurrent bumps the generation; a later open/queue change cancels the pause
        auto alive = alive_;
        reopenToken_ = player_->Prepared.add([this, gen, alive](const MediaPreparedInfo&) {
            if (!*alive) return;
            if (reopenToken_) { auto t = reopenToken_; reopenToken_ = 0; player_->Prepared.remove(t); }
            if (gen == generation_ && player_->State() != PlayerState::Paused) player_->Pause();   // a player without start_paused
        });
    }
    openPaused_ = paused;
    OpenCurrent(position > 0.5 ? position : -1, false);
    return true;
}

bool PlaybackController::ReopenCurrentHere() {
    if (HasPendingOpen()) return OpenPending();   // never opened yet: its own start position and pause state apply
    double p = player_->Position();
    bool paused = player_->State() == PlayerState::Paused;
    return ReopenCurrent(p == p ? p : 0.0, paused);
}

void PlaybackController::NotifyLeavingCurrent() {
    if (Current() && beforeLeavingCurrent) beforeLeavingCurrent();
}

void PlaybackController::OpenCurrent(double startAt, bool applyResumeRule) {
    pending_.reset();
    const MediaItem* item = Current();
    if (!item) return;
    MediaItem copy = *item;                 // handlers may modify the queue
    generation_++;
    player_->Stop();                        // reuses the single native instance
    if (beforeOpen) beforeOpen(copy.url);
    if (applyResumeRule && resolveResumePosition) startAt = resolveResumePosition(copy, startAt);
    pending_ = PendingOpen{generation_, startAt, openPaused_};
    openPaused_ = false;
    OpenPending();
}

bool PlaybackController::OpenPending() {
    if (!pending_) return false;
    const MediaItem* item = Current();
    if (pending_->generation != generation_ || !item) {
        pending_.reset();
        return false;
    }
    std::optional<OpenOptions> plan = filters_.planOptions ? filters_.planOptions() : std::nullopt;
    if (!plan) return false;
    const PendingOpen p = *pending_;
    pending_.reset();
    MediaItem copy = *item;
    OpenOptions opts = BaseOptions;
    for (auto& e : *plan) SetOption(opts, e.first, e.second);
    std::string af = filters_.audioFilter ? filters_.audioFilter() : "";
    if (!af.empty()) SetOption(opts, "af", af);
    if (p.startAt > 0) SetOption(opts, "ss", std::to_string(p.startAt));
    // libffplay ends the open paused with the landing frame presented and the audio clock not yet started (a pause or a
    // frame step after it would let audio run while the first frame decodes, and play would then start out of sync)
    if (p.paused) SetOption(opts, "start_paused", "1");
    // libffplay keeps the instance's vf across opens: a plan without vf must clear the vf of a previous plan
    bool planVf = false;
    for (auto& e : opts) if (e.first == "vf") planVf = true;
    if (!planVf) player_->SetVideoFilter("");
    player_->SetVolume(volume_);
    player_->SetMuted(muted_);
    if (onOpening) onOpening(copy.url, opts);
    player_->Open(copy.url, opts);
    // re-assert after open as well: the native side persists them, a fake or other backend may not
    player_->SetVolume(volume_);
    player_->SetMuted(muted_);
    return true;
}

void PlaybackController::RebuildOrder(bool keepCurrent) {
    int cur = keepCurrent ? CurrentIndex() : -1;
    std::vector<int> idx(items_.size());
    for (size_t i = 0; i < idx.size(); ++i) idx[i] = (int)i;
    if (shuffle_) {
        // Fisher-Yates; current item (if any) goes first so it "stays current" and the rest follows.
        for (int i = (int)idx.size() - 1; i > 0; i--) {
            int j = Rand(i + 1);
            std::swap(idx[i], idx[j]);
        }
        if (cur >= 0) {
            idx.erase(std::remove(idx.begin(), idx.end(), cur), idx.end());
            idx.insert(idx.begin(), cur);
        }
    }
    order_ = std::move(idx);
    pos_ = cur >= 0 ? (int)(std::find(order_.begin(), order_.end(), cur) - order_.begin()) : -1;
}

void PlaybackController::ExtendOrder(int firstNew) {
    if (shuffle_) {
        // insert randomly into the not-yet-played tail so the played history stays stable
        for (int a = firstNew; a < (int)items_.size(); ++a) {
            int lo = pos_ + 1;
            int at = lo + Rand((int)order_.size() - lo + 1);
            order_.insert(order_.begin() + at, a);
        }
    } else {
        for (int a = firstNew; a < (int)items_.size(); ++a) order_.push_back(a);
    }
}

}

namespace wvp {

void PlaybackController::Move(const std::vector<int>& rowsIn, int insertBefore) {
    std::set<int> rowSet;
    for (int r : rowsIn)
        if (r >= 0 && r < (int)items_.size()) rowSet.insert(r);
    if (rowSet.empty()) return;
    insertBefore = std::clamp(insertBefore, 0, (int)items_.size());
    std::vector<int> rest, moved(rowSet.begin(), rowSet.end());
    for (int i = 0; i < (int)items_.size(); i++)
        if (!rowSet.count(i)) rest.push_back(i);
    int nBefore = (int)std::distance(rowSet.begin(), rowSet.lower_bound(insertBefore));
    int pos = std::clamp(insertBefore - nBefore, 0, (int)rest.size());
    std::vector<int> perm(rest.begin(), rest.begin() + pos);   // perm[new] = old
    perm.insert(perm.end(), moved.begin(), moved.end());
    perm.insert(perm.end(), rest.begin() + pos, rest.end());
    ApplyPermutation(perm);
}

void PlaybackController::ApplyPermutation(const std::vector<int>& perm) {
    bool identity = true;
    for (int i = 0; i < (int)perm.size(); i++)
        if (perm[i] != i) { identity = false; break; }
    if (identity) return;
    std::vector<int> inv(perm.size());   // inv[old] = new
    for (int n = 0; n < (int)perm.size(); n++) inv[(size_t)perm[(size_t)n]] = n;
    std::vector<MediaItem> ni;
    ni.reserve(items_.size());
    for (int o : perm) ni.push_back(std::move(items_[(size_t)o]));
    items_ = std::move(ni);
    if (shuffle_) {
        for (int& o : order_) o = inv[(size_t)o];   // keep the shuffled play order; pos_ still points at the current item
    } else {
        int cur = CurrentIndex();                    // linear order follows the new item order
        for (int i = 0; i < (int)order_.size(); i++) order_[(size_t)i] = i;
        if (cur >= 0) pos_ = inv[(size_t)cur];
    }
    QueueChanged.raise();
}

bool PlaybackController::Reorder(const std::vector<int>& newOrder) {
    if (newOrder.size() != items_.size()) return false;
    std::vector<bool> seen(newOrder.size(), false);
    for (int o : newOrder) {
        if (o < 0 || o >= (int)newOrder.size() || seen[(size_t)o]) return false;
        seen[(size_t)o] = true;
    }
    ApplyPermutation(newOrder);
    return true;
}

void PlaybackController::RemoveMany(std::vector<int> indices) {
    std::set<int> rm;
    for (int r : indices)
        if (r >= 0 && r < (int)items_.size()) rm.insert(r);
    if (rm.empty()) return;
    int cur = CurrentIndex();
    bool curRemoved = cur >= 0 && rm.count(cur);
    if (curRemoved) NotifyLeavingCurrent();
    std::vector<int> map(items_.size(), -1);   // old -> new, -1 = removed
    std::vector<MediaItem> ni;
    for (int i = 0; i < (int)items_.size(); i++) {
        if (rm.count(i)) continue;
        map[(size_t)i] = (int)ni.size();
        ni.push_back(std::move(items_[(size_t)i]));
    }
    items_ = std::move(ni);
    int survivorsBeforePos = 0;
    std::vector<int> no;
    for (int p = 0; p < (int)order_.size(); p++) {
        int m = map[(size_t)order_[(size_t)p]];
        if (m < 0) continue;
        if (p < pos_) survivorsBeforePos++;
        no.push_back(m);
    }
    order_ = std::move(no);
    if (curRemoved) {
        // like RemoveAt: stop, and keep position pointing at the entry that followed (not played)
        generation_++;
        player_->Stop();
        pos_ = std::min(survivorsBeforePos, (int)order_.size() - 1);
        QueueChanged.raise();
        RaiseCurrentChanged();
    } else {
        pos_ = cur < 0 ? -1 : (int)(std::find(order_.begin(), order_.end(), map[(size_t)cur]) - order_.begin());
        QueueChanged.raise();
    }
}

std::vector<int> PlaybackController::MoveSelection(const std::vector<int>& indices, MoveOp op) {
    int n = (int)items_.size();
    std::vector<bool> sel((size_t)n, false);
    for (int r : indices)
        if (r >= 0 && r < n) sel[(size_t)r] = true;
    std::vector<int> perm((size_t)n);   // perm[new] = old
    for (int i = 0; i < n; i++) perm[(size_t)i] = i;
    auto isSel = [&](int p) { return sel[(size_t)perm[(size_t)p]]; };
    switch (op) {
    case MoveOp::Top:
        std::stable_partition(perm.begin(), perm.end(), [&](int o) { return sel[(size_t)o]; });
        break;
    case MoveOp::Bottom:
        std::stable_partition(perm.begin(), perm.end(), [&](int o) { return !sel[(size_t)o]; });
        break;
    case MoveOp::Up:
        for (int p = 1; p < n; p++)
            if (isSel(p) && !isSel(p - 1)) std::swap(perm[(size_t)p], perm[(size_t)p - 1]);
        break;
    case MoveOp::Down:
        for (int p = n - 2; p >= 0; p--)
            if (isSel(p) && !isSel(p + 1)) std::swap(perm[(size_t)p], perm[(size_t)p + 1]);
        break;
    }
    ApplyPermutation(perm);
    std::vector<int> result;
    for (int p = 0; p < n; p++)
        if (isSel(p)) result.push_back(p);
    return result;
}

}
