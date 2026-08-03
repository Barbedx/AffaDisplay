// The ONE sync FSM, driven against BOTH profiles through the real panel classes — so the
// constants in carminat/ and updatelist/ are under test as well as the machine in core/.
//
// The four properties this suite exists to keep:
//   * the AFFA3 NAV hello reply is byte-exact, B0 x3, and paced from the measured
//     61 11 00 request rather than sent as one blocking burst;
//   * the one-shot BA bootstrap leaves only for the panel's 61 11 01 START phase;
//   * the heartbeat is a wall-clock deadline, not a call counter, on both families;
//   * the peer watchdog is milliseconds — 4999 up, 5001 down — and FUNCSREG goes with it.
//
// And the one that is a live bug in the legacy shim: a SHORT-DLC 0x3CF frame must not
// latch START off memory the panel never sent.

#include "../affa_test_support.h"

#include "carminat/CarminatDisplay.h"
#include "carminat/CarminatConstants.h"
#include "updatelist/UpdateListDisplay.h"
#include "updatelist/UpdateListConstants.h"

using namespace affa;
using affatest::mk;
using affatest::drain;
using affatest::expectFrame;
using affatest::pump;

namespace {

template <class Panel>
struct Rig {
  LoopbackLink<256> link;
  affatest::FakeClock clk;
  Panel d;
  Rig() : d(link, clk) {}
};

using CarRig = Rig<CarminatDisplay>;
using UlRig  = Rig<UpdateListDisplay>;

struct LegacyCarRig {
  LoopbackLink<256> link;
  affatest::FakeClock clk;
  CarminatDisplay d;
  LegacyCarRig()
      : d(link, clk, carminat::CarminatHelloProfile::MeganeCanLegacy70B0B0) {}
};

static const Frame kCarminatHello =
    {0x3AF, 8, {0xB0, 0x14, 0x11, 0x00, 0x1F, 0x00, 0x00, 0x00}, false};

static const Frame kCarminatAlive =
    {0x3AF, 8, {0xB9, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, false};

static const Frame kCarminatRequest =
    {0x3AF, 8, {0xBA, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, false};

static const Frame kCarminatRegText =
    {0x151, 8, {0x70, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, false};

static const Frame kCarminatRegNav =
    {0x1F1, 8, {0x70, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, false};

static const Frame kCarminatPowerOn =
    {0x151, 8, {0x03, 0x52, 0x09, 0x00, 0x00, 0x00, 0x00, 0x00}, false};

static const Frame kCarminatTime1000 =
    {0x151, 8, {0x05, 0x56, 0x31, 0x30, 0x30, 0x30, 0x00, 0x00}, false};

void expectNoFrame(CarRig& r, const char* what) {
  Frame f;
  TEST_ASSERT_FALSE_MESSAGE(r.link.takeSent(f), what);
}

// Emit the captured nonblocking AFFA3 announce after a complete good request.  The helper
// intentionally advances the test clock rather than waiting: these are protocol deadlines,
// not delays inside poll().  It leaves any registration/payload traffic behind the third B0
// in the link so callers can assert its order separately.
void finishCarminatHello(CarRig& r) {
  r.clk.advance(carminat::kHelloFirstDelayMs);
  r.d.poll();
  expectFrame(r.link, kCarminatHello, "Carminat B0 1/3 at +31 ms");

  r.clk.advance(carminat::kHelloFrameGapMs);
  r.d.poll();
  expectFrame(r.link, kCarminatHello, "Carminat B0 2/3 at +62 ms");

  r.clk.advance(carminat::kHelloFrameGapMs);
  r.d.poll();
  expectFrame(r.link, kCarminatHello, "Carminat B0 3/3 at +93 ms");
}

// Registration ACKs are modelled by LoopbackLink on the following poll.  Keep the helper
// bounded so a regression cannot turn a unit test into an unbounded spin.
void finishCarminatRegistration(CarRig& r) {
  for (int i = 0; i < 8 && !r.d.registered(); ++i) r.d.poll();
  TEST_ASSERT_TRUE_MESSAGE(r.d.registered(), "Carminat functions should register");
}

// Counts what left on the sync id, by leading byte. Everything else is counted separately
// so a stray frame cannot hide inside "other".
struct SyncTally {
  int hello = 0, alive = 0, request = 0, other = 0;
};

template <class L>
SyncTally tally(L& link, uint16_t syncId, uint8_t aliveByte, uint8_t requestByte) {
  SyncTally t;
  Frame f;
  while (link.takeSent(f)) {
    if (f.id != syncId) { ++t.other; continue; }
    if (f.data[0] == kRegisterByte || f.data[0] == 0xB0) ++t.hello;
    else if (f.data[0] == aliveByte) ++t.alive;
    else if (f.data[0] == requestByte) ++t.request;
    else ++t.other;
  }
  return t;
}

}  // namespace

// ---------------------------------------------------------------------------
// Hello
// ---------------------------------------------------------------------------

void test_carminat_hello_is_three_paced_b0_frames(void) {
  CarRig r;
  r.d.begin();
  r.d.poll();
  drain(r.link);

  r.link.inject(affatest::panelSyncRequest());
  r.d.poll();

  // The monitor capture is B0 at +31, +62 and +93 ms.  00 must stay gated until the
  // third frame has actually been offered to CAN; no delay()/busy-wait is allowed here.
  TEST_ASSERT_FALSE_MESSAGE(r.d.synced(), "00 is not usable before its B0 announce");
  expectNoFrame(r, "no B0 in the same poll as 61 11 00");
  r.clk.advance(carminat::kHelloFirstDelayMs - 1);
  r.d.poll();
  expectNoFrame(r, "first B0 waits the full +31 ms");
  TEST_ASSERT_FALSE(r.d.synced());

  r.clk.advance(1);
  r.d.poll();
  expectFrame(r.link, kCarminatHello, "Carminat B0 1/3 at +31 ms");
  TEST_ASSERT_FALSE_MESSAGE(r.d.synced(), "first B0 does not unlock output");

  r.clk.advance(carminat::kHelloFrameGapMs - 1);
  r.d.poll();
  expectNoFrame(r, "second B0 waits a further +31 ms");
  TEST_ASSERT_FALSE(r.d.synced());

  r.clk.advance(1);
  r.d.poll();
  expectFrame(r.link, kCarminatHello, "Carminat B0 2/3 at +62 ms");
  TEST_ASSERT_FALSE_MESSAGE(r.d.synced(), "second B0 does not unlock output");

  r.clk.advance(carminat::kHelloFrameGapMs - 1);
  r.d.poll();
  expectNoFrame(r, "third B0 waits a further +31 ms");
  TEST_ASSERT_FALSE(r.d.synced());

  r.clk.advance(1);
  r.d.poll();
  expectFrame(r.link, kCarminatHello, "Carminat B0 3/3 at +93 ms");
  TEST_ASSERT_TRUE_MESSAGE(r.d.synced(), "third B0 completes good authorization");

  // REGISTRATION IS PART OF THE OPENING. [CAP] In all four OEM captures the radio puts
  // `151 70` on the wire 0.014-0.302 ms after B0#3 and `1F1 70` 0.311-0.587 ms after it,
  // with no application involvement whatsoever — this rig never rendered anything. So the
  // hello is exactly B0 x3 on the SYNC id, and the frames that follow it are the two
  // function registrations, not a fourth announce.
  expectFrame(r.link, kCarminatRegText, "151 registration leaves with B0#3");
  expectNoFrame(r, "the captured hello is exactly B0 x3, then 151 waits for its 551 ACK");
  TEST_ASSERT_FALSE_MESSAGE(r.d.registered(),
                            "registration is not COMPLETE until both 74 ACKs return");

  // Pin the profile data as well as the wire. UpdateList remains an immediate one-frame
  // legacy profile below; these delays are deliberately Carminat-only.
  TEST_ASSERT_EQUAL_UINT8(3, carminat::kSync.helloCount);
  TEST_ASSERT_TRUE_MESSAGE(carminat::kSync.registerAfterHello,
                           "the 0x70 probes belong to the opening, not to rendering");
  TEST_ASSERT_EQUAL_UINT32(carminat::kHelloMinMs, carminat::kSync.helloMinMs);
  TEST_ASSERT_EQUAL_UINT32(31, carminat::kSync.helloFirstDelayMs);
  TEST_ASSERT_EQUAL_UINT32(31, carminat::kSync.helloFrameGapMs);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(carminat::kHello[0], carminat::kHello[1], 8);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(carminat::kHello[1], carminat::kHello[2], 8);
}

void test_carminat_legacy_profile_is_immediate_70_b0_b0_but_still_requires_00(void) {
  // The compatibility selector preserves the historical MeganeCAN opening without making
  // it the default or weakening the captured profile's strict authorization gate.
  LegacyCarRig r;
  r.d.begin();
  r.d.poll();
  drain(r.link);

  r.link.inject(affatest::panelSyncStart());
  r.d.poll();
  expectFrame(r.link, kCarminatAlive, "legacy 01 bootstrap B9");
  expectFrame(r.link, kCarminatRequest, "legacy 01 bootstrap BA");
  Frame f;
  TEST_ASSERT_FALSE_MESSAGE(r.link.takeSent(f), "legacy 01 has no hello and no output");
  TEST_ASSERT_FALSE_MESSAGE(r.d.synced(), "legacy profile still rejects 61 11 01");

  r.link.inject(affatest::panelSyncRequest());
  r.d.poll();
  static const Frame kLegacyH0 =
      {0x3AF, 8, {0x70, 0x1A, 0x11, 0x00, 0x00, 0x00, 0x00, 0x01}, false};
  expectFrame(r.link, kLegacyH0, "legacy hello 70");
  expectFrame(r.link, kCarminatHello, "legacy hello first B0");
  expectFrame(r.link, kCarminatHello, "legacy hello second B0");
  // Same panel, same registration rule: the compatibility profile differs only in the
  // SPELLING and pacing of the three opening frames, so `151 70` still follows the last of
  // them without any application render. [CAP] B0#3 -> 151 70 measures 0.014-0.302 ms.
  expectFrame(r.link, kCarminatRegText, "legacy opening also registers 151 immediately");
  TEST_ASSERT_FALSE_MESSAGE(r.link.takeSent(f),
                            "legacy profile emits exactly 70/B0/B0 on the sync id");
  TEST_ASSERT_TRUE_MESSAGE(r.d.synced(), "legacy profile authorizes only after 61 11 00");
  TEST_ASSERT_TRUE_MESSAGE(carminat::kLegacyMeganeCanSync.registerAfterHello,
                           "only the hello spelling differs between the two profiles");
}

void test_updatelist_hello_is_exactly_one_frame(void) {
  UlRig r;
  r.d.begin();
  r.d.poll();
  drain(r.link);

  r.link.inject(affatest::panelSyncRequest());
  r.d.poll();

  static const Frame kH0 = {0x3DF, 8, {0x70, 0x1A, 0x11, 0x00, 0x00, 0x00, 0x00, 0x01}, false};
  expectFrame(r.link, kH0, "UpdateList hello");
  Frame f;
  TEST_ASSERT_FALSE_MESSAGE(r.link.takeSent(f), "UpdateList sends ONE hello, not three");
  TEST_ASSERT_EQUAL_UINT8(1, updatelist::kSync.helloCount);
}

// ---------------------------------------------------------------------------
// Panel-initiated Carminat startup
// ---------------------------------------------------------------------------

void test_carminat_waits_for_a_panel_message_before_transmitting(void) {
  CarRig r;
  r.d.begin();

  // The initial FAILED state is local bookkeeping, not permission to put BA probes on
  // the wire.  AFFA3 NAV starts only after the panel's 61 11 authorization request.
  r.d.poll();
  r.clk.advance(5000);
  r.d.poll();
  SyncTally t = tally(r.link, 0x3AF, 0xB9, 0xBA);
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, t.alive, "no heartbeat before the panel speaks");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, t.request, "no BA-per-second startup traffic");

  // A good request starts the measured B0 schedule, not an immediate heartbeat. Once the
  // third B0 completes authorization, B9 is profile-paced at 500 ms and BA stays absent.
  r.link.inject(affatest::panelSyncRequest());
  r.d.poll();
  expectNoFrame(r, "00 schedules B0; it does not transmit immediately");
  finishCarminatHello(r);
  TEST_ASSERT_TRUE(r.d.synced());

  // B0#3 carries the opening's registration with it — measured at 0.014-0.302 ms behind the
  // third announce in every capture — so it must be consumed here before the heartbeat
  // window is examined. Without a self-ACK rig the second probe stays behind 551.
  expectFrame(r.link, kCarminatRegText, "151 registration is part of the opening");
  expectNoFrame(r, "1F1 waits for the 551 ACK, not for the clock");

  r.clk.advance(carminat::kSyncIntervalMs - (3 * carminat::kHelloFrameGapMs) - 1);
  r.d.poll();
  expectNoFrame(r, "B9 is not early during the first 500-ms interval");
  r.clk.advance(1);
  r.d.poll();
  expectFrame(r.link, kCarminatAlive, "first Carminat B9 at +500 ms");
  expectNoFrame(r, "normal Carminat heartbeat never brings BA");
}

void test_carminat_ping_alone_never_starts_authentication(void) {
  // `69` says only that a panel is alive. It may not unlock the three auth frames, function
  // registration, or a render queued by application code before the panel sends its separate
  // GOOD `61 11 00` authorization request.
  //
  // What a first bare `69` DOES earn is one bounded B9 -> BA discovery transaction: in
  // "aknowledge offed display cONNECT OT POWER.csv" the display is already powered and
  // pinging into an empty bus, and the radio's opening move is precisely that pair. It is a
  // probe, not an answer and not a session — see test_carminat_never_pongs_between_heartbeats
  // for the separate proof that a `69` on an OPEN session produces nothing at all.
  CarRig r;
  r.d.begin();
  TEST_ASSERT_EQUAL(Result::Ok, r.d.setPower(true)); // held until real 61 11 00

  r.link.inject(affatest::panelPeerAlive());
  r.d.poll();
  SyncTally first = tally(r.link, 0x3AF, 0xB9, 0xBA);
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, first.alive, "the first bare 69 earns one discovery B9");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, first.request, "…followed by exactly one BA");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, first.hello, "69 must not start Carminat auth");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, first.other, "queued output stays held before auth");
  TEST_ASSERT_FALSE_MESSAGE(r.d.synced(), "a bare 69 must not clear FAILED");
  TEST_ASSERT_FALSE_MESSAGE(r.d.registered(), "and it registers no function");

  // ONE-SHOT MEANS ONE. An un-ACKed display repeats `69` on its own timer — 504 ms in the
  // captures, line rate on a bench with no CAN ACK — and none of those repeats may re-arm
  // the discovery pair or start a BA-per-second stream.
  for (int i = 0; i < 64; ++i) r.link.inject(affatest::panelPeerAlive());
  r.clk.advance(3000);
  r.d.poll();

  SyncTally t = tally(r.link, 0x3AF, 0xB9, 0xBA);
  TEST_ASSERT_FALSE_MESSAGE(r.d.synced(), "a bare 69 must not clear FAILED");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, t.hello, "69 must not start Carminat auth");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, t.alive, "the discovery B9 is spent, never repeated");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, t.request, "and never becomes a BA stream");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, t.other, "queued output stays held before auth");

  // Now the real authorization request arrives. It is still staged: only the final B0
  // releases registration and the held power command.
  r.link.inject(mk(0x3CF, {0x61, 0x11, 0x00, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3}));
  r.d.poll();
  expectNoFrame(r, "61 11 00 waits +31 ms for its first B0");
  TEST_ASSERT_FALSE_MESSAGE(r.d.synced(), "61 11 00 remains gated during the B0 sequence");
  finishCarminatHello(r);
  TEST_ASSERT_TRUE_MESSAGE(r.d.synced(), "third B0 opens the usable session");
  drain(r.link); // registration was allowed only after the third B0
}

void test_carminat_bootstrap_is_held_until_good_auth(void) {
  // 01 is discovery only for the capture-backed profile: exactly B9 + BA, no B0
  // announce, registration, or screen traffic. Only a later complete 61 11 00 begins
  // the staged announce that releases output.
  TEST_ASSERT_TRUE_MESSAGE(carminat::kSync.requireAuthRequest,
                           "Carminat requires a display-originated auth request");
  TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, carminat::kSync.authRequestByte2,
                                 "only 61 11 00 is good auth");
  TEST_ASSERT_TRUE_MESSAGE(carminat::kSync.oneShotResyncOnStart,
                           "Carminat preserves the one-shot legacy START bootstrap");
  TEST_ASSERT_FALSE_MESSAGE(carminat::kSync.helloOnNonAuthRequest,
                            "01 must not receive the captured B0 x3 announce");
  CarRig r;
  r.d.begin();
  r.d.setSelfAck(true);
  TEST_ASSERT_EQUAL(Result::Ok, r.d.setPower(true));
  TEST_ASSERT_EQUAL(Result::Ok, r.d.setTime("1000"));

  r.link.inject(affatest::panelSyncStart());
  r.d.poll();
  expectFrame(r.link, kCarminatAlive, "01 bootstrap B9");
  expectFrame(r.link, kCarminatRequest, "01 bootstrap BA");
  expectNoFrame(r, "01 gets exactly B9 + BA, not a hello");
  TEST_ASSERT_FALSE_MESSAGE(r.d.synced(), "01 is bootstrap, not a usable session");
  TEST_ASSERT_FALSE_MESSAGE(r.d.registered(), "no function can register from 01");

  // A no-ACK display may retransmit 01 at line rate. The B9 + BA pair is exactly once;
  // no per-second BA stream and no B0 reply may return.
  for (int i = 0; i < 64; ++i) r.link.inject(affatest::panelSyncStart());
  r.d.poll();
  expectNoFrame(r, "01 retransmissions are silent after their one B9 + BA pair");
  TEST_ASSERT_FALSE(r.d.synced());
  TEST_ASSERT_FALSE(r.d.registered());

  // Good 00 alone schedules B0x3. Its final frame releases registration; a measured
  // nonblocking 400-ms quiet interval then releases the held power/time work.
  r.link.inject(affatest::panelSyncRequest());
  r.d.poll();
  expectNoFrame(r, "00 waits until +31 ms for B0#1");
  finishCarminatHello(r);
  TEST_ASSERT_TRUE_MESSAGE(r.d.synced(), "good 00 opens only after B0#3");

  // B0#3 left a 151 registration behind it. Its ACK completion then emits 1F1; the final
  // registration ACK arms the payload gate, with no render prematurely overtaking it.
  expectFrame(r.link, kCarminatRegText, "151 registration follows B0#3");
  r.d.poll();
  expectFrame(r.link, kCarminatRegNav, "1F1 registration follows 151");
  r.d.poll();
  TEST_ASSERT_TRUE_MESSAGE(r.d.registered(), "functions register only after good 00");
  expectNoFrame(r, "registration completion starts a quiet payload interval");

  r.clk.advance(carminat::kPayloadAfterRegistrationMs - 1);
  r.d.poll();
  expectNoFrame(r, "power/time wait 399 ms after the final registration ACK");
  r.clk.advance(1);
  r.d.poll();
  expectFrame(r.link, kCarminatPowerOn, "power on at +400 ms");
  r.d.poll();
  expectFrame(r.link, kCarminatTime1000, "held time follows power on");
}

void test_carminat_acks_panel_registration_as_a_reflex_without_unlocking_output(void) {
  // THE 5C1 IS AN UNCONDITIONAL REFLEX, AND THE CAPTURES ARE UNAMBIGUOUS. Every single
  // display-originated `1C1` is answered with `5C1 74 00 …` in 0.288/0.453/0.470/0.483 ms
  // — 12/12 across the four OEM captures — and the FIRST one lands BETWEEN B0#1 and B0#2,
  // i.e. before the authorization phase has finished. There is no observable state in which
  // the radio hears a `1C1` and stays silent.
  //
  // This used to be gated on a prior `61 11` having been seen. That gate was safe only
  // because the captured displays all happen to lead with `61 11`: a display that led with
  // `1C1 70` would have gone unanswered for ever, and its channel registration would never
  // have completed. Scope now comes from the id (exactly 0x1C1) and from "not one of ours".
  //
  // The gate this test still defends is the OTHER one: answering the panel's channel is not
  // permission for OUR 151/1F1 registrations or for rendering. Those wait for 61 11 00 plus
  // its announce burst.
  CarRig r;
  r.d.begin();
  TEST_ASSERT_EQUAL(Result::Ok, r.d.setPower(true)); // held application work

  static const Frame kPanelRegistrationAck =
      {0x5C1, 8, {0x74, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, false};
  Frame f;

  // Nothing at all has been exchanged yet — no 61 11, no 69. The reflex still fires.
  r.link.inject(mk(0x1C1, {0x70, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3}));
  r.d.poll();
  expectFrame(r.link, kPanelRegistrationAck, "5C1 answers 1C1 before any 61 11 arrives");
  TEST_ASSERT_FALSE_MESSAGE(r.link.takeSent(f), "the reflex is one 74 and nothing else");
  TEST_ASSERT_FALSE_MESSAGE(r.d.synced(), "1C1 is the PANEL's registration, not ours");
  TEST_ASSERT_FALSE_MESSAGE(r.d.registered(), "our function registration remains locked");

  r.link.inject(affatest::panelSyncStart());
  r.d.poll();
  drain(r.link); // one B9/BA; 01 has no B0 announce and held power stays absent

  // ONE ACK PER FRAME, not one per session: the panel re-registers its channel and gets
  // answered again. [CAP] every 1C1 in the corpus has its own 5C1.
  r.link.inject(mk(0x1C1, {0x70, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3}));
  r.d.poll();
  expectFrame(r.link, kPanelRegistrationAck, "01 bootstrap panel registration ACK");
  TEST_ASSERT_FALSE_MESSAGE(r.d.synced(), "1C1 ACK is not good authorization");
  TEST_ASSERT_FALSE_MESSAGE(r.d.registered(), "our function registration remains locked");
  TEST_ASSERT_FALSE_MESSAGE(r.link.takeSent(f), "no 151/1F1 payload may follow the 5C1");
}

void test_carminat_ignores_unknown_full_auth_until_00(void) {
  // The capture-backed profile accepts only 61 11 00 for its B0 announce. An unknown
  // third byte is neither a B9/BA bootstrap (reserved for 01) nor authorization.
  CarRig r;
  r.d.begin();
  r.d.poll();
  drain(r.link);

  r.link.inject(mk(0x3CF, {0x61, 0x11, 0x5A, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3}));
  r.d.poll();
  SyncTally t = tally(r.link, 0x3AF, 0xB9, 0xBA);
  TEST_ASSERT_FALSE_MESSAGE(r.d.synced(), "unknown auth never unlocks the session");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, t.hello, "unknown 61 11 xx gets no B0 announce");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, t.alive, "unknown auth gets no bootstrap B9");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, t.request, "unknown auth gets no BA");
}

void test_carminat_does_not_cancel_start_pair_when_00_follows_immediately(void) {
  // Legacy leaves START set until its tick emits B9+BA, even when 00 follows 01 before
  // that tick. Keep the one pair, but defer usable authorization until 00's paced hello.
  CarRig r;
  r.d.begin();
  r.link.inject(affatest::panelSyncStart());
  r.link.inject(affatest::panelSyncRequest());
  r.d.poll();

  TEST_ASSERT_FALSE_MESSAGE(r.d.synced(), "00's staged B0 sequence still gates output");
  expectFrame(r.link, kCarminatAlive, "START B9 survives an immediately following 00");
  expectFrame(r.link, kCarminatRequest, "START BA survives an immediately following 00");
  expectNoFrame(r, "01 still contributes no B0 announce");

  finishCarminatHello(r);
  TEST_ASSERT_TRUE_MESSAGE(r.d.synced(), "00 becomes usable only after B0#3");
  // The only thing B0#3 is allowed to bring with it is the opening's first registration
  // probe (0.014-0.302 ms behind it in every capture). In particular NOT a second BA: the
  // START pair is spent, and completing the announce burst does not re-arm it.
  expectFrame(r.link, kCarminatRegText, "B0#3 carries the opening's 151 registration");
  expectNoFrame(r, "the one START BA is not retried");
}

// ---------------------------------------------------------------------------
// THE SHORT-DLC TRAP
// ---------------------------------------------------------------------------

void test_short_dlc_carminat_auth_request_stays_silent(void) {
  // The byte that says GOOD auth must actually be on the wire. A DLC-2 `61 11` cannot be
  // treated as `61 11 00` based on stale buffer memory, or ESP32 would answer bad auth.
  CarRig r;
  r.d.begin();
  r.d.poll();
  drain(r.link);

  Frame f = mk(0x3CF, {0x61, 0x11, 0x00, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3});
  f.len = 2;
  r.link.inject(f);
  r.clk.advance(1000);
  r.d.poll();

  SyncTally t = tally(r.link, 0x3AF, 0xB9, 0xBA);
  TEST_ASSERT_FALSE_MESSAGE(r.d.synced(), "Carminat needs byte 2: only full 61 11 00 authenticates");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, t.hello, "a short request gets no hello reply");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, t.alive, "and no heartbeat");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, t.request,
                                "a short request must not provoke BA");
}

void test_short_dlc_peer_alive_is_honoured(void) {
  // DLC 1 `69`. data[0] is the ENTIRE test on this channel; nothing else may be read.
  CarRig r;
  r.d.begin();
  r.link.inject(affatest::panelSyncRequest());
  r.d.poll();
  finishCarminatHello(r);
  drain(r.link);

  for (uint32_t s = 1; s <= 6; ++s) {
    Frame f = mk(0x3CF, {0x69, 0, 0, 0, 0, 0, 0, 0});
    f.len = 1;
    r.link.inject(f);
    r.clk.advance(1000);
    r.d.poll();
    TEST_ASSERT_TRUE_MESSAGE(r.d.synced(), "a DLC-1 ping must hold the link up");
  }
}

void test_a_foreign_cluster_token_is_not_answered(void) {
  // §1.1: one OEM cluster says `61 23`, not `61 11`. We would never answer it, and it must
  // not clear FAILED either.
  CarRig r;
  r.d.begin();
  r.d.poll();
  drain(r.link);

  Frame f = mk(0x3CF, {0x61, 0x23, 0, 0, 0, 0, 0, 0});
  f.len = 2;
  r.link.inject(f);
  r.d.poll();

  SyncTally t = tally(r.link, 0x3AF, 0xB9, 0xBA);
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, t.hello, "a foreign token gets no hello");
  TEST_ASSERT_FALSE_MESSAGE(r.d.synced(), "and does not bring the link up");
}

// ---------------------------------------------------------------------------
// Heartbeat pacing — both families
// ---------------------------------------------------------------------------

void test_updatelist_heartbeat_is_paced_by_the_clock(void) {
  // test_core proves this for the Carminat profile with a million polls. The point of
  // repeating it here is the OTHER profile: a call-counting implementation would emit a
  // storm of 0x79 and then tear the link down.
  UlRig r;
  r.d.begin();
  r.link.inject(affatest::panelSyncRequest());
  r.d.poll();
  drain(r.link);

  for (uint32_t i = 0; i < 20000; ++i) {
    r.clk.t = 1 + (i * 1000) / 20000;          // sweep 1 .. 1000 ms
    if ((i % 5000) == 0) r.link.inject(affatest::panelPeerAlive());
    r.d.poll();
    TEST_ASSERT_TRUE(r.d.synced());
  }
  SyncTally t = tally(r.link, 0x3DF, 0x79, 0x7A);
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, t.alive, "exactly one 0x79 per simulated second");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, t.request, "and no request while the link is up");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, t.other, "nothing else belongs on 0x3DF here");
}

void test_a_stalled_loop_does_not_produce_a_catch_up_burst(void) {
  // `_nextSyncMs = now + interval`, never `+=`. A caller that stopped calling poll() for
  // ten seconds owes the panel one heartbeat, not ten.
  CarRig r;
  r.d.begin();
  r.link.inject(affatest::panelSyncRequest());
  r.d.poll();
  finishCarminatHello(r);
  drain(r.link);

  r.clk.advance(10000);
  r.link.inject(affatest::panelPeerAlive());
  r.d.poll();
  SyncTally t = tally(r.link, 0x3AF, 0xB9, 0xBA);
  // ONE frame, not eleven and not two. The stall owes the panel a single paced heartbeat,
  // and the injected `69` adds nothing to it: the OEM radio's B9 is a free-running 500.08 ms
  // timer, not a reply (see test_carminat_never_pongs_between_heartbeats). A `+=` pacing bug
  // would show up here as ten frames; a pong would show up as two.
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, t.alive, "one paced heartbeat, not ten and not a pong");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, t.request, "and a stalled loop never owes a BA");
}

// ---------------------------------------------------------------------------
// THE RADIO DOES NOT ANSWER PINGS — SyncProfile::replyToPing is false everywhere
//
// This section used to assert the opposite, on the strength of MeganeCAN's driver calling
// tick() from its 0x69 handler. Four OEM captures of a real radio talking to a real display
// disprove it outright:
//
//   * the radio's `3AF B9 00` inter-frame time is 499.94 / 500.02 / 500.08 / 500.13 /
//     500.18 / 500.24 / 500.30 ms — a free-running timer with sigma <= 0.5 ms;
//   * the display's `3CF 69` runs on its OWN, slower and less stable clock: 503.7 / 504.0 /
//     504.4 ms, with 512 and 520 ms excursions in "aknowledge on on display.csv";
//   * so the 69 DRIFTS through a full phase cycle relative to the B9 — in one capture the
//     two frames pass within 0.023 ms of each other — and the B9 cadence never flinches.
//
// A reply cannot drift past the thing it replies to. The B9 is a metronome; the 69 is a
// separate metronome. These tests pin exactly that: a ping produces no frame, and above all
// it does not move the heartbeat's phase.
// ---------------------------------------------------------------------------

void test_carminat_never_pongs_between_heartbeats(void) {
  // Mid-interval ping. Nothing may leave, and — the stronger half — the heartbeat must
  // still land on the deadline it already owned, not 500 ms after the ping. A pong
  // implementation consumes the cadence and would shift that deadline.
  CarRig r;
  r.d.begin();
  r.link.inject(affatest::panelSyncRequest());   // t = 0, so the first B9 is due at t = 500
  r.d.poll();
  finishCarminatHello(r);                        // t = 93
  drain(r.link);

  r.clk.advance(200);                            // t = 293, well inside the interval
  r.link.inject(affatest::panelPeerAlive());
  r.d.poll();
  SyncTally t = tally(r.link, 0x3AF, 0xB9, 0xBA);
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, t.alive, "a 69 between heartbeats produces no B9");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, t.request, "and certainly no BA");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, t.other, "the ping is answered with silence");

  // THE PHASE IS THE POINT. 500 ms after the REQUEST, not 500 ms after the ping.
  r.clk.t = carminat::kSyncIntervalMs - 1;       // t = 499
  r.d.poll();
  expectNoFrame(r, "the free-running heartbeat is not early either");
  r.clk.advance(1);                              // t = 500
  r.d.poll();
  expectFrame(r.link, kCarminatAlive, "B9 keeps its own phase: the ping did not reset it");
  expectNoFrame(r, "one heartbeat, and nothing rides along with it");
}

void test_a_ping_storm_never_moves_the_free_running_heartbeat(void) {
  // An unacknowledged panel repeats `69` at line rate — 126 copies in 32 ms measured on the
  // bench. Under the old pong model this was the storm trap and the answer was "one pong,
  // floored by AFFA_PING_REPLY_MIN_MS". Measured against a real radio the answer is
  // stronger and simpler: ZERO. A storm of pings changes neither the frame count nor the
  // heartbeat's phase.
  CarRig r;
  r.d.begin();
  r.link.inject(affatest::panelSyncRequest());   // first B9 due at t = 500
  r.d.poll();
  finishCarminatHello(r);
  drain(r.link);

  r.clk.advance(1);                              // t = 94, clear of the paced tick
  for (int i = 0; i < 126; ++i) r.link.inject(affatest::panelPeerAlive());
  r.d.poll();                                    // pumpRx drains the whole burst
  SyncTally t = tally(r.link, 0x3AF, 0xB9, 0xBA);
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, t.alive, "126 pings in one burst: NO pong at all");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, t.request, "and no BA");
  TEST_ASSERT_TRUE_MESSAGE(r.d.synced(), "a ping storm is still liveness: the link stays up");

  r.clk.t = carminat::kSyncIntervalMs - 1;
  r.d.poll();
  expectNoFrame(r, "126 pings did not pull the heartbeat forward");
  r.clk.advance(1);
  r.d.poll();
  expectFrame(r.link, kCarminatAlive, "…nor push it back: B9 lands on its own 500 ms");
}

void test_neither_family_pongs(void) {
  // Pinned in BOTH profiles AND on the wire for both, so flipping one flag cannot pass
  // unnoticed. Carminat's flag was true until the four captures above disproved it; with it
  // on we emitted the paced B9 AND a second one ~4 ms later, twice the OEM rate.
  TEST_ASSERT_FALSE_MESSAGE(carminat::kSync.replyToPing,
                            "Carminat's B9 is a free-running 500.08 ms timer, not a reply");
  // The compatibility profile is the one place the old behaviour survives, ON PURPOSE: it
  // exists to reproduce the MeganeCAN driver's wire for a panel that demonstrated it. Pin
  // it so "neither family pongs" is a statement about the shipped defaults, not a claim
  // that the flag no longer exists.
  TEST_ASSERT_TRUE_MESSAGE(carminat::kLegacyMeganeCanSync.replyToPing,
                           "the legacy compatibility profile keeps the historical pong");
  TEST_ASSERT_FALSE_MESSAGE(updatelist::kSync.replyToPing, "UpdateList does not pong either");

  CarRig c;
  c.d.begin();
  c.link.inject(affatest::panelSyncRequest());
  c.d.poll();
  finishCarminatHello(c);
  drain(c.link);
  c.clk.advance(200);
  c.link.inject(affatest::panelPeerAlive());
  c.d.poll();
  SyncTally tc = tally(c.link, 0x3AF, 0xB9, 0xBA);
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, tc.alive, "Carminat: a ping between ticks is silent");

  UlRig r;
  r.d.begin();
  r.link.inject(affatest::panelSyncRequest());
  r.d.poll();
  drain(r.link);

  r.clk.advance(400);
  r.link.inject(affatest::panelPeerAlive());
  r.d.poll();
  SyncTally t = tally(r.link, 0x3DF, 0x79, 0x7A);
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, t.alive, "UpdateList: a ping between ticks is silent too");
}

void test_passive_never_pongs(void) {
  // A real radio owns the handshake. Passive injects data and answers NOTHING — pings
  // included, or we and the radio would double-answer every ping on a vehicle bus.
  CarRig r;
  r.d.begin();
  r.d.setPassive(true);
  r.clk.advance(400);
  r.link.inject(affatest::panelPeerAlive());
  r.d.poll();
  Frame f;
  TEST_ASSERT_FALSE_MESSAGE(r.link.takeSent(f), "passive transmits nothing, pong included");
}

// ---------------------------------------------------------------------------
// The peer watchdog is milliseconds
// ---------------------------------------------------------------------------

namespace {

// Brings a Carminat rig through the captured staggered auth, registration, and payload
// gate. The panel request is still at t=0, so its peer deadline remains exactly
// AFFA_PEER_TIMEOUT_MS despite the later protocol clock advances.
void armed(CarRig& r) {
  r.clk.t = 0;
  r.d.begin();
  r.link.inject(affatest::panelSyncRequest());
  r.d.poll();
  r.d.setSelfAck(true);
  (void)r.d.setPower(true);
  finishCarminatHello(r);
  finishCarminatRegistration(r);
  r.clk.advance(carminat::kPayloadAfterRegistrationMs);
  affatest::pumpUntilIdle(r.d);
  TEST_ASSERT_TRUE(r.d.registered());
  drain(r.link);
}

}  // namespace

void test_recovery_reasserts_cached_power_before_held_time(void) {
  // A power command that reached the panel is desired library state, not a one-shot that
  // the application must remember to resend. After the panel starts a new session, the
  // library must restore it internally before a time render that was held for recovery.
  CarRig r;
  armed(r); // power on is ACKed/cached and the first session is idle

  r.link.inject(affatest::panelSyncStart());
  r.d.poll();
  expectFrame(r.link, kCarminatAlive, "lost session discovery B9");
  expectFrame(r.link, kCarminatRequest, "lost session discovery BA");
  expectNoFrame(r, "lost 01 has no B0 or application payload");
  TEST_ASSERT_FALSE(r.d.synced());
  TEST_ASSERT_FALSE(r.d.registered());

  TEST_ASSERT_EQUAL(Result::Ok, r.d.setTime("1000")); // held behind the new session
  r.link.inject(affatest::panelSyncRequest());
  r.d.poll();
  expectNoFrame(r, "new 00 waits for its staged B0 announce");
  finishCarminatHello(r);

  // The re-registration burst must precede both the internally restored power and the
  // held time. Its final ACK, not B0#3, is the start of the 400-ms payload deadline.
  expectFrame(r.link, kCarminatRegText, "recovery 151 registration");
  r.d.poll();
  expectFrame(r.link, kCarminatRegNav, "recovery 1F1 registration");
  r.d.poll();
  TEST_ASSERT_TRUE(r.d.registered());
  expectNoFrame(r, "nothing renders with the final registration ACK");

  r.clk.advance(carminat::kPayloadAfterRegistrationMs - 1);
  r.d.poll();
  expectNoFrame(r, "cached power and held time wait the full 400-ms gate");

  r.clk.advance(1);
  r.d.poll();
  expectFrame(r.link, kCarminatPowerOn, "cached power is restored before held time");
  r.d.poll();
  expectFrame(r.link, kCarminatTime1000, "held time follows the restored power");
}

void test_peer_deadline_holds_at_4999ms(void) {
  CarRig r;
  armed(r);
  r.clk.t = AFFA_PEER_TIMEOUT_MS - 1;          // 4999
  r.d.poll();
  TEST_ASSERT_TRUE_MESSAGE(r.d.synced(), "4999 ms of silence is still inside the window");
  TEST_ASSERT_TRUE_MESSAGE(r.d.registered(), "and FUNCSREG survives");
}

void test_peer_deadline_fires_at_5001ms_and_drops_funcsreg(void) {
  CarRig r;
  armed(r);
  r.clk.t = AFFA_PEER_TIMEOUT_MS + 1;          // 5001
  r.d.poll();
  TEST_ASSERT_FALSE_MESSAGE(r.d.synced(), "peer loss is declared on the CLOCK");
  TEST_ASSERT_FALSE_MESSAGE(r.d.registered(),
                            "FUNCSREG does not survive a resync — the panel forgot us too");
  // Carminat recovery waits silently for the panel's next 61 11 instead of spraying BA.
  drain(r.link);
  r.clk.advance(1000);
  r.d.poll();
  SyncTally t = tally(r.link, 0x3AF, 0xB9, 0xBA);
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, t.alive, "recovery is quiet until the panel speaks");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, t.request, "FAILED never re-arms periodic BA on Carminat");

  r.link.inject(affatest::panelSyncRequest());
  r.d.poll();
  expectNoFrame(r, "recovery 00 waits for the captured B0 schedule");
  finishCarminatHello(r);
  TEST_ASSERT_TRUE_MESSAGE(r.d.synced(), "the next panel request restarts the session");
  drain(r.link); // B0#3 also releases the fresh registration probes
}

void test_peer_loss_is_not_a_poll_count(void) {
  // The defect this library exists to make impossible: legacy decremented
  // `static int8_t timeout = SYNC_TIMEOUT` once per tick() CALL, so from a free-running
  // loop "five seconds" expired in milliseconds.
  CarRig r;
  armed(r);
  for (int i = 0; i < 200000; ++i) r.d.poll();      // clock frozen
  TEST_ASSERT_TRUE_MESSAGE(r.d.synced(), "200000 polls at t=0 must not expire anything");
  TEST_ASSERT_TRUE(r.d.registered());
}

// ---------------------------------------------------------------------------
// The two request arguments look symmetrical and are not
// ---------------------------------------------------------------------------

void test_the_request_argument_asymmetry_is_preserved(void) {
  // Carminat's `BA 00 00 …` is 0xBA plus seven filler bytes that merely happen to be zero.
  // UpdateList's `7A 01` carries a genuine argument. Harmonising them would change the
  // wire for one of the two families.
  TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, carminat::kSync.requestArg, "Carminat requestArg");
  TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, carminat::kSync.filler, "Carminat filler");
  TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x01, updatelist::kSync.requestArg, "UpdateList requestArg");
  TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x81, updatelist::kSync.filler, "UpdateList filler");

  // data[1] of the HEARTBEAT is a literal 0x00 in both families and is NOT the filler.
  UlRig r;
  r.d.begin();
  r.d.poll();
  static const Frame kAlive =
      {0x3DF, 8, {0x79, 0x00, 0x81, 0x81, 0x81, 0x81, 0x81, 0x81}, false};
  expectFrame(r.link, kAlive, "UpdateList heartbeat: 79 00 81 x6, not 79 81 81 x6");
}

// ---------------------------------------------------------------------------

void setUp(void) {}
void tearDown(void) {}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_carminat_hello_is_three_paced_b0_frames);
  RUN_TEST(test_carminat_legacy_profile_is_immediate_70_b0_b0_but_still_requires_00);
  RUN_TEST(test_updatelist_hello_is_exactly_one_frame);
  RUN_TEST(test_carminat_waits_for_a_panel_message_before_transmitting);
  RUN_TEST(test_carminat_ping_alone_never_starts_authentication);
  RUN_TEST(test_carminat_bootstrap_is_held_until_good_auth);
  RUN_TEST(test_carminat_acks_panel_registration_as_a_reflex_without_unlocking_output);
  RUN_TEST(test_carminat_ignores_unknown_full_auth_until_00);
  RUN_TEST(test_carminat_does_not_cancel_start_pair_when_00_follows_immediately);
  RUN_TEST(test_short_dlc_carminat_auth_request_stays_silent);
  RUN_TEST(test_short_dlc_peer_alive_is_honoured);
  RUN_TEST(test_a_foreign_cluster_token_is_not_answered);
  RUN_TEST(test_updatelist_heartbeat_is_paced_by_the_clock);
  RUN_TEST(test_a_stalled_loop_does_not_produce_a_catch_up_burst);
  RUN_TEST(test_carminat_never_pongs_between_heartbeats);
  RUN_TEST(test_a_ping_storm_never_moves_the_free_running_heartbeat);
  RUN_TEST(test_neither_family_pongs);
  RUN_TEST(test_passive_never_pongs);
  RUN_TEST(test_recovery_reasserts_cached_power_before_held_time);
  RUN_TEST(test_peer_deadline_holds_at_4999ms);
  RUN_TEST(test_peer_deadline_fires_at_5001ms_and_drops_funcsreg);
  RUN_TEST(test_peer_loss_is_not_a_poll_count);
  RUN_TEST(test_the_request_argument_asymmetry_is_preserved);
  return UNITY_END();
}
