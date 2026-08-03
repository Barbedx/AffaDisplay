// Session-boundary regressions for the AFFA3 NAV handshake.
//
// The panel may revoke a session (61 11 01) while one of our 0x151 jobs is
// waiting for 0x551.  An ACK is only meaningful for the same panel session
// that accepted the request.  These tests deliberately deliver the old ACK
// after the reset and require a completely new 151 -> 551, 1F1 -> 5F1
// registration pass before *any* application or restored-control traffic may
// leave the ESP.

#include <unity.h>

#include "../affa_test_support.h"
#include "carminat/CarminatDisplay.h"
#include "core/AffaRing.h"

using namespace affa;
using affatest::FakeClock;
using affatest::drain;
using affatest::expectFrame;
using affatest::mk;

namespace {

struct Rig {
  LoopbackLink<128> link;
  FakeClock clock;
  CarminatDisplay display;

  Rig() : display(link, clock) {}
};

// A live controller that can never accept a frame.  The two modes cover the distinct
// driver reports: Busy means local TX pressure, Rejected means a hard refusal while the
// controller still otherwise reports healthy.  Keeping the receive ring real lets the
// test feed a 01/69 storm without pretending the callback path is synchronous.
class StuckLink final : public ICanLink {
 public:
  enum class Mode : uint8_t { Busy, Rejected };

  explicit StuckLink(Mode m) : mode(m) {}

  bool send(const Frame&) override { return false; }
  TxDisposition trySend(const Frame& f) override {
    ++offers;
    if (f.id == 0x3AF && f.data[0] == 0xB9) ++aliveOffers;
    if (f.id == 0x3AF && f.data[0] == 0xBA) ++requestOffers;
    return mode == Mode::Busy ? TxDisposition::Busy : TxDisposition::Rejected;
  }
  bool recv(Frame& out) override { return rx.pop(out); }
  bool isLive() const override { return true; }
  bool healthy() const override { return true; }

  void inject(const Frame& f) { rx.push(f); }

  Mode mode;
  uint32_t offers = 0;
  uint32_t aliveOffers = 0;
  uint32_t requestOffers = 0;

 private:
  AffaRing<Frame, 128> rx;
};

// Accept B9 but make the following BA locally Busy once.  This pins the discovery-pair
// phase machine: after a successful B9, recovery must retry BA itself, not restart at B9.
class BaBusyOnceLink final : public ICanLink {
 public:
  bool send(const Frame& f) override { return trySend(f) == TxDisposition::Accepted; }
  TxDisposition trySend(const Frame& f) override {
    if (f.id == 0x3AF && f.data[0] == 0xB9) {
      ++aliveOffers;
      sent.push(f);
      return TxDisposition::Accepted;
    }
    if (f.id == 0x3AF && f.data[0] == 0xBA) {
      ++requestOffers;
      if (busyBaOnce) {
        busyBaOnce = false;
        return TxDisposition::Busy;
      }
      sent.push(f);
      return TxDisposition::Accepted;
    }
    sent.push(f);
    return TxDisposition::Accepted;
  }
  bool recv(Frame& out) override { return rx.pop(out); }
  bool isLive() const override { return true; }
  bool healthy() const override { return true; }

  void inject(const Frame& f) { rx.push(f); }
  bool takeSent(Frame& f) { return sent.pop(f); }

  bool busyBaOnce = true;
  uint32_t aliveOffers = 0;
  uint32_t requestOffers = 0;

 private:
  AffaRing<Frame, 64> rx;
  AffaRing<Frame, 64> sent;
};

constexpr Frame kHello =
    {0x3AF, 8, {0xB0, 0x14, 0x11, 0x00, 0x1F, 0x00, 0x00, 0x00}, false};
constexpr Frame kAlive =
    {0x3AF, 8, {0xB9, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, false};
constexpr Frame kRequest =
    {0x3AF, 8, {0xBA, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, false};
constexpr Frame kReg151 =
    {0x151, 8, {0x70, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, false};
constexpr Frame kReg1F1 =
    {0x1F1, 8, {0x70, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, false};
constexpr Frame kPowerOn =
    {0x151, 8, {0x03, 0x52, 0x09, 0x00, 0x00, 0x00, 0x00, 0x00}, false};
constexpr Frame kTime1000 =
    {0x151, 8, {0x05, 0x56, 0x31, 0x30, 0x30, 0x30, 0x00, 0x00}, false};

Frame ack(uint16_t requestId) {
  return mk(static_cast<uint32_t>(requestId) | 0x0400,
            {0x74, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3});
}

void expectNoFrame(Rig& r, const char* what) {
  Frame f;
  TEST_ASSERT_FALSE_MESSAGE(r.link.takeSent(f), what);
}

// Runs the paced B0 x3 that answers whichever full request has just been accepted, and
// consumes the first registration probe.
//
// REGISTRATION LEAVES WITH B0#3, unconditionally. [CAP] In all four OEM captures the radio
// puts `151 70` on the wire 0.014-0.302 ms after the third announce with no application
// involvement at all, so this helper does NOT require the caller to hold a payload first.
void runHelloToFirstRegistration(Rig& r, const char* phase) {
  r.clock.advance(carminat::kHelloFirstDelayMs);
  r.display.poll();
  expectFrame(r.link, kHello, "B0 #1");

  r.clock.advance(carminat::kHelloFrameGapMs);
  r.display.poll();
  expectFrame(r.link, kHello, "B0 #2");

  r.clock.advance(carminat::kHelloFrameGapMs);
  r.display.poll();
  expectFrame(r.link, kHello, "B0 #3");
  expectFrame(r.link, kReg151, phase);
  TEST_ASSERT_FALSE_MESSAGE(r.display.registered(),
                            "one registration ACK cannot latch FUNCSREG");
}

// Opens one *new* captured Carminat authorization with the good `00` and leaves the first
// 151 registration waiting for its panel ACK.
void startSessionToFirstRegistration(Rig& r, const char* phase) {
  r.link.inject(affatest::panelSyncRequest());
  r.display.poll();
  expectNoFrame(r, "61 11 00 schedules the paced B0 opening");
  runHelloToFirstRegistration(r, phase);
}

void completeFreshRegistration(Rig& r, const char* first, const char* second) {
  r.link.inject(ack(0x151));
  r.display.poll();
  expectFrame(r.link, kReg1F1, first);
  TEST_ASSERT_FALSE_MESSAGE(r.display.registered(),
                            "the first fresh registration is not the whole table");

  r.link.inject(ack(0x1F1));
  r.display.poll();
  expectNoFrame(r, second);
  TEST_ASSERT_TRUE_MESSAGE(r.display.registered(),
                           "only both new registration ACKs may latch FUNCSREG");
}

// A `61 11 01` that arrives while we have NOT yet put a BA on the wire. The display is
// calling into what is, as far as it can tell, an empty bus, and it earns exactly one
// bounded B9 -> BA discovery transaction — never the announce burst.
void revokeSession(Rig& r, const char* why) {
  r.link.inject(affatest::panelSyncStart());
  r.display.poll();
  expectFrame(r.link, kAlive, why);
  expectFrame(r.link, kRequest, why);
  expectNoFrame(r, "START bootstrap is exactly B9 + BA");
  TEST_ASSERT_FALSE_MESSAGE(r.display.synced(), "61 11 01 revokes usable authorization");
  TEST_ASSERT_FALSE_MESSAGE(r.display.registered(), "61 11 01 revokes FUNCSREG");
}

// A `61 11 01` that ANSWERS the BA above is a different frame with the same bytes: it is a
// full request. MEASURED, NOT ASSUMED — "aknowledge offed display cONNECT OT POWER.csv" is a
// display that was powered with no radio present; it repeats `3CF 61 11 01` sixteen times at
// ~104 ms and NEVER sends `61 11 00`, and the OEM radio answers it with the ordinary B0
// announce burst 30.75 ms later and then completes the whole session — registration, `03 52`,
// ISO-TP text. The low bit reports the display's own state; it is not an authorization grade.
//
// It still voids the previous session first: a START arriving while we hold registrations
// says "your registration is void", so FUNCSREG and usable authorization drop before the new
// announce burst is scheduled.
void revokeSessionWithAnsweringStart(Rig& r, const char* why) {
  r.link.inject(affatest::panelSyncStart());
  r.display.poll();
  expectNoFrame(r, "an answering 01 schedules the paced B0 opening, exactly as 00 does");
  TEST_ASSERT_FALSE_MESSAGE(r.display.synced(), why);
  TEST_ASSERT_FALSE_MESSAGE(r.display.registered(),
                            "a new opening voids FUNCSREG before it re-registers");
}

// Build an ACKed desired power state using the regular profile path, then leave manual ACK
// control enabled for the race under test.
void armCachedPower(Rig& r) {
  r.display.begin();
  r.display.setSelfAck(true);
  TEST_ASSERT_EQUAL(static_cast<uint8_t>(Result::Ok),
                    static_cast<uint8_t>(r.display.setPower(true)));
  affatest::completeCarminatAuth(r.display, r.link, r.clock);
  affatest::settleCarminatRegistration(r.display, r.clock);
  r.clock.advance(carminat::kPayloadAfterRegistrationMs);
  affatest::pumpUntilIdle(r.display);
  TEST_ASSERT_TRUE(r.display.registered());
  r.display.setSelfAck(false);
  drain(r.link);
}

void assertBootstrapStormIsBounded(StuckLink::Mode mode) {
  StuckLink link(mode);
  FakeClock clock;
  CarminatDisplay display(link, clock);
  display.begin();

  // A bad controller receives a mixture of the panel's two opening reminders.  The library
  // may make its bounded initial offer (and one bounded retry), but a reminder must never
  // reset that budget or turn the CAN task into an infinite B9/BA producer.
  for (uint8_t i = 0; i < 32; ++i) {
    link.inject((i & 1u) ? affatest::panelSyncStart() : affatest::panelPeerAlive());
    display.poll();
    clock.advance(AFFA_TX_RETRY_MS);
  }

  TEST_ASSERT_LESS_OR_EQUAL_UINT32_MESSAGE(
      2, link.aliveOffers,
      "one-shot bootstrap gets at most its initial B9 offer and one bounded retry");
  TEST_ASSERT_LESS_OR_EQUAL_UINT32_MESSAGE(
      2, link.requestOffers,
      "one-shot bootstrap gets at most its initial BA offer and one bounded retry");
  TEST_ASSERT_LESS_OR_EQUAL_UINT32_MESSAGE(
      4, link.offers,
      "01/69 reminders cannot keep re-arming a permanently undeliverable bootstrap");
}

}  // namespace

// A late 0x551 ACK from the *old* 0x151 registration must be ignored after a fresh 61 11
// 01.  In particular it may not recreate Failed|FuncsReg, because then a later good 00
// would open the gate and send the held clock without the new ordered registrations.
void test_late_registration_ack_cannot_revive_the_old_session(void) {
  Rig r;
  r.display.begin();
  TEST_ASSERT_EQUAL(static_cast<uint8_t>(Result::Ok),
                    static_cast<uint8_t>(r.display.setTime("1000")));

  startSessionToFirstRegistration(r, "old-session 151 registration");
  revokeSession(r, "new 01 tears down the old in-flight registration");

  // This is the decisive race: 0x551 belongs to the 151 frame which was put on the wire
  // before 01.  It must not complete a registration in the newly reset session.
  r.link.inject(ack(0x151));
  r.display.poll();
  TEST_ASSERT_FALSE_MESSAGE(r.display.registered(),
                            "late old-session 551 may not restore FUNCSREG");
  TEST_ASSERT_FALSE_MESSAGE(r.display.synced(),
                            "late old-session 551 may not restore authorization");
  expectNoFrame(r, "late registration ACK does not transmit application traffic");

  // The next 00 is a genuinely new session.  The retained time is allowed out only after
  // another full, ordered registration table has been acknowledged.
  startSessionToFirstRegistration(r, "fresh session starts again at 151");
  completeFreshRegistration(r, "fresh 551 releases the 1F1 registration",
                             "final registration ACK starts only the 400-ms gate");

  r.clock.advance(carminat::kPayloadAfterRegistrationMs - 1);
  r.display.poll();
  expectNoFrame(r, "held time stays behind the full post-registration quiet period");
  r.clock.advance(1);
  r.display.poll();
  expectFrame(r.link, kTime1000, "only the new complete registration releases time");
  expectNoFrame(r, "no stale duplicate payload follows the fresh clock");
}

// The same epoch rule applies to the internal durable-control replay.  If its old 0x551
// ACK arrives after a second 01, it must not clear the pending restore for the next session;
// otherwise a powered-on display can silently remain off after an otherwise successful
// recovery, and the held clock overtakes the missing power command.
void test_late_reassert_ack_cannot_clear_the_next_session_restore(void) {
  Rig r;
  armCachedPower(r);

  // Session B: lose the original registered session, retain a clock, then reach its
  // internally generated power replay and deliberately leave that 151 in flight.
  revokeSession(r, "first 01 starts recovery of cached power");
  TEST_ASSERT_EQUAL(static_cast<uint8_t>(Result::Ok),
                    static_cast<uint8_t>(r.display.setTime("1000")));
  startSessionToFirstRegistration(r, "recovery B starts at 151");
  completeFreshRegistration(r, "recovery B 551 releases 1F1",
                             "recovery B registration has no payload before +400 ms");
  r.clock.advance(carminat::kPayloadAfterRegistrationMs);
  r.display.poll();
  expectFrame(r.link, kPowerOn, "session B starts its cached power replay first");

  // Now session C starts before B's power ACK arrives.  The late ACK must be discarded as
  // B-only evidence, while the desired ON state remains pending for C.
  //
  // This second 01 arrives AFTER the BA that the first revocation put on the wire, so it is
  // a full request and opens C by itself — no `61 11 00` is involved anywhere below. That is
  // the harder version of this race, not a weaker one: the epoch has to be voided by the very
  // frame that also starts the next session.
  revokeSessionWithAnsweringStart(r, "second 01 invalidates the in-flight B reassert");
  r.link.inject(ack(0x151));
  r.display.poll();
  TEST_ASSERT_FALSE_MESSAGE(r.display.registered(),
                            "late reassert 551 may not restore B registration");
  TEST_ASSERT_FALSE_MESSAGE(r.display.synced(),
                            "late reassert 551 may not restore B authorization");
  expectNoFrame(r, "late reassert ACK does not leak held time into session C");

  runHelloToFirstRegistration(r, "session C starts a fresh registration table");
  completeFreshRegistration(r, "session C 551 releases 1F1",
                             "session C registration still waits before payload");
  r.clock.advance(carminat::kPayloadAfterRegistrationMs);
  r.display.poll();
  expectFrame(r.link, kPowerOn,
              "session C replays desired power despite the stale session-B ACK");

  // Its own ACK is current, so it completes the restore and only then lets the held time
  // follow.  This also guards against a fix that merely leaves `pending` set forever.
  r.link.inject(ack(0x151));
  r.display.poll();
  expectFrame(r.link, kTime1000, "current power ACK releases the held clock");
  expectNoFrame(r, "the fresh session emits no duplicate restore");
}

// A bare 69 is not authorization, but it is enough evidence that a panel is present to
// make the single discovery attempt.  The default profile must issue exactly B9 + BA once;
// it must not turn a ping retry stream into either periodic BA or application traffic.
void test_bare_first_69_gets_one_discovery_pair_but_never_unlocks_output(void) {
  Rig r;
  r.display.begin();
  TEST_ASSERT_EQUAL(static_cast<uint8_t>(Result::Ok),
                    static_cast<uint8_t>(r.display.setTime("1000")));

  r.link.inject(affatest::panelPeerAlive());
  r.display.poll();
  expectFrame(r.link, kAlive, "first bare 69 gets one B9 discovery frame");
  expectFrame(r.link, kRequest, "first bare 69 gets one BA discovery frame");
  expectNoFrame(r, "bare 69 sends no hello, registration, or held payload");
  TEST_ASSERT_FALSE_MESSAGE(r.display.synced(), "69 alone is not 61 11 00 authorization");
  TEST_ASSERT_FALSE_MESSAGE(r.display.registered(), "69 alone never registers functions");

  for (uint8_t i = 0; i < 32; ++i) r.link.inject(affatest::panelPeerAlive());
  r.display.poll();
  expectNoFrame(r, "repeated bare 69 frames do not make B9/BA periodic");
  TEST_ASSERT_FALSE(r.display.synced());
  TEST_ASSERT_FALSE(r.display.registered());

  r.clock.advance(carminat::kSyncIntervalMs * 3);
  r.display.poll();
  expectNoFrame(r, "a bare 69 discovery never grows into an idle heartbeat stream");
}

// A `69` that lands exactly on the paced-heartbeat deadline must still yield exactly ONE
// B9 — the paced one — because the radio's heartbeat is a free-running timer and the ping is
// not an input to it.
//
// THIS IS A MEASURED COLLISION, NOT A HYPOTHETICAL. The display's 69 runs at 503.7-504.4 ms
// while the radio's B9 runs at 500.0-500.3 ms, so the two drift through each other; in
// "aknowledge offed display 2.csv" they pass within 0.023 ms and the B9 cadence does not
// so much as hiccup. Both of the ways to get this wrong are visible from here: a pong
// implementation puts a second B9 alongside the paced one, and a cadence-consuming pong puts
// none at all because the ping "already answered".
void test_a_ping_on_the_heartbeat_boundary_still_yields_exactly_one_b9(void) {
  Rig r;
  r.display.begin();
  r.link.inject(affatest::panelSyncRequest());   // t = 0: the first B9 is due at t = 500
  r.display.poll();
  expectNoFrame(r, "good 00 waits for B0 #1");
  r.clock.advance(carminat::kHelloFirstDelayMs);
  r.display.poll();
  expectFrame(r.link, kHello, "B0 #1");
  r.clock.advance(carminat::kHelloFrameGapMs);
  r.display.poll();
  expectFrame(r.link, kHello, "B0 #2");
  r.clock.advance(carminat::kHelloFrameGapMs);
  r.display.poll();
  expectFrame(r.link, kHello, "B0 #3");
  // The opening registers the panel's functions whether or not anything is ever rendered:
  // [CAP] `151 70` follows B0#3 by 0.014-0.302 ms in every capture, and this rig has no
  // queued payload at all. A build that only ever shows a clock still has to register.
  expectFrame(r.link, kReg151, "the opening registers 151 with no application payload");
  expectNoFrame(r, "and 1F1 waits for the 551 ACK, not for the clock");
  TEST_ASSERT_TRUE(r.display.synced());

  r.clock.advance(carminat::kSyncIntervalMs - 3 * carminat::kHelloFrameGapMs);  // t = 500
  r.link.inject(affatest::panelPeerAlive());
  r.display.poll();
  expectFrame(r.link, kAlive, "the paced heartbeat lands on its own deadline");
  expectNoFrame(r, "the coincident ping adds no second B9 and cancels no first one");
}

void test_busy_one_shot_bootstrap_is_bounded_under_repeated_01_and_69(void) {
  assertBootstrapStormIsBounded(StuckLink::Mode::Busy);
}

void test_rejected_one_shot_bootstrap_is_bounded_under_repeated_01_and_69(void) {
  assertBootstrapStormIsBounded(StuckLink::Mode::Rejected);
}

void test_busy_ba_retries_only_ba_without_a_second_b9(void) {
  BaBusyOnceLink link;
  FakeClock clock;
  CarminatDisplay display(link, clock);
  display.begin();

  link.inject(affatest::panelPeerAlive());
  display.poll();
  expectFrame(link, kAlive, "discovery B9 was accepted");
  Frame f;
  TEST_ASSERT_FALSE_MESSAGE(link.takeSent(f), "first BA offer was Busy and did not leave CAN");
  TEST_ASSERT_EQUAL_UINT32(1, link.aliveOffers);
  TEST_ASSERT_EQUAL_UINT32(1, link.requestOffers);
  TEST_ASSERT_FALSE(display.synced());
  TEST_ASSERT_FALSE(display.registered());

  clock.advance(AFFA_TX_RETRY_MS);
  display.poll();
  expectFrame(link, kRequest, "only the pending BA retries after a Busy controller");
  TEST_ASSERT_FALSE_MESSAGE(link.takeSent(f), "BA retry must not duplicate the accepted B9");
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, link.aliveOffers,
                                    "accepted B9 is never re-offered with the BA retry");
  TEST_ASSERT_EQUAL_UINT32(2, link.requestOffers);
  TEST_ASSERT_FALSE(display.synced());
  TEST_ASSERT_FALSE(display.registered());
}

void setUp(void) {}
void tearDown(void) {}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_late_registration_ack_cannot_revive_the_old_session);
  RUN_TEST(test_late_reassert_ack_cannot_clear_the_next_session_restore);
  RUN_TEST(test_bare_first_69_gets_one_discovery_pair_but_never_unlocks_output);
  RUN_TEST(test_a_ping_on_the_heartbeat_boundary_still_yields_exactly_one_b9);
  RUN_TEST(test_busy_one_shot_bootstrap_is_bounded_under_repeated_01_and_69);
  RUN_TEST(test_rejected_one_shot_bootstrap_is_bounded_under_repeated_01_and_69);
  RUN_TEST(test_busy_ba_retries_only_ba_without_a_second_b9);
  return UNITY_END();
}
