#pragma once
// Decision logic for "rebind the GPU-dependent objects" (device loss today; a window moved to another GPU's monitor later).
// Pure C++: the caller owns the clock (nowMs) and performs the rebuild. Flow:
//
//     ctl.ReportLoss(reason)                 coalesced while an incident is open
//     switch (ctl.Next(now)) {               Rebind / Fallback: do the rebuild now; None: nothing to do
//     }
//     ctl.OnRebindResult(ok, now)            ok ends the incident; !ok retries (Next) until the attempt budget is spent,
//                                            then Next() returns Fallback once, and after that losses are ignored
//     ctl.TakeNotice(n)                      exactly one user notice per finished incident
#include <cstdint>
#include <string>
#include <vector>

namespace app {

enum class RebindReason { DeviceRemoved, DeviceReset, RecreateTarget, Test, AdapterChange };
const char* RebindReasonName(RebindReason r);

enum class RebindAction { None, Rebind, Fallback };

struct RebindPolicy {
    int maxAttempts = 3;            // rebuild attempts allowed inside windowMs before the fallback
    uint64_t windowMs = 30000;
};

struct RebindNotice {
    enum class Kind { Recovered, Fallback, Failed } kind = Kind::Recovered;
    RebindReason reason = RebindReason::DeviceRemoved;
    std::string text;
};

class RebindController {
public:
    explicit RebindController(RebindPolicy p = {}) : policy_(p) {}

    /// Records a loss. Returns true when it opened a new incident; false when it was coalesced into the open one
    /// (pending or rebinding) or ignored (fallback already active / gave up).
    bool ReportLoss(RebindReason reason);
    /// What to do right now. Rebind and Fallback move the incident to "rebinding" (one at a time); the caller must answer
    /// with OnRebindResult. None when nothing is pending or a rebuild is in progress.
    RebindAction Next(uint64_t nowMs);
    void OnRebindResult(bool ok, uint64_t nowMs);

    bool Pending() const { return state_ == State::Pending; }
    bool Busy() const { return state_ != State::Idle; }
    /// The UI runs on WARP and playback must use software decoding + SDL until ResetFallback().
    bool FallbackActive() const { return fallback_; }
    /// Fallback itself failed: nothing more is attempted.
    bool GaveUp() const { return gaveUp_; }
    void ResetFallback() { fallback_ = false; gaveUp_ = false; attempts_.clear(); }

    RebindReason Reason() const { return reason_; }
    int Incidents() const { return incidents_; }
    int AttemptsInWindow(uint64_t nowMs) const;

    bool TakeNotice(RebindNotice& out);

private:
    enum class State { Idle, Pending, Rebinding };
    void Prune(uint64_t nowMs);

    RebindPolicy policy_;
    State state_ = State::Idle;
    bool inFallback_ = false;          // the rebuild in flight is the fallback one
    bool fallback_ = false, gaveUp_ = false;
    RebindReason reason_ = RebindReason::DeviceRemoved;
    std::vector<uint64_t> attempts_;   // times of Rebind attempts
    bool noticePending_ = false;
    RebindNotice notice_;
    int incidents_ = 0;
};

}
