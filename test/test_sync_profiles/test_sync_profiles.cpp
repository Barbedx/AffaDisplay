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

// THE DISPLAY REGISTERS ITS OWN CHANNEL FIRST, and that is now a precondition of ours.
// [CAP] measured 4/4 across the OEM captures: the display's `1C1 70` lands 0.81-1.55 ms
// after B0#1 — i.e. BETWEEN the first and second announce frames — we answer `5C1 74 00 …`
// within 0.25-0.48 ms (12/12), and only 60.69-61.34 ms later, after B0#3, does the radio put
// its own `151 70` on the wire. A rig that never injects the 1C1 is not modelling this panel
// at all: the library then correctly refuses to register, for ever.
static const Frame kPanelChannelReg =
    {0x1C1, 8, {0x70, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3}, false};
static const Frame kPanelChannelAck =
    {0x5C1, 8, {0x74, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, false};

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

  // The display opens ITS channel here, in the measured gap between B0#1 and B0#2, and the
  // reflex `5C1 74` leaves in the same poll that drained it — ahead of B0#2, because the RX
  // pump runs before the sync pump. See kPanelChannelReg for the capture evidence. This is
  // what unlocks our own `151 70` when B0#3 lands; without it nothing registers.
  r.link.inject(kPanelChannelReg);
  r.clk.advance(carminat::kHelloFrameGapMs);
  r.d.poll();
  expectFrame(r.link, kPanelChannelAck, "5C1 74 reflex answers 1C1 70 ahead of B0#2");
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

// The whole measured opening, up to and including a COMPLETED registration, with the link
// drained and the clock left at request-time + 93 ms.
//
// THE KEEP-ALIVE DOES NOT EXIST BEFORE FUNCSREG. [CAP] in "aknowledge offed display.csv" the
// radio's first `3AF B9` is at 85055726 — 15.3 ms after the display's `5F1 74` completed the
// registration — and there is nothing whatsoever on 0x3AF between B0#3 and it. So a test
// about heartbeat PACING must first get the session all the way open; a rig that stops at
// B0#3 measures the absence of a heartbeat that has not been allowed to start, which is a
// different and much weaker statement. Self-ACK stands in for the display's 551/5F1.
void openCarminatSession(CarRig& r) {
  r.d.setSelfAck(true);
  // BOTH REQUESTS ARE AT t = 0, and that is deliberate: the announce costs no clock time
  // here, so every "500 ms after the request" assertion downstream still counts from t = 0.
  // The real panel spaces its two asks ~104 ms apart; that spacing is not what these tests
  // are about. See SyncProfile::helloRequiresAnnounce.
  affatest::carminatOpeningRequest(r.d, r.link);
  finishCarminatHello(r);
  finishCarminatRegistration(r);
  drain(r.link);
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

void test_carminat_hello_is_a_ba_announce_then_three_paced_b0_frames(void) {
  CarRig r;
  r.d.begin();
  r.d.poll();
  drain(r.link);

  // RENAMED from test_carminat_hello_is_three_paced_b0_frames, because the opening is no
  // longer three frames — it is four, and the first of them is ours.
  //
  // OUR `BA` COMES FIRST, AND THE BURST ANSWERS THE PANEL'S *NEXT* REQUEST. [CAP] measured
  // 4/4: the radio's `3AF BA` always precedes the `3CF 61 11 xx` that draws the burst, and
  // "aknowledge offed display cONNECT OT POWER.csv" spells the whole exchange out —
  //
  //   147305418  3CF 61 11 01     the display, already repeating every ~104 ms
  //   147328538  3AF BA 00        the radio announces into it
  //   147409570  3CF 61 11 01     the display asks AGAIN, 81 ms later
  //   147440321  3AF B0 14 11 ..  and THIS request draws the burst, 30.75 ms behind it
  //
  // Confirmed on real hardware 2026-08-04: without the announce the panel never opens its
  // own `1C1` channel and the session dies at "waiting for the 1C1" every time. So the
  // FIRST good request is answered with the bare BA and nothing else — it schedules no B0
  // at all, which is asserted below by letting its would-be +31 ms deadline pass in silence.
  r.link.inject(affatest::panelSyncRequest());
  r.d.poll();
  TEST_ASSERT_FALSE_MESSAGE(r.d.synced(), "00 is not usable before its B0 announce");
  expectFrame(r.link, kCarminatRequest, "the first 61 11 00 draws our BA announce");
  expectNoFrame(r, "no B0 in the same poll as the first 61 11 00");
  r.clk.advance(carminat::kHelloFirstDelayMs);
  r.d.poll();
  expectNoFrame(r, "and none at +31 ms either: the first request scheduled no burst");
  TEST_ASSERT_FALSE(r.d.synced());

  // The panel asks again on its own ~104 ms timer. THIS is the request the captures pace
  // the announce from, so every deadline below is measured from here.
  r.link.inject(affatest::panelSyncRequest());
  r.d.poll();

  // The monitor capture is B0 at +31, +62 and +93 ms.  00 must stay gated until the
  // third frame has actually been offered to CAN; no delay()/busy-wait is allowed here.
  TEST_ASSERT_FALSE_MESSAGE(r.d.synced(), "00 is not usable before its B0 announce");
  expectNoFrame(r, "no B0 in the same poll as the second 61 11 00 either");
  r.clk.advance(carminat::kHelloFirstDelayMs - 1);
  r.d.poll();
  expectNoFrame(r, "first B0 waits the full +31 ms");
  TEST_ASSERT_FALSE(r.d.synced());

  r.clk.advance(1);
  r.d.poll();
  expectFrame(r.link, kCarminatHello, "Carminat B0 1/3 at +31 ms");
  TEST_ASSERT_FALSE_MESSAGE(r.d.synced(), "first B0 does not unlock output");

  // THE DISPLAY'S OWN REGISTRATION LANDS HERE, and it does not disturb the B0 schedule.
  // [CAP] `1C1 70` is 0.81-1.55 ms behind B0#1 in all four captures, and the `5C1 74 00 …`
  // reflex 0.25-0.48 ms behind that — both comfortably inside the 31 ms gap, and neither one
  // pulls B0#2 forward. The reflex leaves from pumpRx, so it precedes anything pumpSync
  // decides to send in the same poll.
  r.link.inject(kPanelChannelReg);
  r.clk.advance(carminat::kHelloFrameGapMs - 1);
  r.d.poll();
  expectFrame(r.link, kPanelChannelAck, "the 1C1 70 reflex is answered on sight, mid-hello");
  expectNoFrame(r, "second B0 still waits its full further +31 ms");
  TEST_ASSERT_FALSE(r.d.synced());
  TEST_ASSERT_FALSE_MESSAGE(r.d.registered(),
                            "the PANEL's channel is open; ours is still not");

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

  // REGISTRATION IS PART OF THE OPENING, AND IT IS SECOND. [CAP] In all four OEM captures
  // the radio puts `151 70` on the wire 0.014-0.302 ms after B0#3 and `1F1 70` 0.311-0.587 ms
  // after it, with no application involvement whatsoever — this rig never rendered anything.
  // But it is 60.69-61.34 ms behind the display's `1C1 70`, never in front of it: we answer
  // the display's channel first and only then open ours. So the hello is exactly B0 x3 on the
  // SYNC id, and the frames that follow it are the two function registrations, not a fourth
  // announce.
  expectFrame(r.link, kCarminatRegText, "151 registration leaves with B0#3");
  expectNoFrame(r, "the captured hello is exactly B0 x3, then 151 waits for its 551 ACK");
  TEST_ASSERT_FALSE_MESSAGE(r.d.registered(),
                            "registration is not COMPLETE until both 74 ACKs return");

  // Pin the profile data as well as the wire. UpdateList remains an immediate one-frame
  // legacy profile below; these delays are deliberately Carminat-only.
  TEST_ASSERT_EQUAL_UINT8(3, carminat::kSync.helloCount);
  TEST_ASSERT_TRUE_MESSAGE(carminat::kSync.registerAfterHello,
                           "the 0x70 probes belong to the opening, not to rendering");
  // Pin the flag as well as the wire, on BOTH profiles: this is the same panel family, and
  // a silent flip back to a one-request opening is exactly the regression that cost a bench
  // session ("waiting for the 1C1", for ever).
  TEST_ASSERT_TRUE_MESSAGE(carminat::kSync.helloRequiresAnnounce,
                           "the BA must be on the wire before the burst means anything");
  TEST_ASSERT_TRUE_MESSAGE(carminat::kLegacyMeganeCanSync.helloRequiresAnnounce,
                           "the compatibility profile is the same panel, same BA-first rule");
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

  // THE ANNOUNCE IS A BARE `BA`, NOT A B9 + BA PAIR, and that is a library rule now rather
  // than a per-profile flag. [CAP] the reattach capture, "aknowledge offed
  // display.csv" at 84945066, shows the radio re-finding a sleeping display with a single
  // unprompted `3AF BA`. The two captures that DO show a B9 before registration are both
  // consistent with a free-running 500 ms heartbeat that happened to tick during the opening,
  // so the quieter reading is taken: BA asks the question, B9 only ever says "still here".
  r.link.inject(affatest::panelSyncStart());
  r.d.poll();
  expectFrame(r.link, kCarminatRequest, "legacy 01 bootstrap BA");
  Frame f;
  TEST_ASSERT_FALSE_MESSAGE(r.link.takeSent(f),
                            "legacy 01 has no B9, no hello and no output");
  TEST_ASSERT_FALSE_MESSAGE(r.d.synced(), "legacy profile still rejects 61 11 01");

  r.link.inject(affatest::panelSyncRequest());
  r.d.poll();
  static const Frame kLegacyH0 =
      {0x3AF, 8, {0x70, 0x1A, 0x11, 0x00, 0x00, 0x00, 0x00, 0x01}, false};
  expectFrame(r.link, kLegacyH0, "legacy hello 70");
  expectFrame(r.link, kCarminatHello, "legacy hello first B0");
  expectFrame(r.link, kCarminatHello, "legacy hello second B0");
  // NOT YET. The compatibility profile differs from the captured one only in the SPELLING
  // and pacing of the three opening frames — the registration RULE is shared, and it is the
  // display's own `1C1 70` that opens the door, not the end of the hello. This zero-gap
  // profile emits all three frames in one poll, so the display has had no opportunity to
  // register its channel and we must not have registered ours.
  TEST_ASSERT_FALSE_MESSAGE(r.link.takeSent(f),
                            "legacy profile emits exactly 70/B0/B0 on the sync id");
  TEST_ASSERT_TRUE_MESSAGE(r.d.synced(), "legacy profile authorizes only after 61 11 00");

  // Now the display opens its channel. [CAP] the reflex `5C1 74 00 …` answers it in
  // 0.25-0.48 ms, and our own `151 70` follows on its heels — the same order the captured
  // profile keeps, just without the 31 ms announce pacing in between.
  r.link.inject(kPanelChannelReg);
  r.d.poll();
  expectFrame(r.link, kPanelChannelAck, "legacy 5C1 74 reflex");
  expectFrame(r.link, kCarminatRegText, "151 70 follows the DISPLAY's registration, not hello");
  TEST_ASSERT_FALSE_MESSAGE(r.link.takeSent(f), "1F1 still waits for the 551 ACK");
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

void test_carminat_announces_into_a_silent_bus_slowly_and_ba_only(void) {
  CarRig r;
  // The heartbeat cannot start before registration completes (see below), so the rig has to
  // be able to finish one. Self-ACK is armed before anything is injected, exactly as the
  // instructions for a Carminat opening require.
  r.d.setSelfAck(true);
  r.d.begin();

  // The initial FAILED state is local bookkeeping, not permission to put BA probes on the
  // wire. AFFA3 NAV stays quiet for a full announce interval first.
  r.d.poll();
  r.clk.advance(carminat::kSync.announceWhenSilentMs - 1);
  r.d.poll();
  SyncTally quiet = tally(r.link, 0x3AF, 0xB9, 0xBA);
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, quiet.alive, "silent for the whole first interval");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, quiet.request, "no BA-per-second startup traffic");

  // BUT SILENCE IS NOT THE END STATE, and that is the correction. A display that has gone to
  // sleep never sends 61 11, so a node that waits for one waits for ever — measured on the
  // bench as rx 0 / tx 0, both sides waiting for the other. The OEM radio breaks that tie by
  // announcing into the silence.
  //
  // THE ANNOUNCE IS A BARE `BA`, NOT A PAIR. This used to assert alive == request, on the
  // strength of a B9 appearing near the BA in "aknowledge on on display.csv". The reattach
  // capture, "aknowledge offed display.csv" at 84945066, is the clean one — a display that
  // was asleep and is being re-found — and the radio's announce there is a single unprompted
  // `3AF BA` with NO B9 in front of it. B9 is the heartbeat of an ESTABLISHED session; on a
  // bus where the handshake has not started it is pure noise in the phase that can least
  // afford it. So the guarantee is "slow, bare and bounded": BA alone, at most one per
  // elapsed announce interval, never the BA-per-second storm waitForPanel was added to kill.
  r.clk.advance(5 * carminat::kSync.announceWhenSilentMs);
  for (int i = 0; i < 8; ++i) r.d.poll();
  SyncTally t = tally(r.link, 0x3AF, 0xB9, 0xBA);
  TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, t.request,
                                       "a silent bus is announced into, not waited on");
  TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(
      2, t.request, "one announce per elapsed interval, never a per-poll storm");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, t.alive, "the announce is a bare BA: no B9 accompanies it");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, t.hello, "and no B0 announce before a 61 11 00");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, t.other, "nothing else belongs on 0x3AF here");

  // A good request starts the measured B0 schedule, not an immediate heartbeat. Once the
  // third B0 completes authorization, B9 is profile-paced at 500 ms and BA stays absent.
  //
  // AND IT TAKES TWO REQUESTS. The slow silent-bus announce above is the waitForPanel probe,
  // not the handshake's own BA: it never latches `_unauthControlIssued`, so the panel's first
  // good `61 11 00` still buys one — and only one — announce of its own. [CAP] the radio's
  // BA precedes the `61 11 xx` that draws the burst in all four captures; see
  // SyncProfile::helloRequiresAnnounce and the transcript in affa_test_support.h.
  affatest::carminatOpeningRequest(r.d, r.link,
                                   "the first good 00 answers with the announce, not a burst");
  finishCarminatHello(r);
  TEST_ASSERT_TRUE(r.d.synced());

  // B0#3 carries the opening's registration with it — measured at 0.014-0.302 ms behind the
  // third announce in every capture — and the heartbeat waits for that registration to
  // COMPLETE, not merely to be sent. [CAP] in the reattach capture the radio's first B9 is
  // 15.3 ms after the display's `5F1 74`, with nothing at all on 0x3AF between B0#3 and it.
  // So both probes and both ACKs are driven out here before the heartbeat window is examined.
  expectFrame(r.link, kCarminatRegText, "151 registration is part of the opening");
  expectNoFrame(r, "1F1 waits for the 551 ACK, not for the clock");
  r.d.poll();
  expectFrame(r.link, kCarminatRegNav, "1F1 follows the 551 ACK for 151");
  r.d.poll();
  TEST_ASSERT_TRUE_MESSAGE(r.d.registered(), "the 5F1 ACK completes FUNCSREG");
  expectNoFrame(r, "completing registration emits nothing by itself");

  // The B9 phase is owned by the 61 11 00, not by registration: the panel request was the
  // last thing to arm _nextSyncMs, and it is 500 ms from THERE.
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
  // What a first bare `69` DOES earn is one bounded discovery probe: in "aknowledge offed
  // display cONNECT OT POWER.csv" the display is already powered and pinging into an empty
  // bus, and the radio answers by opening a discovery transaction. It is a probe, not an
  // answer and not a session — see test_carminat_never_pongs_between_heartbeats for the
  // separate proof that a `69` on an OPEN session produces nothing at all.
  //
  // AND THE PROBE IS A BARE `BA`, now as a library rule rather than a profile flag:
  // [CAP] the reattach capture "aknowledge offed display.csv" at 84945066 shows the radio
  // re-finding a sleeping display with one unprompted `3AF BA` and no B9 in front of it. The
  // B9s visible before registration in the other two captures are consistent with a
  // free-running 500 ms heartbeat ticking through the opening, so they are not evidence of a
  // pair. BA asks "is anyone there"; B9 only ever says "still here", and there is no session
  // to still be here in yet. `first.alive == 0` below is the whole of that assertion —
  // SyncProfile::bootstrapAliveFrame used to be read here beside it, which pinned the
  // CONFIGURATION rather than the wire and would have gone on passing if pumpUnauthControl()
  // had emitted a B9 anyway.
  CarRig r;
  r.d.begin();
  TEST_ASSERT_EQUAL(Result::Ok, r.d.setPower(true)); // held until real 61 11 00

  r.link.inject(affatest::panelPeerAlive());
  r.d.poll();
  SyncTally first = tally(r.link, 0x3AF, 0xB9, 0xBA);
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, first.request, "the first bare 69 earns exactly one BA");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, first.alive, "…and no B9 rides in front of it");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, first.hello, "69 must not start Carminat auth");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, first.other, "queued output stays held before auth");
  TEST_ASSERT_FALSE_MESSAGE(r.d.synced(), "a bare 69 must not clear FAILED");
  TEST_ASSERT_FALSE_MESSAGE(r.d.registered(), "and it registers no function");

  // ONE-SHOT MEANS ONE. An un-ACKed display repeats `69` on its own timer — 504 ms in the
  // captures, line rate on a bench with no CAN ACK — and none of those repeats may re-arm
  // the discovery probe or start a BA-per-second stream.
  for (int i = 0; i < 64; ++i) r.link.inject(affatest::panelPeerAlive());
  r.clk.advance(3000);
  r.d.poll();

  SyncTally t = tally(r.link, 0x3AF, 0xB9, 0xBA);
  TEST_ASSERT_FALSE_MESSAGE(r.d.synced(), "a bare 69 must not clear FAILED");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, t.hello, "69 must not start Carminat auth");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, t.alive, "and never grows a B9 on a repeat either");
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
  // THE FIRST REQUEST IS AN ANNOUNCE TRIGGER, WHATEVER ITS BYTE 2. It earns exactly one BA,
  // no B0 announce, no registration and no screen traffic; the panel's NEXT request is the
  // one that draws the burst and releases output. See SyncProfile::helloRequiresAnnounce.
  //
  // THE ANNOUNCE LOST ITS B9. [CAP] "aknowledge offed display.csv" at 84945066 — the
  // reattach, which is the cleanest look at a radio opening a conversation — is a bare
  // `3AF BA`.
  //
  // FIVE FLAG READS USED TO STAND HERE — requireAuthRequest, authRequestByte2,
  // oneShotResyncOnStart, helloOnNonAuthRequest and bootstrapAliveFrame — and four of the
  // five are gone with the fields. They pinned CONFIGURATION, which is the weaker thing to
  // pin: every one of them would have gone on passing while the FSM did something else
  // entirely. What replaces them is already below, on the wire, and always was: one BA and
  // nothing beside it, then silence under a request storm, then a burst.
  TEST_ASSERT_TRUE_MESSAGE(carminat::kSync.requireAuthRequest,
                           "Carminat requires a display-originated auth request");
  CarRig r;
  r.d.begin();
  r.d.setSelfAck(true);
  TEST_ASSERT_EQUAL(Result::Ok, r.d.setPower(true));
  TEST_ASSERT_EQUAL(Result::Ok, r.d.setTime("1000"));

  r.link.inject(affatest::panelSyncStart());
  r.d.poll();
  expectFrame(r.link, kCarminatRequest, "01 bootstrap BA");
  expectNoFrame(r, "01 gets exactly one BA — no B9 in front of it, and no hello");
  TEST_ASSERT_FALSE_MESSAGE(r.d.synced(), "01 is bootstrap, not a usable session");
  TEST_ASSERT_FALSE_MESSAGE(r.d.registered(), "no function can register from 01");

  // A no-ACK display may retransmit 01 at line rate. The BA is exactly once;
  // no per-second BA stream and no B0 reply may return.
  for (int i = 0; i < 64; ++i) r.link.inject(affatest::panelSyncStart());
  r.d.poll();
  expectNoFrame(r, "01 retransmissions are silent after their one BA");
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

void test_any_complete_61_11_xx_is_the_same_request(void) {
  // BYTE 2 IS NOT AN AUTHORIZATION GRADE, AND THIS TEST USED TO SAY THE OPPOSITE.
  //
  // It was `test_carminat_ignores_unknown_full_auth_until_00`, and it asserted that
  // `61 11 5A` produced NOTHING AT ALL: no BA, no burst, no session, until a `61 11 00`
  // arrived. That was the shape of defect this project keeps repeating — a special case
  // standing in for a general rule (HANDOFF.md, "How this project gets things wrong") — and
  // it was pinned as if it were a measurement.
  //
  // The measurement says otherwise. [CAP] "aknowledge offed display cONNECT OT POWER.csv":
  // the display repeats `3CF 61 11 01` sixteen times, never once sends `61 11 00`, and the
  // OEM radio answers with the ordinary B0 burst and completes the entire session. The low
  // bit reports the display's own state. Nothing in any capture reads byte 2 as permission,
  // and nothing in the corpus distinguishes 0x5A from 0x00 — so neither do we.
  //
  // 0x5A is deliberately a byte no capture contains: the rule under test is that the
  // library does not care.
  CarRig r;
  r.d.begin();
  r.d.poll();
  drain(r.link);

  const Frame unknownRequest = mk(0x3CF, {0x61, 0x11, 0x5A, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3});

  r.link.inject(unknownRequest);
  r.d.poll();
  SyncTally first = tally(r.link, 0x3AF, 0xB9, 0xBA);
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, first.request,
                                "an unknown byte 2 is a request: it earns the one BA announce");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, first.alive, "…with no B9 in front of it");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, first.hello,
                                "…and no burst, because our BA had not been sent yet");
  TEST_ASSERT_FALSE_MESSAGE(r.d.synced(), "the first request opens nothing on its own");

  // And the panel's NEXT request draws the burst — the same rule, on the same byte, that
  // affatest::carminatOpeningRequest() drives with `61 11 00`.
  r.link.inject(unknownRequest);
  r.d.poll();
  expectNoFrame(r, "the second request schedules B0; it transmits nothing at once");
  finishCarminatHello(r);
  TEST_ASSERT_TRUE_MESSAGE(r.d.synced(),
                           "a session opens on 61 11 5A exactly as it does on 61 11 00");
  drain(r.link);
}

// ---------------------------------------------------------------------------
// PHASE — the opening as one ordered value
// ---------------------------------------------------------------------------

void assertPhase(CarRig& r, Phase want, const char* what) {
  char msg[160];
  std::snprintf(msg, sizeof(msg), "%s: expected %s, got %s", what, phaseName(want),
                phaseName(r.d.phase()));
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(static_cast<uint8_t>(want),
                                  static_cast<uint8_t>(r.d.phase()), msg);
}

void test_phase_walks_the_measured_opening_in_order(void) {
  // THIS IS THE SAFETY NET FOR STEP 4 of docs/REFACTOR-PLAN.md. `phase()` is derived from
  // the nine booleans today, so it cannot currently disagree with them; what this test pins
  // is the derivation against THE WIRE — which frame moves the opening on, and in what
  // order. When the booleans are deleted and Phase becomes the stored truth, this is the
  // thing that says the new machine still describes the same panel.
  //
  // It walks the measured opening one frame at a time and deliberately does NOT use
  // openCarminatSession(): the helper injects the display's `1C1` inside the burst, which
  // is faithful to the capture and hides AwaitPeerChannel completely. That phase is the one
  // a stalled bench actually sits in ("waiting for the display's 1C1"), so it is worth the
  // hand-driven version.
  CarRig r;
  r.d.begin();
  // Self-ACK stands in for the display's `551`/`5F1`, and it has to be armed BEFORE the
  // 151 is offered — it is latched when the frame is handed to the link, not when the ACK
  // is due. Turning it on later leaves the probe waiting for an ACK that will never come.
  r.d.setSelfAck(true);
  assertPhase(r, Phase::Silent, "begin() has heard nothing and said nothing");

  // The panel's FIRST request arms our announce and is answered with nothing else. [CAP] 4/4.
  r.link.inject(affatest::panelSyncRequest());
  r.d.poll();
  expectFrame(r.link, kCarminatRequest, "the first request draws the BA announce");
  expectNoFrame(r, "…and nothing else");
  assertPhase(r, Phase::Announced, "our BA is out; the panel's next request draws the burst");

  // …and the panel's NEXT one schedules the burst.
  r.link.inject(affatest::panelSyncRequest());
  r.d.poll();
  expectNoFrame(r, "the second request schedules B0; it transmits nothing at once");
  assertPhase(r, Phase::HelloPending, "the burst is scheduled but not yet on the wire");

  r.clk.advance(carminat::kHelloFirstDelayMs);
  r.d.poll();
  expectFrame(r.link, kCarminatHello, "B0 1/3 at +31 ms");
  assertPhase(r, Phase::HelloPending, "one frame of three is not a burst");

  r.clk.advance(carminat::kHelloFrameGapMs);
  r.d.poll();
  expectFrame(r.link, kCarminatHello, "B0 2/3");
  assertPhase(r, Phase::HelloPending, "nor two of three");

  // THE BURST IS COMPLETE AND NOTHING FOLLOWS IT. No 1C1 has arrived, so our own
  // registration is correctly refused — this is the phase a bench stalls in when the panel
  // never received our announce, and before it had a name it cost a session to recognise.
  r.clk.advance(carminat::kHelloFrameGapMs);
  r.d.poll();
  expectFrame(r.link, kCarminatHello, "B0 3/3");
  expectNoFrame(r, "no 151 may leave before the display has opened its own channel");
  assertPhase(r, Phase::AwaitPeerChannel, "the burst is out; the display has not answered");

  // The display opens its channel. We reflex the `5C1 74` and only then register ours.
  r.link.inject(kPanelChannelReg);
  r.d.poll();
  expectFrame(r.link, kPanelChannelAck, "5C1 74 answers the display's 1C1 70");
  expectFrame(r.link, kCarminatRegText, "and unlocks our own 151 registration");
  assertPhase(r, Phase::Registering, "our probes are out, awaiting their ACKs");

  finishCarminatRegistration(r);
  drain(r.link);
  assertPhase(r, Phase::Settling, "registered, but inside the measured 400 ms quiet interval");

  r.clk.advance(carminat::kPayloadAfterRegistrationMs - 1);
  r.d.poll();
  assertPhase(r, Phase::Settling, "399 ms is not 400");
  r.clk.advance(1);
  r.d.poll();
  assertPhase(r, Phase::Ready, "and only now may an application render");
}

void test_phase_falls_back_when_the_panel_voids_the_session(void) {
  // LEAVING Ready IS THE HOOK STEP 6 NEEDS. The panel drops the session about every seven
  // minutes on the bench — fourteen times in a 96-minute soak — and it is invisible because
  // recovery works. There is exactly one edge here, and this test pins it so that the drop
  // snapshot can be hung on it without hunting through nine booleans for the moment.
  CarRig r;
  r.d.begin();
  openCarminatSession(r);
  r.clk.advance(carminat::kPayloadAfterRegistrationMs);
  r.d.poll();
  drain(r.link);
  assertPhase(r, Phase::Ready, "the soak's steady state");
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, r.d.sessionsLost(),
                                   "opening a session is not losing one");

  // Any complete `61 11 xx` while registered says the panel forgot us.
  r.link.inject(affatest::panelSyncStart());
  r.d.poll();
  assertPhase(r, Phase::HelloPending, "a voided session falls straight back to the burst");
  TEST_ASSERT_FALSE_MESSAGE(r.d.registered(), "and FUNCSREG goes with it");
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, r.d.sessionsLost(),
                                   "…and the drop is COUNTED, which is the whole point");

  // It re-opens on its own: our BA is long since on the wire, so this request draws the
  // burst directly rather than arming another announce.
  finishCarminatHello(r);
  r.link.inject(kPanelChannelReg);
  r.d.poll();
  drain(r.link);
  finishCarminatRegistration(r);
  drain(r.link);
  r.clk.advance(carminat::kPayloadAfterRegistrationMs);
  r.d.poll();
  assertPhase(r, Phase::Ready, "self-healed, which is why nobody noticed fourteen of these");
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, r.d.sessionsLost(),
                                   "recovering does not un-count the loss");
}

void test_carminat_does_not_cancel_the_start_announce_when_00_follows_immediately(void) {
  // Legacy leaves START set until its tick emits the bootstrap, even when 00 follows 01
  // before that tick. Keep the one announce, but defer usable authorization until 00's
  // paced hello.
  //
  // RENAMED from ..._start_pair_...: the bootstrap is no longer a pair. [CAP] the reattach
  // capture "aknowledge offed display.csv" at 84945066 opens with a bare `3AF BA`, so
  // the announce is BA-only and the B9 that used to lead is gone. What the test pins is
  // unchanged in strength — a 00 arriving in the same RX drain must neither cancel the START
  // announce nor duplicate it.
  CarRig r;
  r.d.begin();
  r.link.inject(affatest::panelSyncStart());
  r.link.inject(affatest::panelSyncRequest());
  r.d.poll();

  TEST_ASSERT_FALSE_MESSAGE(r.d.synced(), "00's staged B0 sequence still gates output");
  expectFrame(r.link, kCarminatRequest, "START BA survives an immediately following 00");
  expectNoFrame(r, "…and gains no B9, and 01 still contributes no B0 announce");

  // THE 00 IN THAT SAME DRAIN ARRIVED BEFORE OUR BA LEFT, so under helloRequiresAnnounce all
  // it can do is re-arm the announce 01 had already armed — which is precisely the property
  // this test exists to pin, now visible one layer further down: the arm is idempotent, so
  // two frames that both want an announce still produce exactly ONE BA. The burst then
  // answers the panel's next request, as measured 4/4 ([CAP], and the "cONNECT OT POWER"
  // transcript in affa_test_support.h).
  r.link.inject(affatest::panelSyncRequest());
  r.d.poll();
  expectNoFrame(r, "the answering 00 schedules B0; it transmits nothing itself");

  finishCarminatHello(r);
  TEST_ASSERT_TRUE_MESSAGE(r.d.synced(), "00 becomes usable only after B0#3");
  // The only thing B0#3 is allowed to bring with it is the opening's first registration
  // probe (0.014-0.302 ms behind it in every capture, and behind the display's own 1C1 that
  // finishCarminatHello injects). In particular NOT a second BA: the START announce is
  // spent, and completing the burst does not re-arm it.
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
  // Two requests with our BA between them — the measured opening. See
  // SyncProfile::helloRequiresAnnounce; the burst answers the panel's SECOND ask.
  affatest::carminatOpeningRequest(r.d, r.link);
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
  // Registration must COMPLETE before there is a heartbeat to stall in the first place —
  // see openCarminatSession() for the capture that pins B9 behind the display's 5F1.
  openCarminatSession(r);

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
  // t = 0 for the request, so the first B9 is due at t = 500; the opening (announce plus a
  // completed registration) leaves the clock at t = 93. Registration has to finish here or
  // there is no heartbeat at all to have a phase — that gate is change (3), and it does not
  // touch the property under test, which is that a ping never moves the phase.
  openCarminatSession(r);                        // t = 93

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
  // First B9 due at t = 500, counted from the 61 11 00 the opening starts with. Registration
  // is completed inside the helper because the heartbeat does not start before FUNCSREG.
  openCarminatSession(r);                        // t = 93

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
  // Open the session all the way to FUNCSREG. Stopping at B0#3 would leave the heartbeat
  // suppressed by change (3) and `tc.alive == 0` would then be true for a reason that has
  // nothing to do with pongs — a vacuous pass. Here B9 is armed and simply is not a reply.
  openCarminatSession(c);
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
  // The opening is two requests with our BA between them, both at t = 0, so the peer deadline
  // this helper's callers rely on is still exactly AFFA_PEER_TIMEOUT_MS from zero.
  affatest::carminatOpeningRequest(r.d, r.link);
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

  // THE 01 THAT ENDS THIS SESSION IS AN *ANSWERING* START NOW, NOT A DISCOVERY PROBE, and
  // that is a consequence of helloRequiresAnnounce rather than a weakening of the test. Our
  // BA went out during the opening `armed()` performed, so `_unauthControlIssued` is latched
  // for good; a `61 11 01` arriving after it is a full request, exactly as measured ([CAP]
  // "aknowledge offed display cONNECT OT POWER.csv" completes an entire session on 01 with no
  // `61 11 00` anywhere in it). It still carries the same meaning to us — "your registration
  // is void" — so FUNCSREG and usable authorization drop here just as they did before; the
  // difference is that this one frame ALSO schedules the replacement burst, so no second BA
  // and no further request is owed, and the recovery below is unchanged in every other way.
  r.link.inject(affatest::panelSyncStart());
  r.d.poll();
  expectNoFrame(r, "an answering 01 revokes and re-opens silently: no BA, no B9, no payload");
  TEST_ASSERT_FALSE(r.d.synced());
  TEST_ASSERT_FALSE(r.d.registered());

  TEST_ASSERT_EQUAL(Result::Ok, r.d.setTime("1000")); // held behind the new session
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

  // Peer loss clears the announce latch as well as the session, so recovery re-opens with
  // the SAME two-request exchange a cold bus does: our BA first, the burst on the panel's
  // next ask. A recovery that skipped the announce is exactly the bench failure this rule
  // was measured to fix.
  affatest::carminatOpeningRequest(r.d, r.link,
                                   "recovery announces before it answers with a burst");
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
  RUN_TEST(test_carminat_hello_is_a_ba_announce_then_three_paced_b0_frames);
  RUN_TEST(test_carminat_legacy_profile_is_immediate_70_b0_b0_but_still_requires_00);
  RUN_TEST(test_updatelist_hello_is_exactly_one_frame);
  RUN_TEST(test_carminat_announces_into_a_silent_bus_slowly_and_ba_only);
  RUN_TEST(test_carminat_ping_alone_never_starts_authentication);
  RUN_TEST(test_carminat_bootstrap_is_held_until_good_auth);
  RUN_TEST(test_carminat_acks_panel_registration_as_a_reflex_without_unlocking_output);
  RUN_TEST(test_any_complete_61_11_xx_is_the_same_request);
  RUN_TEST(test_phase_walks_the_measured_opening_in_order);
  RUN_TEST(test_phase_falls_back_when_the_panel_voids_the_session);
  RUN_TEST(test_carminat_does_not_cancel_the_start_announce_when_00_follows_immediately);
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
