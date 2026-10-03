// --logictest cases for the rebind state machine (RebindLogic.h).
#include "LogicTest.h"
#include "RebindLogic.h"

namespace app {

LOGIC_TEST(rebind_single_loss_rebinds_once_and_notifies_once) {
    RebindController c;
    LT_CHECK(c.ReportLoss(RebindReason::DeviceRemoved));
    LT_CHECK(c.Pending());
    LT_CHECK_EQ((int)c.Next(1000), (int)RebindAction::Rebind);
    LT_CHECK(c.Busy() && !c.Pending());
    c.OnRebindResult(true, 1010);
    LT_CHECK(!c.Busy());
    RebindNotice n;
    LT_CHECK(c.TakeNotice(n));
    LT_CHECK(n.kind == RebindNotice::Kind::Recovered);
    LT_CHECK(!c.TakeNotice(n));
    LT_CHECK_EQ((int)c.Next(1020), (int)RebindAction::None);
}

LOGIC_TEST(rebind_losses_during_incident_are_coalesced) {
    RebindController c;
    LT_CHECK(c.ReportLoss(RebindReason::DeviceReset));
    LT_CHECK(!c.ReportLoss(RebindReason::RecreateTarget));      // pending
    LT_CHECK_EQ((int)c.Next(2), (int)RebindAction::Rebind);
    LT_CHECK(!c.ReportLoss(RebindReason::DeviceRemoved));       // rebinding
    LT_CHECK_EQ((int)c.Next(4), (int)RebindAction::None);          // one rebind at a time
    LT_CHECK(c.Reason() == RebindReason::DeviceReset);
    c.OnRebindResult(true, 5);
    LT_CHECK_EQ(c.Incidents(), 1);
    RebindNotice n;
    LT_CHECK(c.TakeNotice(n));
    LT_CHECK(!c.TakeNotice(n));
}

LOGIC_TEST(rebind_failed_attempts_retry_then_fall_back) {
    RebindController c;
    LT_CHECK(c.ReportLoss(RebindReason::DeviceRemoved));
    for (int i = 0; i < 3; i++) {
        LT_CHECK_EQ((int)c.Next(i * 10), (int)RebindAction::Rebind);
        c.OnRebindResult(false, i * 10 + 1);
        RebindNotice n;
        LT_CHECK(!c.TakeNotice(n));                                // no notice while the incident is open
    }
    LT_CHECK_EQ((int)c.Next(40), (int)RebindAction::Fallback);
    LT_CHECK(!c.FallbackActive());
    c.OnRebindResult(true, 41);
    LT_CHECK(c.FallbackActive() && !c.GaveUp());
    RebindNotice n;
    LT_CHECK(c.TakeNotice(n));
    LT_CHECK(n.kind == RebindNotice::Kind::Fallback);
    LT_CHECK(!c.ReportLoss(RebindReason::DeviceRemoved));      // no loop once on the fallback
    LT_CHECK_EQ((int)c.Next(50), (int)RebindAction::None);
    c.ResetFallback();
    LT_CHECK(c.ReportLoss(RebindReason::AdapterChange));
}

LOGIC_TEST(rebind_repeated_successful_incidents_inside_the_window_fall_back) {
    RebindController c;
    for (int i = 0; i < 3; i++) {
        LT_CHECK(c.ReportLoss(RebindReason::DeviceRemoved));
        LT_CHECK_EQ((int)c.Next(1000 * i), (int)RebindAction::Rebind);
        c.OnRebindResult(true, 1000 * i + 5);
    }
    LT_CHECK(c.ReportLoss(RebindReason::DeviceRemoved));
    LT_CHECK_EQ((int)c.Next(3000), (int)RebindAction::Fallback);
    c.OnRebindResult(true, 3001);
    LT_CHECK(c.FallbackActive());
}

LOGIC_TEST(rebind_attempts_expire_after_the_window) {
    RebindController c;
    for (int i = 0; i < 3; i++) {
        LT_CHECK(c.ReportLoss(RebindReason::DeviceReset));
        LT_CHECK_EQ((int)c.Next(1000 * i), (int)RebindAction::Rebind);
        c.OnRebindResult(true, 1000 * i + 5);
    }
    LT_CHECK_EQ(c.AttemptsInWindow(2500), 3);
    LT_CHECK(c.ReportLoss(RebindReason::DeviceReset));
    LT_CHECK_EQ((int)c.Next(40000), (int)RebindAction::Rebind);    // the old attempts left the 30 s window
    LT_CHECK_EQ(c.AttemptsInWindow(40000), 1);
    c.OnRebindResult(true, 40010);
    LT_CHECK(!c.FallbackActive());
}

LOGIC_TEST(rebind_failed_fallback_gives_up_with_one_notice) {
    RebindController c(RebindPolicy{1, 30000});
    LT_CHECK(c.ReportLoss(RebindReason::Test));
    LT_CHECK_EQ((int)c.Next(0), (int)RebindAction::Rebind);
    c.OnRebindResult(false, 1);
    LT_CHECK_EQ((int)c.Next(2), (int)RebindAction::Fallback);
    c.OnRebindResult(false, 3);
    LT_CHECK(c.GaveUp() && !c.FallbackActive());
    RebindNotice n;
    LT_CHECK(c.TakeNotice(n));
    LT_CHECK(n.kind == RebindNotice::Kind::Failed);
    LT_CHECK(!c.TakeNotice(n));
    LT_CHECK(!c.ReportLoss(RebindReason::Test));
}

LOGIC_TEST(rebind_reason_names) {
    LT_CHECK_EQ(std::string(RebindReasonName(RebindReason::DeviceRemoved)), std::string("device_removed"));
    LT_CHECK_EQ(std::string(RebindReasonName(RebindReason::DeviceReset)), std::string("device_reset"));
    LT_CHECK_EQ(std::string(RebindReasonName(RebindReason::RecreateTarget)), std::string("recreate_target"));
    LT_CHECK_EQ(std::string(RebindReasonName(RebindReason::Test)), std::string("test"));
    LT_CHECK_EQ(std::string(RebindReasonName(RebindReason::AdapterChange)), std::string("adapter_change"));
}

}
