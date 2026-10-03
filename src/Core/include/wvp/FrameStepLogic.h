#pragma once
// Input model for held-key stepping: frame step (D previous / F next) and keyframe seek (Left / Right). One object
// serves both, because libffplay runs one such request at a time (FFP_EBUSY otherwise) and completes both with a
// done event that the host routes to StepDone(). The action (frame / keyframe) is an opaque id handed to the player
// hook. Pure C++ (UI thread only): the player call, the physical key state and the clock are injected, so the host
// and the tests drive the same object.
//
//   - No step queue. One step is in flight at a time; nothing is submitted while one is.
//   - A fresh key press starts exactly one step. The first OS auto-repeat of that key (the key is really held)
//     arms the hold chain; the chain never runs on a plain tap, however fast the step completes.
//   - Auto-repeat events that arrive while a step is in flight are dropped. The chain advances only from StepDone:
//     when a step completes and the key is still physically held (hooks.held), the next step starts at once.
//   - Key up, focus loss, another command, a direction change or the end of the media ends the chain. The step
//     already in flight cannot be recalled, so it finishes; nothing starts after it.
//   - A step that moved nothing (start / end of media, first / last keyframe) ends the chain until a fresh press.
//   - A fresh press of a key while a step is in flight is kept in one slot and starts when that step completes,
//     but only if that key is still held then. A press that was released in the meantime is dropped.
#include <cstdint>
#include <functional>

namespace wvp {

class FrameStepController {
public:
    struct Hooks {
        std::function<int(int action, int direction)> step;   // starts a step (+1 / -1); 0 = accepted, anything else = not started
        std::function<bool(int key)> held;        // is the key physically down right now (GetAsyncKeyState)
        std::function<uint64_t()> nowMs;
    };

    /// watchdogMs: a step that has not completed after this long is treated as lost (the next press may start again).
    explicit FrameStepController(Hooks hooks, uint64_t watchdogMs = 5000);

    /// key: the host's key id (virtual key); action: what to step (host defined); direction: +1 next, -1 previous;
    /// repeat: OS auto-repeat of a held key.
    void KeyDown(int key, int action, int direction, bool repeat);
    void KeyUp(int key);
    /// The player reported the step finished; moved is false when no frame was presented (start / end of media).
    void StepDone(bool moved);
    /// One step from a source that has no key (menu): dropped while one is in flight. Ends any hold chain.
    bool StepOnce(int action, int direction);
    /// Focus loss, any other command: ends the chain and the pending press; an in-flight step still completes.
    void Cancel();
    /// Media opened / stopped / replaced: also forgets the in-flight step.
    void Reset();

    bool InFlight() const { return inFlight_; }
    int DroppedRepeats() const { return droppedRepeats_; }
    int Rejected() const { return rejected_; }

private:
    bool Start(int action, int direction);
    void CheckWatchdog();

    Hooks hooks_;
    uint64_t watchdogMs_;
    bool inFlight_ = false;
    uint64_t startedMs_ = 0;
    int chainKey_ = 0, chainAction_ = 0, chainDir_ = 0;
    bool armed_ = false;        // the OS auto-repeat has confirmed the key is held
    bool exhausted_ = false;    // the last step presented nothing: no more until a fresh press
    bool pending_ = false;
    int pendingKey_ = 0, pendingAction_ = 0, pendingDir_ = 0;
    int droppedRepeats_ = 0, rejected_ = 0;
};

}
