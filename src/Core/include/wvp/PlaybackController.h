#pragma once
// Play queue on top of one IMediaPlayer with current index,
// next/previous, stable shuffle, repeat modes, auto-advance on Ended (guarded by a generation counter)
// and persistence of volume/mute/filters across items.
// Not thread-safe: use from the UI thread. When the player raises Ended on another thread, pass a `post`
// function so auto-advance (Stop/Open) is posted onto the owning thread.
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "Event.h"
#include "IMediaPlayer.h"
#include "MediaItem.h"

namespace wvp {

enum class RepeatMode { Off, All, One };

enum class MoveOp { Top, Up, Down, Bottom };

/// Supplies the vf/af strings applied on every Open.
struct FilterProvider {
    std::function<std::wstring()> videoFilter;       // may be empty function
    std::function<std::wstring()> audioFilter;
    /// Planner mode: the open options of the current plan (hwaccel, hwframes, presenter, vf, filter_hw_device).
    /// Empty function or empty result (and planActive false/empty) = legacy behaviour. Replaces videoFilter /
    /// the controller's hwDecoder for the video side; an explicit BaseOptions hwaccel still wins.
    std::function<OpenOptions()> planOptions;
    /// True when a plan is active even though it needs no options (software decode, no vf, sdl presenter).
    std::function<bool()> planActive;
};

struct FilterCommand { std::wstring target, command, argument; };

/// Forwarded live to the player via ApplyFilterChange().
struct FilterChange {
    bool videoRebuild = false, audioRebuild = false;
    bool reopen = false;                  // planner: decoder/presenter/hwframes/filter device changed: reopen the item (also sets videoRebuild)
    bool presenterParamsChanged = false;  // planner: presenter-applied parameters (rotation, flips, eq) changed
    PresenterParams presenterParams;      // the new parameters (valid when presenterParamsChanged and no reopen)
    std::wstring videoFilter, audioFilter;
    std::vector<FilterCommand> commands;
};

class PlaybackController {
public:
    using PostFn = std::function<void(std::function<void()>)>;

    /// player must outlive the controller unless ownedPlayer is used.
    explicit PlaybackController(IMediaPlayer& player, PostFn post = {}, unsigned randomSeed = std::random_device{}());
    /// Takes ownership of the player (destroyed with the controller / Dispose()).
    explicit PlaybackController(std::unique_ptr<IMediaPlayer> ownedPlayer, PostFn post = {}, unsigned randomSeed = std::random_device{}());
    ~PlaybackController();
    PlaybackController(const PlaybackController&) = delete;
    PlaybackController& operator=(const PlaybackController&) = delete;

    IMediaPlayer& Player() { return *player_; }

    /// Extra ffplay/AVFormat options passed on every Open (e.g. hwaccel, user_agent). vf/af come from the filter provider.
    OpenOptions BaseOptions;
    void SetBaseOption(const std::wstring& key, const std::wstring& value);

    /// First-class decoder choice, applied by every Open (so it cannot be lost to hook ordering): "auto" (default; libffplay tries
    /// d3d11va, d3d12va, cuda, dxva2, then software), a device type name ("d3d11va", "d3d12va", "cuda", "dxva2"), or "off"/"" for
    /// software decoding. An explicit `hwaccel` entry in BaseOptions wins over this property.
    void SetHwDecoder(const std::wstring& name) { hwDecoder_ = name; }
    const std::wstring& HwDecoder() const { return hwDecoder_; }
    /// The value that goes into the `hwaccel` option for a decoder choice, "" when software decoding is wanted.
    static std::wstring HwaccelOptionFor(const std::wstring& decoderChoice);

    /// Re-opens the current item at `position` seconds (ffplay `ss`). When `paused` it opens with start_paused=1 (the landing frame is
    /// presented, Position() is valid, audio starts only on play; Pause() at Prepared if the player is not paused). Volume/mute persist in
    /// the player; audio/subtitle track selection is NOT restored (the native API has no select-by-index call, only cycling).
    /// Returns false when nothing is current.
    bool ReopenCurrent(double position, bool paused);
    /// ReopenCurrent at the player's current Position()/paused state. Used when a setting that needs a reopen changes.
    bool ReopenCurrentHere();

    void SetFilters(FilterProvider p) { filters_ = std::move(p); }

    /// Diagnostics hook: called right before every player->Open with the url and the FINAL options (BaseOptions incl.
    /// hwaccel, vf/af from the filter provider, filter_hw_device, ...). Purely observational.
    std::function<void(const std::wstring& url, const OpenOptions& options)> onOpening;
    /// Called at the start of every OpenCurrent, before the options are built (so the host can re-plan for the new url).
    std::function<void(const std::wstring& url)> beforeOpen;
    /// Diagnostics hook: called at the start of ApplyFilterChange with the change being forwarded.
    std::function<void(const FilterChange& change)> onFilterChange;
    /// Called when the player refused live presenter parameters (SetPresenterParams returned `err` != 0): the host marks the
    /// absorption as impossible (FilterManager::PresenterParamsRejected) and applies the resulting change. Not called when the
    /// player has no open media (the parameters then simply ride on the next open).
    std::function<void(int err, const PresenterParams& rejected)> onPresenterParamsRejected;
    /// Resolves the start offset for every newly started item. requestedPosition is -1 when no caller preference was supplied.
    /// The host applies its resume policy here so transport, queue, and auto-advance opens all use the same decision.
    std::function<double(const MediaItem& item, double requestedPosition)> resolveResumePosition;
    /// Called before the current item is left, stopped, or removed, while Current() still refers to that item.
    /// Hosts use this to force-persist its last playback position before the player is stopped or the queue changes.
    std::function<void()> beforeLeavingCurrent;
    /// Forwards a live filter change (rebuild strings and commands) to the player.
    void ApplyFilterChange(const FilterChange& c);

    Event<> QueueChanged;
    Event<const MediaItem*, int> CurrentChanged;   // item (nullptr = none), index in Items (-1 = none)
    Event<> ModeChanged;                           // shuffle or repeat changed
    Event<> QueueEnded;                            // last item ended with Repeat Off

    const std::vector<MediaItem>& Items() const { return items_; }   // insertion order
    std::vector<MediaItem> PlayOrder() const;                        // effective play order (== Items when shuffle off)
    int CurrentIndex() const { return pos_ >= 0 && pos_ < (int)order_.size() ? order_[pos_] : -1; }
    const MediaItem* Current() const { int i = CurrentIndex(); return i >= 0 ? &items_[i] : nullptr; }
    bool HasNext() const { return NextPosition(false) >= 0; }
    bool HasPrevious() const { return PreviousPosition() >= 0; }

    bool Shuffle() const { return shuffle_; }
    void SetShuffle(bool v);
    RepeatMode Repeat() const { return repeat_; }
    void SetRepeat(RepeatMode m);
    int Volume() const { return volume_; }
    void SetVolume(int v);                          // clamped 0..100
    bool IsMuted() const { return muted_; }
    void SetMuted(bool m);

    void Enqueue(const MediaItem& item);
    void Enqueue(const std::vector<MediaItem>& items);
    /// Replaces the queue. startIndex < 0 keeps nothing current; autoplay opens the start item.
    void SetQueue(const std::vector<MediaItem>& items, int startIndex = 0, bool autoplay = true, double startAt = -1);
    void Clear();
    void RemoveAt(int index);                       // throws std::out_of_range
    /// Moves `rows` (indices into Items) as one block before original index insertBefore (0..Count). Playback is not
    /// interrupted; the current item stays current; the shuffle play order is preserved; raises QueueChanged only.
    void Move(const std::vector<int>& rows, int insertBefore);
    /// Applies a permutation: newOrder[newIndex] = oldIndex (e.g. from SortOrder). Same guarantees as Move (current item stays
    /// current, shuffle order preserved, QueueChanged only; nothing raised for the identity). False (nothing changed) when
    /// newOrder is not a permutation of 0..Count-1.
    bool Reorder(const std::vector<int>& newOrder);
    /// Removes several items (any order, duplicates and out-of-range entries ignored). Like RemoveAt when the current item is
    /// among them: stops, raises QueueChanged then CurrentChanged (current moves to the following entry, not played).
    void RemoveMany(std::vector<int> indices);
    /// Moves the selected items as a group per `op` (Up/Down keep a block that already touches the edge in place).
    /// Returns the selected items' new indices, ascending, for re-selecting them. Same guarantees as Move.
    std::vector<int> MoveSelection(const std::vector<int>& indices, MoveOp op);

    bool PlayAt(int index, double startAt = -1);    // index in Items order; optional resume offset in seconds
    void Play();                                    // plays current (opens first if none) or resumes
    void Pause() { player_->Pause(); }
    void TogglePause() { player_->TogglePause(); }
    void Stop();
    bool Next();                                    // wraps with Repeat All; false when none
    bool Previous();                                // at start with Repeat Off/One restarts the current item

    void Dispose();

private:
    void Init(PostFn post, unsigned seed);
    int NextPosition(bool autoAdvance) const;
    int PreviousPosition() const;
    void OnEnded();
    void AdvanceAfterEnd(int gen);
    void NotifyLeavingCurrent();
    void OpenCurrent(double startAt = -1, bool applyResumeRule = true);
    void RebuildOrder(bool keepCurrent);
    void ExtendOrder(int firstNew);
    void ApplyPermutation(const std::vector<int>& perm);   // perm[new] = old; no-op for the identity
    int Rand(int maxExclusive);
    void RaiseCurrentChanged() { const MediaItem* c = Current(); CurrentChanged.raise(c, CurrentIndex()); }

    std::unique_ptr<IMediaPlayer> owned_;
    IMediaPlayer* player_ = nullptr;
    PostFn post_;
    std::vector<MediaItem> items_;
    std::vector<int> order_;       // play order: indices into items_
    int pos_ = -1;                 // position within order_; -1 = nothing current
    bool shuffle_ = false;
    RepeatMode repeat_ = RepeatMode::Off;
    int volume_ = 100;
    bool muted_ = false;
    int generation_ = 0;           // invalidates posted auto-advances
    std::mt19937 rng_;
    bool disposed_ = false;
    FilterProvider filters_;
    std::wstring hwDecoder_ = L"auto";
    Event<const MediaPreparedInfo&>::Token reopenToken_ = 0;
    bool openPaused_ = false;   // the next OpenCurrent passes start_paused=1 (ReopenCurrent while paused)
    Event<>::Token endedToken_ = 0;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

}
