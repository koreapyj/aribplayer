#include "RebindLogic.h"

#include <algorithm>

namespace app {

const char* RebindReasonName(RebindReason r) {
    switch (r) {
    case RebindReason::DeviceRemoved: return "device_removed";
    case RebindReason::DeviceReset: return "device_reset";
    case RebindReason::RecreateTarget: return "recreate_target";
    case RebindReason::Test: return "test";
    case RebindReason::AdapterChange: return "adapter_change";
    }
    return "unknown";
}

void RebindController::Prune(uint64_t nowMs) {
    attempts_.erase(std::remove_if(attempts_.begin(), attempts_.end(),
                                   [&](uint64_t t) { return nowMs >= t && nowMs - t >= policy_.windowMs; }),
                    attempts_.end());
}

int RebindController::AttemptsInWindow(uint64_t nowMs) const {
    int n = 0;
    for (uint64_t t : attempts_) if (!(nowMs >= t && nowMs - t >= policy_.windowMs)) n++;
    return n;
}

bool RebindController::ReportLoss(RebindReason reason) {
    if (fallback_ || gaveUp_) return false;
    if (state_ != State::Idle) return false;
    state_ = State::Pending;
    reason_ = reason;
    incidents_++;
    return true;
}

RebindAction RebindController::Next(uint64_t nowMs) {
    if (state_ != State::Pending) return RebindAction::None;
    Prune(nowMs);
    state_ = State::Rebinding;
    if ((int)attempts_.size() >= policy_.maxAttempts) {
        inFallback_ = true;
        return RebindAction::Fallback;
    }
    inFallback_ = false;
    attempts_.push_back(nowMs);
    return RebindAction::Rebind;
}

void RebindController::OnRebindResult(bool ok, uint64_t nowMs) {
    (void)nowMs;
    if (state_ != State::Rebinding) return;
    if (inFallback_) {
        state_ = State::Idle;
        inFallback_ = false;
        if (ok) fallback_ = true; else gaveUp_ = true;
        noticePending_ = true;
        notice_.reason = reason_;
        notice_.kind = ok ? RebindNotice::Kind::Fallback : RebindNotice::Kind::Failed;
        notice_.text = ok ? "Graphics device keeps failing - using software rendering and playback"
                          : "Graphics device lost and could not be recovered - restart the app";
        return;
    }
    if (!ok) { state_ = State::Pending; return; }   // retried by the next Next(); the attempt budget bounds it
    state_ = State::Idle;
    noticePending_ = true;
    notice_.reason = reason_;
    notice_.kind = RebindNotice::Kind::Recovered;
    notice_.text = reason_ == RebindReason::AdapterChange ? "Moved to another graphics adapter" : "Graphics device reset";
}

bool RebindController::TakeNotice(RebindNotice& out) {
    if (!noticePending_) return false;
    noticePending_ = false;
    out = notice_;
    return true;
}

}
