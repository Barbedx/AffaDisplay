// The ONE sync FSM, driven against BOTH profiles through the real panel classes — so the
// constants in carminat/ and updatelist/ are under test as well as the machine in core/.
//
// The four properties this suite exists to keep:
//   * the hello reply is byte-exact and in order, and Carminat's second and third frames
//     are IDENTICAL (two sendCan calls in the legacy source, and present in the capture);
//   * the sync request leaves only while FAILED or START is set;
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

void test_carminat_hello_is_three_frames_the_last_two_identical(void) {
  CarRig r;
  r.d.begin();
  r.d.poll();                       // FAILED: alive + request
  drain(r.link);

  r.link.inject(affatest::panelSyncRequest());
  r.d.poll();

  static const Frame kH0 = {0x3AF, 8, {0x70, 0x1A, 0x11, 0x00, 0x00, 0x00, 0x00, 0x01}, false};
  static const Frame kH1 = {0x3AF, 8, {0xB0, 0x14, 0x11, 0x00, 0x1F, 0x00, 0x00, 0x00}, false};
  expectFrame(r.link, kH0, "Carminat hello 0/3");
  expectFrame(r.link, kH1, "Carminat hello 1/3");
  expectFrame(r.link, kH1, "Carminat hello 2/3 — IDENTICAL to 1/3, and that is observed");
  Frame f;
  TEST_ASSERT_FALSE_MESSAGE(r.link.takeSent(f), "the hello reply is exactly three frames");
  TEST_ASSERT_TRUE(r.d.synced());

  // And the table in the panel header says the same thing, so a future edit that
  // deduplicated the two frames would fail here as well as on the wire.
  TEST_ASSERT_EQUAL_UINT8(3, carminat::kSync.helloCount);
  TEST_ASSERT_EQUAL_UINT32(carminat::kHelloMinMs, carminat::kSync.helloMinMs);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(carminat::kHello[1], carminat::kHello[2], 8);
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

  // Once the panel has spoken, normal B9/pong liveness proceeds.  TWO per
  // ping-second since 0.4.1: the paced heartbeat plus the pong the ping provokes
  // (SyncProfile::replyToPing — MeganeCAN's two-heartbeats-a-second wire).  The request
  // must remain absent; the alive count is pinned so a pacing
  // regression in either half cannot hide.
  r.link.inject(affatest::panelSyncRequest());
  r.d.poll();
  drain(r.link);
  for (int s = 0; s < 3; ++s) {
    r.link.inject(affatest::panelPeerAlive());
    r.clk.advance(1000);
    r.d.poll();
  }
  t = tally(r.link, 0x3AF, 0xB9, 0xBA);
  TEST_ASSERT_EQUAL_INT_MESSAGE(6, t.alive, "one paced heartbeat + one pong, per second");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, t.request, "no request once the link is up");
}

void test_carminat_ping_alone_never_starts_authentication(void) {
  // `69` says only that a panel is alive. It may not unlock B9, the three auth frames, a
  // generic ACK, function registration, or a render queued by application code before the
  // panel sends its separate GOOD `61 11 00` authorization request.
  CarRig r;
  r.d.begin();
  TEST_ASSERT_EQUAL(Result::Ok, r.d.setPower(true)); // held until real 61 11 00

  r.link.inject(affatest::panelPeerAlive());
  r.d.poll();
  r.clk.advance(3000);
  r.d.poll();

  SyncTally t = tally(r.link, 0x3AF, 0xB9, 0xBA);
  TEST_ASSERT_FALSE_MESSAGE(r.d.synced(), "a bare 69 must not clear FAILED");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, t.hello, "69 must not start Carminat auth");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, t.alive, "69 before auth receives no B9");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, t.request, "and never creates BA");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, t.other, "queued output stays held before auth");

  // Now the real authorization request arrives.  The exact profile-specific hello is
  // allowed immediately and the held power command can proceed through registration.
  r.link.inject(mk(0x3CF, {0x61, 0x11, 0x00, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3}));
  r.d.poll();
  t = tally(r.link, 0x3AF, 0xB9, 0xBA);
  TEST_ASSERT_TRUE_MESSAGE(r.d.synced(), "61 11 00 alone opens the session");
  TEST_ASSERT_EQUAL_INT_MESSAGE(3, t.hello, "61 11 00 starts three-frame Carminat auth");
}

void test_carminat_bootstrap_is_held_until_good_auth(void) {
  // The proven Carminat driver answers EVERY complete 61 11 with the hello trio. `01` is
  // the bootstrap / START phase, so it also gets one B9+BA pair, but it remains locked:
  // no registration and no screen traffic are allowed before the later good `00`.
  TEST_ASSERT_TRUE_MESSAGE(carminat::kSync.requireAuthRequest,
                           "Carminat requires a display-originated auth request");
  TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, carminat::kSync.authRequestByte2,
                                 "only 61 11 00 is good auth");
  TEST_ASSERT_TRUE_MESSAGE(carminat::kSync.oneShotResyncOnStart,
                           "Carminat preserves the one-shot legacy START bootstrap");
  CarRig r;
  r.d.begin();
  r.d.setSelfAck(true);
  TEST_ASSERT_EQUAL(Result::Ok, r.d.setPower(true));
  TEST_ASSERT_EQUAL(Result::Ok, r.d.setTime("1000"));

  r.link.inject(affatest::panelSyncStart());
  r.d.poll();
  SyncTally t = tally(r.link, 0x3AF, 0xB9, 0xBA);
  TEST_ASSERT_FALSE_MESSAGE(r.d.synced(), "01 is bootstrap, not a usable session");
  TEST_ASSERT_FALSE_MESSAGE(r.d.registered(), "no function can register from 01");
  TEST_ASSERT_EQUAL_INT_MESSAGE(3, t.hello, "01 gets the exact Carminat hello trio");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, t.alive, "01 gets one B9 bootstrap");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, t.request, "01 gets one BA bootstrap");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, t.other, "held power/time cannot overtake authorization");

  // A no-ACK display retransmits 01 at line rate. The delayed hello may be coalesced, but
  // BA is exactly once for this phase: no per-second or per-request BA stream may return.
  for (int i = 0; i < 64; ++i) r.link.inject(affatest::panelSyncStart());
  r.d.poll();
  t = tally(r.link, 0x3AF, 0xB9, 0xBA);
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, t.request, "01 retransmissions do not repeat BA");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, t.alive, "01 retransmissions do not repeat bootstrap B9");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, t.other, "still no app traffic during a 01 storm");

  // Let the coalesced hello floor elapse, then provide the good authorization. Only now
  // may the held power/time jobs register functions and run.
  r.clk.advance(carminat::kHelloMinMs);
  r.link.inject(affatest::panelSyncRequest());
  r.d.poll();
  t = tally(r.link, 0x3AF, 0xB9, 0xBA);
  TEST_ASSERT_TRUE_MESSAGE(r.d.synced(), "good 61 11 00 opens the usable session");
  TEST_ASSERT_EQUAL_INT_MESSAGE(3, t.hello, "00 also gets exact Carminat hello");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, t.request, "00 needs no BA bootstrap");
  affatest::pumpUntilIdle(r.d);
  TEST_ASSERT_TRUE_MESSAGE(r.d.registered(), "functions register only after good 00");
}

void test_carminat_answers_unknown_full_auth_with_hello_only(void) {
  // The old handler tests only the 61 11 prefix before sending its three hello frames. An
  // unknown third byte must preserve that response, but it cannot become either a BA
  // bootstrap (reserved for 01) or authorization (reserved for 00).
  CarRig r;
  r.d.begin();
  r.d.poll();
  drain(r.link);

  r.link.inject(mk(0x3CF, {0x61, 0x11, 0x5A, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3}));
  r.d.poll();
  SyncTally t = tally(r.link, 0x3AF, 0xB9, 0xBA);
  TEST_ASSERT_FALSE_MESSAGE(r.d.synced(), "unknown auth never unlocks the session");
  TEST_ASSERT_EQUAL_INT_MESSAGE(3, t.hello, "every full 61 11 gets legacy hello x3");
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

  SyncTally t = tally(r.link, 0x3AF, 0xB9, 0xBA);
  TEST_ASSERT_FALSE_MESSAGE(r.d.synced(), "00's deferred hello still gates output");
  TEST_ASSERT_EQUAL_INT_MESSAGE(3, t.hello, "01 hello was sent immediately");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, t.alive, "START's B9 remains present");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, t.request, "START's BA remains present");

  r.clk.advance(carminat::kHelloMinMs);
  r.d.poll();
  t = tally(r.link, 0x3AF, 0xB9, 0xBA);
  TEST_ASSERT_TRUE_MESSAGE(r.d.synced(), "00 becomes usable only after its hello");
  TEST_ASSERT_EQUAL_INT_MESSAGE(3, t.hello, "00 receives its own deferred hello trio");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, t.request, "the one START BA is not retried");
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
  drain(r.link);

  r.clk.advance(10000);
  r.link.inject(affatest::panelPeerAlive());
  r.d.poll();
  SyncTally t = tally(r.link, 0x3AF, 0xB9, 0xBA);
  // Two frames, not eleven: the single paced heartbeat the stall owes, plus the pong the
  // injected ping provokes (0.4.1). The catch-up burst this test guards against would
  // show as ten paced frames; the pong is a reply, not a catch-up.
  TEST_ASSERT_EQUAL_INT_MESSAGE(2, t.alive, "one paced heartbeat + the ping's pong, not ten");
}

// ---------------------------------------------------------------------------
// The pong — SyncProfile::replyToPing, Carminat only (0.4.1)
// ---------------------------------------------------------------------------

void test_carminat_answers_a_ping_between_heartbeats(void) {
  // The proven driver's B9 follows the panel's 69 within milliseconds. Mid-interval —
  // paced heartbeat not due for another 600 ms — the ping must still be answered NOW,
  // with a frame byte-identical to the heartbeat, and nothing else.
  CarRig r;
  r.d.begin();
  r.link.inject(affatest::panelSyncRequest());
  r.d.poll();
  drain(r.link);

  r.clk.advance(400);
  r.link.inject(affatest::panelPeerAlive());
  r.d.poll();
  SyncTally t = tally(r.link, 0x3AF, 0xB9, 0xBA);
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, t.alive, "the pong leaves NOW, not on the next tick");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, t.request, "a pong is an alive frame, nothing rides along");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, t.other, "and it is byte-identical to the heartbeat");
}

void test_the_pong_is_paced_against_a_ping_storm(void) {
  // An unacknowledged panel repeats `69` at line rate — 126 copies in 32 ms measured on
  // the bench. One pong per copy would be the hello storm's twin. The state is credited
  // per ping; the FRAME is floored at AFFA_PING_REPLY_MIN_MS.
  CarRig r;
  r.d.begin();
  r.link.inject(affatest::panelSyncRequest());
  r.d.poll();
  drain(r.link);

  r.clk.advance(1);                              // clear of the paced tick
  for (int i = 0; i < 126; ++i) r.link.inject(affatest::panelPeerAlive());
  r.d.poll();                                    // pumpRx drains the whole burst
  SyncTally t = tally(r.link, 0x3AF, 0xB9, 0xBA);
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, t.alive, "126 pings in one burst: ONE pong");
  TEST_ASSERT_TRUE(r.d.synced());
}

void test_updatelist_does_not_pong(void) {
  // The pong is a Carminat reproduction of MeganeCAN's wire; the AFFA2 family keeps the
  // paced-only heartbeat. Pinned in the profiles AND on the wire, so flipping one flag
  // cannot pass unnoticed.
  TEST_ASSERT_TRUE_MESSAGE(carminat::kSync.replyToPing, "Carminat pongs");
  TEST_ASSERT_FALSE_MESSAGE(updatelist::kSync.replyToPing, "UpdateList does not");

  UlRig r;
  r.d.begin();
  r.link.inject(affatest::panelSyncRequest());
  r.d.poll();
  drain(r.link);

  r.clk.advance(400);
  r.link.inject(affatest::panelPeerAlive());
  r.d.poll();
  SyncTally t = tally(r.link, 0x3DF, 0x79, 0x7A);
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, t.alive, "a ping between ticks leaves the wire silent");
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

// Brings a Carminat rig to synced + registered with the clock still at zero, so the peer
// deadline armed by begin() is exactly AFFA_PEER_TIMEOUT_MS.
void armed(CarRig& r) {
  r.clk.t = 0;
  r.d.begin();
  r.link.inject(affatest::panelSyncRequest());
  r.d.poll();
  r.d.setSelfAck(true);
  (void)r.d.setPower(true);
  affatest::pumpUntilIdle(r.d);
  TEST_ASSERT_TRUE(r.d.registered());
  drain(r.link);
}

}  // namespace

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
  t = tally(r.link, 0x3AF, 0xB9, 0xBA);
  TEST_ASSERT_EQUAL_INT_MESSAGE(3, t.hello, "the next panel request restarts the session");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, t.request, "and recovery remains panel-initiated");
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
  RUN_TEST(test_carminat_hello_is_three_frames_the_last_two_identical);
  RUN_TEST(test_updatelist_hello_is_exactly_one_frame);
  RUN_TEST(test_carminat_waits_for_a_panel_message_before_transmitting);
  RUN_TEST(test_carminat_ping_alone_never_starts_authentication);
  RUN_TEST(test_carminat_bootstrap_is_held_until_good_auth);
  RUN_TEST(test_carminat_answers_unknown_full_auth_with_hello_only);
  RUN_TEST(test_carminat_does_not_cancel_start_pair_when_00_follows_immediately);
  RUN_TEST(test_short_dlc_carminat_auth_request_stays_silent);
  RUN_TEST(test_short_dlc_peer_alive_is_honoured);
  RUN_TEST(test_a_foreign_cluster_token_is_not_answered);
  RUN_TEST(test_updatelist_heartbeat_is_paced_by_the_clock);
  RUN_TEST(test_a_stalled_loop_does_not_produce_a_catch_up_burst);
  RUN_TEST(test_carminat_answers_a_ping_between_heartbeats);
  RUN_TEST(test_the_pong_is_paced_against_a_ping_storm);
  RUN_TEST(test_updatelist_does_not_pong);
  RUN_TEST(test_passive_never_pongs);
  RUN_TEST(test_peer_deadline_holds_at_4999ms);
  RUN_TEST(test_peer_deadline_fires_at_5001ms_and_drops_funcsreg);
  RUN_TEST(test_peer_loss_is_not_a_poll_count);
  RUN_TEST(test_the_request_argument_asymmetry_is_preserved);
  return UNITY_END();
}
