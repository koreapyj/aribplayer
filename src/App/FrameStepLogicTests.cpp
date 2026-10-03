// --logictest cases for the held-key step input model (wvp/FrameStepLogic.h): D / F frame steps and Left / Right keyframe seeks.
#include "LogicTest.h"
#include "wvp/FrameStepLogic.h"

#include <set>
#include <vector>

namespace app {
namespace {

constexpr int kD = 'D', kF = 'F', kLeft = 0x25, kRight = 0x27;
constexpr int kFrame = 0, kKey = 1;

struct StepRig {
    std::set<int> down;            // keys physically held
    std::vector<int> issued;       // directions handed to the player
    std::vector<int> actions;      // the action of each, in the same order
    int rc = 0;                    // what the player answers to a step request
    uint64_t now = 1000;
    wvp::FrameStepController c;

    StepRig() : c(wvp::FrameStepController::Hooks{
        [this](int action, int dir) { if (rc == 0) { issued.push_back(dir); actions.push_back(action); } return rc; },
        [this](int key) { return down.count(key) != 0; },
        [this] { return now; }}) {}

    void Press(int key, int dir, int action = kFrame) { down.insert(key); c.KeyDown(key, action, dir, false); }
    void Repeat(int key, int dir, int action = kFrame) { c.KeyDown(key, action, dir, true); }
    void Release(int key) { down.erase(key); c.KeyUp(key); }
};

}

LOGIC_TEST(framestep_tap_is_exactly_one_step_even_if_still_down_at_completion) {
    StepRig r;
    r.Press(kF, +1);
    LT_CHECK_EQ((int)r.issued.size(), 1);
    LT_CHECK(r.c.InFlight());
    r.c.StepDone(true);                       // key still down, but the OS never reported a repeat
    LT_CHECK_EQ((int)r.issued.size(), 1);
    LT_CHECK(!r.c.InFlight());
    r.Release(kF);
    LT_CHECK_EQ((int)r.issued.size(), 1);
}

LOGIC_TEST(framestep_hold_chains_from_completion_while_the_key_is_down) {
    StepRig r;
    r.Press(kD, -1);
    r.Repeat(kD, -1);                         // OS repeat while the first step is still running: dropped, but arms the chain
    LT_CHECK_EQ((int)r.issued.size(), 1);
    LT_CHECK_EQ(r.c.DroppedRepeats(), 1);
    r.Repeat(kD, -1); r.Repeat(kD, -1);
    LT_CHECK_EQ((int)r.issued.size(), 1);     // never more than one in flight
    r.c.StepDone(true);
    LT_CHECK_EQ((int)r.issued.size(), 2);
    r.c.StepDone(true);
    LT_CHECK_EQ((int)r.issued.size(), 3);
    r.down.erase(kD);                         // released: the keyup message has not been processed yet
    r.c.StepDone(true);
    LT_CHECK_EQ((int)r.issued.size(), 3);     // the physical state wins over the late message
    LT_CHECK(!r.c.InFlight());
    for (int d : r.issued) LT_CHECK_EQ(d, -1);
}

LOGIC_TEST(framestep_repeat_after_an_already_finished_step_starts_the_chain) {
    StepRig r;
    r.Press(kF, +1);
    r.c.StepDone(true);                       // fast F: done long before the OS repeat delay
    LT_CHECK_EQ((int)r.issued.size(), 1);
    r.Repeat(kF, +1);
    LT_CHECK_EQ((int)r.issued.size(), 2);
    r.Repeat(kF, +1);                         // in flight: dropped
    LT_CHECK_EQ((int)r.issued.size(), 2);
    r.c.StepDone(true);
    LT_CHECK_EQ((int)r.issued.size(), 3);
}

LOGIC_TEST(framestep_key_up_mid_step_runs_nothing_after_the_inflight_step) {
    StepRig r;
    r.Press(kD, -1);
    r.Repeat(kD, -1);
    r.Release(kD);
    LT_CHECK(r.c.InFlight());                 // cannot be recalled
    r.c.StepDone(true);
    LT_CHECK_EQ((int)r.issued.size(), 1);
    LT_CHECK(!r.c.InFlight());
    r.Repeat(kD, -1);                         // stray repeat after the release
    LT_CHECK_EQ((int)r.issued.size(), 1);
}

LOGIC_TEST(framestep_direction_change_cancels_the_chain_and_the_new_key_starts_its_own) {
    StepRig r;
    r.Press(kD, -1); r.Repeat(kD, -1);        // D chain armed, step in flight
    r.Press(kF, +1);                          // F pressed while D still down and a step running
    LT_CHECK_EQ((int)r.issued.size(), 1);     // not queued behind: nothing new yet
    r.c.StepDone(true);
    LT_CHECK_EQ((int)r.issued.size(), 2);
    LT_CHECK_EQ(r.issued[1], +1);
    r.Repeat(kD, -1);                         // D's OS repeat has stopped being ours
    r.Repeat(kF, +1);                         // F held: arms the new chain
    r.c.StepDone(true);
    LT_CHECK_EQ((int)r.issued.size(), 3);
    LT_CHECK_EQ(r.issued[2], +1);
    r.Release(kF);
    r.c.StepDone(true);
    LT_CHECK_EQ((int)r.issued.size(), 3);
}

LOGIC_TEST(framestep_press_released_before_the_inflight_step_ends_is_dropped) {
    StepRig r;
    r.Press(kD, -1); r.Repeat(kD, -1);
    r.Press(kF, +1);
    r.Release(kF);
    r.c.StepDone(true);
    LT_CHECK_EQ((int)r.issued.size(), 1);
    LT_CHECK(!r.c.InFlight());
}

LOGIC_TEST(framestep_focus_loss_or_other_command_ends_the_chain) {
    StepRig r;
    r.Press(kF, +1); r.Repeat(kF, +1);
    r.c.Cancel();                             // focus lost / another command
    r.c.StepDone(true);                       // key still down as far as GetAsyncKeyState knows
    LT_CHECK_EQ((int)r.issued.size(), 1);
    r.Repeat(kF, +1);                         // later repeats of the abandoned press
    LT_CHECK_EQ((int)r.issued.size(), 1);
    r.Press(kF, +1);                          // a fresh press starts again
    LT_CHECK_EQ((int)r.issued.size(), 2);
}

LOGIC_TEST(framestep_no_frame_moved_stops_the_chain_until_a_fresh_press) {
    StepRig r;
    r.Press(kF, +1); r.Repeat(kF, +1);
    r.c.StepDone(true);
    LT_CHECK_EQ((int)r.issued.size(), 2);
    r.c.StepDone(false);                      // end of media
    LT_CHECK_EQ((int)r.issued.size(), 2);
    r.Repeat(kF, +1); r.Repeat(kF, +1);
    LT_CHECK_EQ((int)r.issued.size(), 2);
    r.Release(kF); r.Press(kD, -1);
    LT_CHECK_EQ((int)r.issued.size(), 3);
}

LOGIC_TEST(framestep_rejected_step_is_not_in_flight_and_ends_the_chain) {
    StepRig r;
    r.rc = -16;                               // FFP_EBUSY
    r.Press(kF, +1);
    LT_CHECK(!r.c.InFlight());
    LT_CHECK_EQ(r.c.Rejected(), 1);
    LT_CHECK_EQ((int)r.issued.size(), 0);
    r.rc = 0;
    r.Press(kF, +1);
    LT_CHECK(r.c.InFlight());
}

LOGIC_TEST(framestep_lost_step_is_forgotten_after_the_watchdog) {
    StepRig r;
    r.Press(kF, +1);
    r.Release(kF);
    r.now += 1000;
    r.Press(kF, +1);                          // still "in flight": kept as the pending press, held at completion
    LT_CHECK_EQ((int)r.issued.size(), 1);
    r.Release(kF);
    r.now += 6000;
    r.Press(kF, +1);                          // the step never completed: start anew
    LT_CHECK_EQ((int)r.issued.size(), 2);
}

LOGIC_TEST(framestep_menu_step_is_one_shot_and_dropped_while_busy) {
    StepRig r;
    LT_CHECK(r.c.StepOnce(kFrame, +1));
    LT_CHECK(!r.c.StepOnce(kFrame, +1));
    LT_CHECK_EQ((int)r.issued.size(), 1);
    r.c.StepDone(true);
    LT_CHECK_EQ((int)r.issued.size(), 1);     // no chain
    LT_CHECK(r.c.StepOnce(kFrame, -1));
    LT_CHECK_EQ((int)r.issued.size(), 2);
}

LOGIC_TEST(framestep_reset_forgets_the_inflight_step) {
    StepRig r;
    r.Press(kF, +1); r.Repeat(kF, +1);
    r.c.Reset();                              // new media
    LT_CHECK(!r.c.InFlight());
    r.c.StepDone(true);                       // stale completion of the old media
    LT_CHECK_EQ((int)r.issued.size(), 1);
    r.Press(kF, +1);
    LT_CHECK_EQ((int)r.issued.size(), 2);
}

LOGIC_TEST(keyseek_tap_is_one_keyframe_seek_and_carries_its_action) {
    StepRig r;
    r.Press(kRight, +1, kKey);
    LT_CHECK_EQ((int)r.issued.size(), 1);
    LT_CHECK_EQ(r.actions[0], kKey);
    LT_CHECK_EQ(r.issued[0], +1);
    r.c.StepDone(true);                       // key still down, no OS repeat seen: a tap never chains
    LT_CHECK_EQ((int)r.issued.size(), 1);
    r.Release(kRight);
    LT_CHECK_EQ((int)r.issued.size(), 1);
}

LOGIC_TEST(keyseek_hold_walks_one_keyframe_per_completion_and_drops_repeats) {
    StepRig r;
    r.Press(kLeft, -1, kKey);
    r.Repeat(kLeft, -1, kKey); r.Repeat(kLeft, -1, kKey); r.Repeat(kLeft, -1, kKey);
    LT_CHECK_EQ((int)r.issued.size(), 1);     // repeats while a seek is in flight are dropped, not queued
    LT_CHECK_EQ(r.c.DroppedRepeats(), 3);
    r.c.StepDone(true);
    LT_CHECK_EQ((int)r.issued.size(), 2);
    r.Repeat(kLeft, -1, kKey);
    LT_CHECK_EQ((int)r.issued.size(), 2);
    r.c.StepDone(true);
    LT_CHECK_EQ((int)r.issued.size(), 3);
    for (size_t i = 0; i < r.issued.size(); ++i) { LT_CHECK_EQ(r.issued[i], -1); LT_CHECK_EQ(r.actions[i], kKey); }
}

LOGIC_TEST(keyseek_release_leaves_nothing_queued) {
    StepRig r;
    r.Press(kRight, +1, kKey); r.Repeat(kRight, +1, kKey); r.Repeat(kRight, +1, kKey);
    r.Release(kRight);                        // key up while the first seek is still running
    r.c.StepDone(true);
    LT_CHECK_EQ((int)r.issued.size(), 1);
    LT_CHECK(!r.c.InFlight());
    r.Repeat(kRight, +1, kKey);               // a late repeat of the released key
    LT_CHECK_EQ((int)r.issued.size(), 1);
    r.Press(kRight, +1, kKey);
    r.Repeat(kRight, +1, kKey);
    r.down.erase(kRight);                     // physically released, keyup message not processed yet
    r.c.StepDone(true);
    LT_CHECK_EQ((int)r.issued.size(), 2);
}

LOGIC_TEST(keyseek_last_keyframe_ends_the_chain_until_a_fresh_press) {
    StepRig r;
    r.Press(kRight, +1, kKey); r.Repeat(kRight, +1, kKey);
    r.c.StepDone(true);
    LT_CHECK_EQ((int)r.issued.size(), 2);
    r.c.StepDone(false);                      // already on the last keyframe: moved nothing
    r.Repeat(kRight, +1, kKey); r.Repeat(kRight, +1, kKey);
    LT_CHECK_EQ((int)r.issued.size(), 2);
    r.Release(kRight); r.Press(kLeft, -1, kKey);
    LT_CHECK_EQ((int)r.issued.size(), 3);
}

LOGIC_TEST(keyseek_and_framestep_share_one_inflight_slot) {
    StepRig r;
    r.Press(kRight, +1, kKey);
    r.Press(kF, +1, kFrame);                  // fresh press while the keyframe seek runs: one pending slot
    LT_CHECK_EQ((int)r.issued.size(), 1);
    r.c.StepDone(true);                       // F still down: it starts now, as a frame step
    LT_CHECK_EQ((int)r.issued.size(), 2);
    LT_CHECK_EQ(r.actions[1], kFrame);
    r.c.StepDone(true);
    r.down.erase(kF);
    LT_CHECK_EQ((int)r.issued.size(), 2);
    LT_CHECK(r.c.StepOnce(kKey, -1));         // menu / command path with an action
    LT_CHECK(!r.c.StepOnce(kFrame, +1));
    LT_CHECK_EQ(r.actions.back(), kKey);
}

LOGIC_TEST(timedskip_hold_chains_one_skip_per_completion_and_release_leaves_nothing) {
    constexpr int kSkip10 = 110, kSkip30 = 130;
    StepRig r;
    r.Press(kRight, +1, kSkip10);
    r.Repeat(kRight, +1, kSkip10);
    r.Repeat(kRight, +1, kSkip10);
    LT_CHECK_EQ((int)r.issued.size(), 1);     // no queue: repeats while a skip is in flight are dropped
    r.c.StepDone(true);
    LT_CHECK_EQ((int)r.issued.size(), 2);
    LT_CHECK_EQ(r.actions[1], kSkip10);
    r.Release(kRight);
    r.c.StepDone(true);
    LT_CHECK_EQ((int)r.issued.size(), 2);     // nothing after the release
    r.Press(kLeft, -1, kSkip30);
    r.c.StepDone(false);                      // no key packet on that side: the chain stays ended
    r.Repeat(kLeft, -1, kSkip30);
    LT_CHECK_EQ((int)r.issued.size(), 3);
    LT_CHECK_EQ(r.actions[2], kSkip30);
}

}
