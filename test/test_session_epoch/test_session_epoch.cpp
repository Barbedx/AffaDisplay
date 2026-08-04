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
    if (f.id == 0x3AF && f.data[0] == 0xB0) ++helloOffers;
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
  uint32_t helloOffers = 0;

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
// The reflex reply we owe the display's own `1C1 70` channel registration. [CAP] 12/12
// across the four OEM captures, answered in 0.288/0.453/0.470/0.483 ms.
constexpr Frame kPanelChannelAck =
    {0x5C1, 8, {0x74, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, false};
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

  // THE DISPLAY REGISTERS ITS OWN CHANNEL FIRST, and ours is gated on it. [CAP] measured
  // 4/4: `1C1 70` lands 0.81-1.55 ms after B0#1 — between the first and second announce
  // frames — we answer `5C1 74 00 …` in 0.25-0.48 ms, and only 60.69-61.34 ms later, after
  // B0#3, does `151 70` follow. The reflex leaves from the RX pump, so it precedes B0#2 in
  // the same poll. A session that never sees a 1C1 never registers, by design, so every
  // opening modelled here has to include one — and each session reset clears the latch, so
  // it is re-injected on every pass through this helper.
  r.link.inject(mk(0x1C1, {0x70, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3}));
  r.clock.advance(carminat::kHelloFrameGapMs);
  r.display.poll();
  expectFrame(r.link, kPanelChannelAck, "5C1 74 reflex, ahead of B0 #2");
  expectFrame(r.link, kHello, "B0 #2");

  r.clock.advance(carminat::kHelloFrameGapMs);
  r.display.poll();
  expectFrame(r.link, kHello, "B0 #3");
  expectFrame(r.link, kReg151, phase);
  TEST_ASSERT_FALSE_MESSAGE(r.display.registered(),
                            "one registration ACK cannot latch FUNCSREG");
}

// Opens one *new* captured Carminat authorization from a bus on which we have not yet
// announced, and leaves the first 151 registration waiting for its panel ACK.
//
// OUR `BA` COMES FIRST, AND THE BURST ANSWERS THE PANEL'S *NEXT* REQUEST. [CAP] 4/4, and
// spelled out in "aknowledge offed display cONNECT OT POWER.csv" — see
// SyncProfile::helloRequiresAnnounce and the transcript in affa_test_support.h. Both requests
// land at the same fake-clock instant, so the deadlines these tests measure from "the
// request" are unmoved.
void startSessionToFirstRegistration(Rig& r, const char* phase) {
  affatest::carminatOpeningRequest(r.display, r.link);
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

// THE BARE-DISCOVERY `61 11 01` IS GONE FROM THIS FILE, and helloRequiresAnnounce is why.
// Every opening now puts our `BA` on the wire before the burst, so `_unauthControlIssued` is
// latched from the first request of the very first session onwards and a later `61 11 01` can
// never again be the "calling into an empty bus" probe — it is always an ANSWERING start,
// i.e. a full request. That path is modelled by revokeSessionWithAnsweringStart() below; the
// bare probe itself is still pinned, at full strength and from a genuinely cold rig, by
// test_bare_first_69_gets_one_discovery_ba_but_never_unlocks_output.
//
// AND THE UNRECOGNISED-BYTE DOOR IS CLOSED TOO, deliberately. `revokeSessionWithUnknownAuth()`
// used to stand here and void a session that had NOT reached FUNCSREG, by sending `61 11 C5`
// — a byte no capture contains — into the second, dimmer copy of the request branch. That
// branch is deleted: any complete `61 11 xx` is the same request, so 0xC5 now takes the one
// path all of them take and, with FUNCSREG unset, is absorbed without a teardown.
//
// WHICH LEAVES A REAL GAP, RECORDED RATHER THAN GLOSSED: nothing inbound can void a
// half-open session — one that drew its burst but never latched FUNCSREG. The peer watchdog
// cannot either, because pumpSync() returns early on `registerAfterHello && !FuncsReg`. It is
// not a REGRESSION — a panel sends `00` or `01`, and both were already absorbed in that state
// — but it is a hole, and closing it means deciding what a post-burst `61 11` means, which no
// capture answers. See docs/REFACTOR-PLAN.md.
//
// A `61 11 01` that ANSWERS our BA is a different frame with the same bytes: it is a full
// request. MEASURED, NOT ASSUMED — "aknowledge offed display cONNECT OT POWER.csv" is a
// display that was powered with no radio present; it repeats `3CF 61 11 01` sixteen times at
// ~104 ms and NEVER sends `61 11 00`, and the OEM radio answers it with the ordinary B0
// announce burst 30.75 ms later and then completes the whole session — registration, `03 52`,
// ISO-TP text. The low bit reports the display's own state; it is not an authorization grade.
//
// It still voids the previous session first: a request arriving while we hold registrations
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

  // The B9 bound is ZERO rather than "at most two". The announce is BA-only, so a B9 offered
  // here at all would be a bug regardless of how few there were — pin it exactly, or this
  // assertion becomes a bound on a frame that is never sent and stops meaning anything.
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(
      0, link.aliveOffers, "the announce is BA-only: no B9 is ever offered to the driver");
  TEST_ASSERT_LESS_OR_EQUAL_UINT32_MESSAGE(
      2, link.requestOffers,
      "one-shot announce gets at most its initial BA offer and one bounded retry");

  // THE TOTAL BOUND USED TO BE `<= 2` AND IT WAS MEASURING THE WRONG THING, which only
  // became visible when byte 2 stopped being special. It held because `61 11 01` fell into
  // the deleted second request branch, where a reminder could never queue a hello at all;
  // `61 11 00` on this same dead link has ALWAYS produced a paced B0 retry stream, and that
  // is deliberate — the hello is what opens the session, so it keeps trying at its own
  // AFFA_TX_RETRY_MS floor until the controller accepts it. Now that every complete request
  // is the same request, the `01`s reach that path too, and the honest bound is not on the
  // COUNT but on what drives it: nothing here may be paced by the PANEL.
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(
      link.offers, link.requestOffers + link.helloOffers,
      "only the announce and the hello are ever offered — nothing else escapes the opening");

  // Consume whatever retry is due at this instant, so the storm below starts from a settled
  // floor rather than racing one.
  display.poll();
  const uint32_t settled = link.offers;

  // AND HERE IS THE PROPERTY THE OLD BOUND WAS REACHING FOR. A no-ACK panel repeats its
  // reminders at line rate; 64 of them in a single RX drain, with the clock standing still,
  // must not buy a single extra transmit attempt. That is what "bounded" has to mean on a
  // wire whose peer controls the frame rate and we control only our own floors.
  for (uint8_t i = 0; i < 64; ++i)
    link.inject((i & 1u) ? affatest::panelSyncStart() : affatest::panelPeerAlive());
  display.poll();
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(
      settled, link.offers,
      "64 reminders in one drain, at one instant, add not one offer to the driver");
}

}  // namespace

// A late 0x551 ACK from the *old* 0x151 registration must be ignored once that session has
// been torn down.  In particular it may not recreate Failed|FuncsReg, because then a later
// good 00 would open the gate and send the held clock without the new ordered registrations.
//
// RENAMED from ..._cannot_revive_the_old_session, and the TRIGGER HAS NOW CHANGED TWICE,
// because each time the protocol was measured the door this test used got narrower.
//
// First helloRequiresAnnounce landed: our `BA` precedes every burst, so from the first
// session onwards every `61 11 01` is an ANSWERING start rather than a bare probe, and the
// test moved to an unrecognised `61 11 C5`. Then byte 2 turned out to mean nothing at all
// (`61 11 xx` is one request, [CAP] sixteen `01`s and no `00` complete a whole session), the
// second request branch was deleted with the flags that selected it, and 0xC5 stopped being
// special: it is absorbed, exactly as `00` and `01` already were, unless FUNCSREG is held.
//
// So the teardown is now the one the library actually implements — a complete request
// arriving at a REGISTERED session — and session A is driven all the way to FUNCSREG to
// reach it. The race under test is unchanged and is still the point: a `0x551` addressed to
// the dead epoch's `0x151` must not restore FUNCSREG or authorization, and the held clock
// must wait for a genuinely new, fully acknowledged registration table.
void test_late_registration_ack_cannot_revive_a_torn_down_session(void) {
  Rig r;
  // Auto-power off: the race under test is a stale `551` against a held CLOCK, and the
  // library's own `03 52 09` would sit between the registration and that clock in two
  // separate sessions. See AffaDisplayBase::setAutoPower().
  r.display.setAutoPower(false);
  r.display.begin();
  TEST_ASSERT_EQUAL(static_cast<uint8_t>(Result::Ok),
                    static_cast<uint8_t>(r.display.setTime("1000")));

  startSessionToFirstRegistration(r, "old-session 151 registration");
  completeFreshRegistration(r, "old-session 551 releases the 1F1 registration",
                            "old-session registration starts only the 400-ms gate");
  // The clock is deliberately NOT advanced past that gate: the held time must still be held
  // when the session dies under it.
  revokeSessionWithAnsweringStart(r, "an answering request voids the registered session");

  // This is the decisive race: 0x551 belongs to session A's 151 registration and arrives
  // after that session was voided. It must not complete a registration in the new one.
  r.link.inject(ack(0x151));
  r.display.poll();
  TEST_ASSERT_FALSE_MESSAGE(r.display.registered(),
                            "late old-session 551 may not restore FUNCSREG");
  TEST_ASSERT_FALSE_MESSAGE(r.display.synced(),
                            "late old-session 551 may not restore authorization");
  expectNoFrame(r, "late registration ACK does not transmit application traffic");

  // The revocation scheduled the next opening itself, so the fresh session resumes at its
  // burst. The retained time is allowed out only after another full, ordered registration
  // table has been acknowledged.
  runHelloToFirstRegistration(r, "fresh session starts again at 151");
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
  //
  // THE FIRST 01 IS AN ANSWERING START TOO NOW. armCachedPower() completed a whole opening,
  // so our BA is long since on the wire and `_unauthControlIssued` is latched — [CAP] and an
  // answering 01 is a full request, not a probe. Session A holds FUNCSREG, so this one voids
  // the epoch exactly as the old bare-probe revocation did AND schedules B's burst itself,
  // which is why runHelloToFirstRegistration() takes it from here rather than a fresh
  // request. Nothing about the reassert race below moves.
  revokeSessionWithAnsweringStart(r, "first 01 starts recovery of cached power");
  TEST_ASSERT_EQUAL(static_cast<uint8_t>(Result::Ok),
                    static_cast<uint8_t>(r.display.setTime("1000")));
  runHelloToFirstRegistration(r, "recovery B starts at 151");
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
// make the single discovery attempt.  The default profile must issue exactly one BA once;
// it must not turn a ping retry stream into either periodic BA or application traffic.
//
// RENAMED from ..._one_discovery_pair_...: there is no pair any more. [CAP] the reattach
// capture "aknowledge offed display.csv" at 84945066 opens with a bare `3AF BA`, so
// the announce is BA-only and the leading B9 is gone. The bound this test exists to
// keep — ONE frame, ever, no matter how many pings arrive — is asserted at full strength on
// the BA, and the absence of the B9 is now asserted too rather than assumed.
void test_bare_first_69_gets_one_discovery_ba_but_never_unlocks_output(void) {
  Rig r;
  r.display.begin();
  TEST_ASSERT_EQUAL(static_cast<uint8_t>(Result::Ok),
                    static_cast<uint8_t>(r.display.setTime("1000")));

  r.link.inject(affatest::panelPeerAlive());
  r.display.poll();
  expectFrame(r.link, kRequest, "first bare 69 gets one BA discovery frame");
  expectNoFrame(r, "no B9, no hello, no registration and no held payload");
  TEST_ASSERT_FALSE_MESSAGE(r.display.synced(), "69 alone is not 61 11 00 authorization");
  TEST_ASSERT_FALSE_MESSAGE(r.display.registered(), "69 alone never registers functions");

  for (uint8_t i = 0; i < 32; ++i) r.link.inject(affatest::panelPeerAlive());
  r.display.poll();
  expectNoFrame(r, "repeated bare 69 frames do not make the BA periodic");
  TEST_ASSERT_FALSE(r.display.synced());
  TEST_ASSERT_FALSE(r.display.registered());

  r.clock.advance(carminat::kSyncIntervalMs * 3);
  r.display.poll();
  expectNoFrame(r, "a bare 69 discovery never grows into an idle heartbeat stream");

  // AND THE DISCOVERY FRAME ITSELF CARRIES NO B9. This used to read the profile flag
  // `SyncProfile::bootstrapAliveFrame`; the flag is gone and the rule is unconditional, so
  // the absence is asserted where it is observable. Every frame this test has produced was
  // consumed by an expectFrame/expectNoFrame above, and exactly one of them existed: the
  // bare `BA` at the top. A late ping cannot conjure the B9 the pair used to lead with
  // either.
  r.link.inject(affatest::panelPeerAlive());
  r.display.poll();
  expectNoFrame(r, "no ping, early or late, ever puts a B9 in front of the announce");
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
  // Auto-power off: this counts B9s in a 500 ms window and the library's own `03 52 09`
  // lands inside it. See AffaDisplayBase::setAutoPower().
  r.display.setAutoPower(false);
  r.display.begin();
  // Two requests, both at t = 0, with our bare `BA` announce between them — the burst answers
  // the panel's SECOND ask ([CAP] 4/4; SyncProfile::helloRequiresAnnounce). The announce
  // costs no fake-clock time, so the first B9 is still due at t = 500 counted from here, and
  // the phase this test is about is untouched.
  affatest::carminatOpeningRequest(r.display, r.link);
  r.clock.advance(carminat::kHelloFirstDelayMs);
  r.display.poll();
  expectFrame(r.link, kHello, "B0 #1");
  // The display opens its own channel between B0#1 and B0#2 ([CAP] 0.81-1.55 ms behind it,
  // 4/4) and we answer `5C1 74 00 …` on sight. That reflex is what authorizes our own 0x70
  // probes: without it this rig — which queues no payload at all — would never register, and
  // then the heartbeat below would never start either.
  r.link.inject(mk(0x1C1, {0x70, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3}));
  r.clock.advance(carminat::kHelloFrameGapMs);
  r.display.poll();
  expectFrame(r.link, kPanelChannelAck, "5C1 74 reflex, ahead of B0 #2");
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

  // THE HEARTBEAT DOES NOT EXIST UNTIL REGISTRATION IS COMPLETE. [CAP] in the reattach
  // capture the radio's first `3AF B9` is 15.3 ms after the display's `5F1 74`, with nothing
  // on 0x3AF between B0#3 and it. So both 74s are delivered here — at t = 93, so the paced
  // deadline the test is about is still the one the 61 11 00 armed at t = 0.
  r.link.inject(ack(0x151));
  r.display.poll();
  expectFrame(r.link, kReg1F1, "551 releases the second probe");
  r.link.inject(ack(0x1F1));
  r.display.poll();
  TEST_ASSERT_TRUE_MESSAGE(r.display.registered(), "5F1 latches FUNCSREG and arms B9");
  expectNoFrame(r, "completing registration is not itself a heartbeat");

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

// RENAMED from ..._without_a_second_b9. There is no FIRST B9 to have a second of any more:
// the announce is a bare BA, unconditionally ([CAP] "aknowledge
// offed display.csv" at 84945066). The phase machine this test exists to pin is intact and
// is if anything easier to get wrong now — a Busy BA must be retried as a BA, exactly once,
// and the retry must not decide that a heartbeat belongs in front of it.
void test_busy_ba_retries_only_ba_and_never_grows_a_b9(void) {
  BaBusyOnceLink link;
  FakeClock clock;
  CarminatDisplay display(link, clock);
  display.begin();

  link.inject(affatest::panelPeerAlive());
  display.poll();
  Frame f;
  TEST_ASSERT_FALSE_MESSAGE(link.takeSent(f), "first BA offer was Busy and did not leave CAN");
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, link.aliveOffers,
                                   "the bootstrap never OFFERS a B9, busy or not");
  TEST_ASSERT_EQUAL_UINT32(1, link.requestOffers);
  TEST_ASSERT_FALSE(display.synced());
  TEST_ASSERT_FALSE(display.registered());

  clock.advance(AFFA_TX_RETRY_MS);
  display.poll();
  expectFrame(link, kRequest, "only the pending BA retries after a Busy controller");
  TEST_ASSERT_FALSE_MESSAGE(link.takeSent(f), "the BA retry brings nothing along with it");
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, link.aliveOffers,
                                   "and the retry does not invent a heartbeat either");
  TEST_ASSERT_EQUAL_UINT32(2, link.requestOffers);
  TEST_ASSERT_FALSE(display.synced());
  TEST_ASSERT_FALSE(display.registered());
}

void setUp(void) {}
void tearDown(void) {}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_late_registration_ack_cannot_revive_a_torn_down_session);
  RUN_TEST(test_late_reassert_ack_cannot_clear_the_next_session_restore);
  RUN_TEST(test_bare_first_69_gets_one_discovery_ba_but_never_unlocks_output);
  RUN_TEST(test_a_ping_on_the_heartbeat_boundary_still_yields_exactly_one_b9);
  RUN_TEST(test_busy_one_shot_bootstrap_is_bounded_under_repeated_01_and_69);
  RUN_TEST(test_rejected_one_shot_bootstrap_is_bounded_under_repeated_01_and_69);
  RUN_TEST(test_busy_ba_retries_only_ba_and_never_grows_a_b9);
  return UNITY_END();
}
