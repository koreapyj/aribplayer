#include "wvp/FrameStepLogic.h"

#include <utility>

namespace wvp {

FrameStepController::FrameStepController(Hooks hooks, uint64_t watchdogMs)
    : hooks_(std::move(hooks)), watchdogMs_(watchdogMs) {}

bool FrameStepController::Start(int action, int direction) {
    if (hooks_.step && hooks_.step(action, direction) == 0) {
        inFlight_ = true;
        startedMs_ = hooks_.nowMs ? hooks_.nowMs() : 0;
        return true;
    }
    ++rejected_;
    return false;
}

void FrameStepController::CheckWatchdog() {
    if (inFlight_ && hooks_.nowMs && hooks_.nowMs() - startedMs_ > watchdogMs_) inFlight_ = false;
}

void FrameStepController::KeyDown(int key, int action, int direction, bool repeat) {
    if (!key || (direction != 1 && direction != -1)) return;
    CheckWatchdog();
    if (!repeat) {
        exhausted_ = false;
        chainKey_ = key; chainAction_ = action; chainDir_ = direction; armed_ = false;   // any earlier chain is over
        pending_ = false;
        if (inFlight_) {
            chainKey_ = 0;
            pending_ = true; pendingKey_ = key; pendingAction_ = action; pendingDir_ = direction;
            return;
        }
        Start(action, direction);
        return;
    }
    if (key != chainKey_ || exhausted_) return;   // repeat of a key whose chain was cancelled or ran out of frames
    armed_ = true;
    if (inFlight_) { ++droppedRepeats_; return; }
    Start(chainAction_, chainDir_);
}

void FrameStepController::KeyUp(int key) {
    if (key == chainKey_) { chainKey_ = 0; armed_ = false; }
    if (pending_ && pendingKey_ == key) pending_ = false;
}

void FrameStepController::StepDone(bool moved) {
    if (!inFlight_) return;
    inFlight_ = false;
    if (pending_) {
        pending_ = false;
        if (hooks_.held && hooks_.held(pendingKey_)) {
            chainKey_ = pendingKey_; chainAction_ = pendingAction_; chainDir_ = pendingDir_; armed_ = false; exhausted_ = false;
            Start(pendingAction_, pendingDir_);
            return;
        }
    }
    if (!moved) exhausted_ = true;
    if (chainKey_ && armed_ && !exhausted_) {
        if (hooks_.held && hooks_.held(chainKey_)) { Start(chainAction_, chainDir_); return; }
        chainKey_ = 0; armed_ = false;
    }
}

bool FrameStepController::StepOnce(int action, int direction) {
    Cancel();
    CheckWatchdog();
    if (inFlight_) return false;
    return Start(action, direction);
}

void FrameStepController::Cancel() {
    chainKey_ = 0; armed_ = false;
    pending_ = false;
}

void FrameStepController::Reset() {
    Cancel();
    inFlight_ = false;
    exhausted_ = false;
}

}
