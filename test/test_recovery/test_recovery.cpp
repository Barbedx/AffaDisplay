// The recovery layer, pinned: the RUNNING-but-deaf watchdog and the hold-window bound.
//
// Both exist because of one bench day (2026-07-29). The controller sat in state RUNNING
// with reception frozen and nothing in the library considered that "down" — healthy() is
// "state == RUNNING" — so the deafness lasted until a human power-cycled the rig. And a
// render held behind a registration that kept failing re-spliced itself for ever on a link
// that was perfectly "ready": inFlight climbed into the thousands with timeouts tracking
// it one for one, because the hold window was only ever checked when the link was DOWN.
//
// The suite asserts the shape of the cures, not just their existence: the stall fires
// ONCE per silence, passes force=true so a recover() that would early-out on "already
// running" restarts the driver anyway, re-arms only on a real received frame, and stays
// out of the way while transmission is deliberately gated.

#include <unity.h>
#include <cstring>

#include "AffaConfig.h"
#include "core/AffaDisplayBase.h"
#include "link/LoopbackLink.h"

#include "../affa_test_support.h"

using namespace affa;
using affatest::FakeClock;
using affatest::mk;
using affatest::panelPeerAlive;
using affatest::panelSyncRequest;

namespace {

constexpr uint8_t kHello[3][8] = {
  {0x70, 0x1A, 0x11, 0x00, 0x00, 0x00, 0x00, 0x01},
  {0xB0, 0x14, 0x11, 0x00, 0x1F, 0x00, 0x00, 0x00},
  {0xB0, 0x14, 0x11, 0x00, 0x1F, 0x00, 0x00, 0x00},
};
constexpr SyncProfile kProfile{0x3AF, 0x3CF, 0x0400, 0xB9, 0xBA, 0x00, 0x00, kHello, 3};
constexpr uint16_t kFuncIds[] = {0x151, 0x1F1};

class TestDisplay final : public AffaDisplayBase {
 public:
  TestDisplay(ICanLink& l, IClock& c) : AffaDisplayBase(l, c, kProfile, kFuncIds, 2) {}
  bool supports(Feature f) const override { return f == Feature::KeyTx; }
  using AffaDisplayBase::enqueue;

 protected:
  uint8_t  packetFiller() const override { return 0x00; }
  uint16_t keyTxId()      const override { return 0x1C1; }
};

// A link whose GATE and HEALTH are independent, which LoopbackLink cannot express: the
// stall watchdog keys off isLive() (gate open) while the down-detector keys off healthy()
// (controller present), and proving the watchdog respects a deliberately shut gate needs
// healthy()==true with isLive()==false at once.
class GatedLink final : public ICanLink {
 public:
  bool gate = true;
  bool healthyV = true;
  bool recoverOk = false;
  uint32_t recoverCalls = 0, forcedCalls = 0;

  bool send(const Frame&) override { return gate; }
  bool recv(Frame& out) override { return _rx.pop(out); }
  bool isLive() const override { return gate && healthyV; }
  bool healthy() const override { return healthyV; }  // defaults to "always fine" — that
                                                      // is the whole point of the deaf bug
  bool recover(bool force) override {
    ++recoverCalls;
    if (force) ++forcedCalls;
    if (recoverOk) healthyV = true;
    return recoverOk;
  }
  Stats stats() const override { return Stats{}; }

  void inject(const Frame& f) { _rx.push(f); }

 private:
  AffaRing<Frame, 64> _rx;
};

int      g_completions = 0;
TxTicket g_lastTicket  = kNoTicket;
Result   g_lastResult  = Result::Ok;

void captureCompletion(TxTicket t, Result r, void*) {
  ++g_completions;
  g_lastTicket = t;
  g_lastResult = r;
}

void hear(GatedLink& l, TestDisplay& d) {
  l.inject(panelSyncRequest());
  d.poll();
}

}  // namespace

// ---------------------------------------------------------------------------

// The full arc: a heard-then-silent bus latches ONE stall, recovery is attempted on the
// normal backoff WITH force, a successful recovery tears the handshake down for a
// re-handshake, and the continuing silence costs nothing further.
void test_stall_fires_once_and_forces_the_recovery(void) {
  GatedLink link;
  FakeClock clk;
  TestDisplay d(link, clk);
  d.begin();
  hear(link, d);
  TEST_ASSERT_TRUE(d.synced());

  // Silence past AFFA_RX_STALL_MS: the stall latches, the link counts one flap, and no
  // recovery is attempted yet — the first attempt honours AFFA_LINK_RECOVER_MS, exactly
  // as a controller-down flap would.
  clk.advance(AFFA_RX_STALL_MS + 100);
  d.poll();
  TEST_ASSERT_EQUAL_UINT32(1, d.linkHealth().flaps);
  TEST_ASSERT_EQUAL_UINT32(0, link.recoverCalls);

  // First attempt, still failing: it must carry force=true, because the controller
  // reports RUNNING and an unforced recover() would early-out with a false success.
  clk.advance(AFFA_LINK_RECOVER_MS);
  d.poll();
  TEST_ASSERT_EQUAL_UINT32(1, link.recoverCalls);
  TEST_ASSERT_EQUAL_UINT32(1, link.forcedCalls);
  TEST_ASSERT_EQUAL_UINT32(1, d.linkHealth().failures);

  // Second attempt, on the doubled backoff, now allowed to succeed: the handshake is torn
  // down to Failed for a fresh re-registration — the panel has been talking to a node
  // that stopped answering, so whatever it had for us is void.
  link.recoverOk = true;
  clk.advance(AFFA_LINK_RECOVER_MS * 2);
  d.poll();
  TEST_ASSERT_EQUAL_UINT32(2, link.recoverCalls);
  TEST_ASSERT_EQUAL_UINT32(2, link.forcedCalls);
  TEST_ASSERT_EQUAL_UINT32(1, d.linkHealth().recoveries);
  TEST_ASSERT_FALSE(d.synced());

  // ONCE PER SILENCE. The bus is still silent, but the latch does not re-arm without a
  // real frame — a genuinely dead or powered-off panel costs exactly one restart, never a
  // reinstall per backoff for ever.
  for (int i = 0; i < 20; ++i) { clk.advance(5000); d.poll(); }
  TEST_ASSERT_EQUAL_UINT32(2, link.recoverCalls);
  TEST_ASSERT_EQUAL_UINT32(1, d.linkHealth().flaps);
}

// A real frame re-arms the watchdog; a second silence is a second flap.
void test_stall_rearms_only_on_a_real_frame(void) {
  GatedLink link;
  link.recoverOk = true;
  FakeClock clk;
  TestDisplay d(link, clk);
  d.begin();
  hear(link, d);

  clk.advance(AFFA_RX_STALL_MS + 100);
  d.poll();
  clk.advance(AFFA_LINK_RECOVER_MS);
  d.poll();
  TEST_ASSERT_EQUAL_UINT32(1, link.recoverCalls);

  // The panel comes back: one frame re-arms everything.
  clk.advance(10);
  link.inject(panelPeerAlive());
  d.poll();

  clk.advance(AFFA_RX_STALL_MS + 100);
  d.poll();
  clk.advance(AFFA_LINK_RECOVER_MS);
  d.poll();
  TEST_ASSERT_EQUAL_UINT32(2, link.recoverCalls);
  TEST_ASSERT_EQUAL_UINT32(2, d.linkHealth().flaps);
}

// A deliberately shut gate means one-sidedness was CHOSEN — mid-OTA, the is-it-us bench
// test, listen-only — and silence is then expected, not a fault. Reopening the gate grants
// a FULL fresh window: the silence that accumulated while gated was the application's
// choice, so converting it into an instant stall would punish every mode switch.
void test_stall_respects_a_deliberately_shut_gate(void) {
  GatedLink link;
  FakeClock clk;
  TestDisplay d(link, clk);
  d.begin();
  hear(link, d);

  link.gate = false;
  for (int i = 0; i < 10; ++i) { clk.advance(5000); d.poll(); }
  TEST_ASSERT_EQUAL_UINT32(0, d.linkHealth().flaps);
  TEST_ASSERT_EQUAL_UINT32(0, link.recoverCalls);

  // Reopening: no instant latch — the watchdog measures from the reopen.
  link.gate = true;
  d.poll();
  TEST_ASSERT_EQUAL_UINT32(0, d.linkHealth().flaps);

  // But a bus still silent for a fresh full window after the reopen IS a stall.
  clk.advance(AFFA_RX_STALL_MS + 100);
  d.poll();
  TEST_ASSERT_EQUAL_UINT32(1, d.linkHealth().flaps);
}

// A latch taken while live must RELEASE when the application goes one-sided afterwards:
// holding it would keep the backoff force-restarting the driver underneath a deliberately
// gated app — mid-OTA, that costs the flash write.
void test_stall_latch_releases_when_the_app_goes_one_sided(void) {
  GatedLink link;
  FakeClock clk;
  TestDisplay d(link, clk);
  d.begin();
  hear(link, d);

  clk.advance(AFFA_RX_STALL_MS + 100);
  d.poll();
  TEST_ASSERT_EQUAL_UINT32(1, d.linkHealth().flaps);   // latched while live

  link.gate = false;                                   // the app chooses silence
  for (int i = 0; i < 10; ++i) { clk.advance(5000); d.poll(); }
  TEST_ASSERT_EQUAL_UINT32(0, link.recoverCalls);      // and is left entirely alone
}

// A successful recovery from a CONTROLLER outage (bus-off class, not a stall) must grant
// the restarted controller a fresh listening window. The stall check is skipped for the
// whole outage, so _lastRxMs is stale by the entire downtime — an instant re-latch here
// would give the fresh driver zero listening time and buy a silent bus a second pointless
// reinstall at the already-doubled backoff.
void test_recovery_grants_a_fresh_window_not_an_instant_relatch(void) {
  GatedLink link;
  FakeClock clk;
  TestDisplay d(link, clk);
  d.begin();
  hear(link, d);

  link.healthyV = false;                               // controller down (bus-off class)
  clk.advance(100);
  d.poll();                                            // flap opens
  TEST_ASSERT_EQUAL_UINT32(1, d.linkHealth().flaps);

  link.recoverOk = true;                               // next attempt will succeed
  clk.advance(AFFA_LINK_RECOVER_MS);
  d.poll();
  TEST_ASSERT_EQUAL_UINT32(1, link.recoverCalls);
  TEST_ASSERT_EQUAL_UINT32(1, d.linkHealth().recoveries);

  // Continued silence: no instant stall from the pre-outage timestamp, and no further
  // restarts — the watchdog re-arms only on a real frame.
  for (int i = 0; i < 20; ++i) { clk.advance(5000); d.poll(); }
  TEST_ASSERT_EQUAL_UINT32(1, link.recoverCalls);
  TEST_ASSERT_EQUAL_UINT32(1, d.linkHealth().flaps);
}

// Echoes must not feed the watchdog: a link that hears only itself is deaf.
void test_echo_frames_do_not_feed_the_watchdog(void) {
  GatedLink link;
  FakeClock clk;
  TestDisplay d(link, clk);
  d.begin();

  affa::Frame f = panelPeerAlive();
  f.fromSelf = true;
  link.inject(f);
  d.poll();

  for (int i = 0; i < 10; ++i) { clk.advance(5000); d.poll(); }
  TEST_ASSERT_EQUAL_UINT32(0, d.linkHealth().flaps);   // never armed: nothing REAL heard
}

// Before anything has ever been heard there is nothing to be deaf TO: a board booted
// beside a powered-off panel waits quietly instead of flapping.
void test_no_stall_before_the_first_frame(void) {
  GatedLink link;
  FakeClock clk;
  TestDisplay d(link, clk);
  d.begin();

  for (int i = 0; i < 10; ++i) { clk.advance(5000); d.poll(); }
  TEST_ASSERT_EQUAL_UINT32(0, d.linkHealth().flaps);
  TEST_ASSERT_EQUAL_UINT32(0, link.recoverCalls);
}

// The hold window binds on a READY link too. Sync stays healthy (the panel pings), but
// registration never completes — every probe times out — so the payload sits behind a
// registration cycle that would re-splice for ever. It must complete NoSync once its hold
// window has run out, not hang until a human intervenes.
void test_hold_window_bounds_a_failing_registration(void) {
  LoopbackLink<> link;                    // autoAck OFF: no probe is ever acknowledged
  FakeClock clk;
  TestDisplay d(link, clk);
  d.onComplete(&captureCompletion, nullptr);
  g_completions = 0;
  d.begin();

  link.inject(panelSyncRequest());
  d.poll();
  TEST_ASSERT_TRUE(d.synced());

  const uint8_t payload[3] = {0x03, 0x52, 0x09};
  const TxTicket t = d.enqueue(0x151, payload, 3);
  TEST_ASSERT_NOT_EQUAL(kNoTicket, t);

  // Pump for double the hold window, keeping the peer alive so sync never Fails — that
  // isolates THIS bound from the link-down hold check that already existed. The exact
  // completion time depends on the probe retry ladder; the contract is "bounded", with
  // the bound of the same order as AFFA_TX_HOLD_MS.
  uint32_t nextPing = 0;
  for (uint32_t elapsed = 0; elapsed <= AFFA_TX_HOLD_MS * 2 && g_completions == 0;
       elapsed += 50) {
    if (elapsed >= nextPing) {
      link.inject(panelPeerAlive());
      nextPing += 1000;
    }
    clk.advance(50);
    d.poll();
  }

  TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_completions,
      "the held render never completed — the registration cycle is unbounded again");
  TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(t), static_cast<uint32_t>(g_lastTicket));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Result::NoSync),
                          static_cast<uint8_t>(g_lastResult));
  TEST_ASSERT_FALSE(d.busy());
}

// ---------------------------------------------------------------------------

void setUp(void) {}
void tearDown(void) {}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_stall_fires_once_and_forces_the_recovery);
  RUN_TEST(test_stall_rearms_only_on_a_real_frame);
  RUN_TEST(test_stall_respects_a_deliberately_shut_gate);
  RUN_TEST(test_stall_latch_releases_when_the_app_goes_one_sided);
  RUN_TEST(test_recovery_grants_a_fresh_window_not_an_instant_relatch);
  RUN_TEST(test_echo_frames_do_not_feed_the_watchdog);
  RUN_TEST(test_no_stall_before_the_first_frame);
  RUN_TEST(test_hold_window_bounds_a_failing_registration);
  return UNITY_END();
}
