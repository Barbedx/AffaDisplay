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
// The sync request
// ---------------------------------------------------------------------------

void test_request_is_emitted_only_while_failed_or_start(void) {
  CarRig r;
  r.d.begin();

  // FAILED -> alive + request.
  r.d.poll();
  SyncTally t = tally(r.link, 0x3AF, 0xB9, 0xBA);
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, t.alive, "one heartbeat");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, t.request, "the request goes out while FAILED");

  // Synced, START clear -> alive only, for as many seconds as you like.
  r.link.inject(affatest::panelSyncRequest());
  r.d.poll();
  drain(r.link);
  for (int s = 0; s < 3; ++s) {
    r.link.inject(affatest::panelPeerAlive());
    r.clk.advance(1000);
    r.d.poll();
  }
  t = tally(r.link, 0x3AF, 0xB9, 0xBA);
  TEST_ASSERT_EQUAL_INT_MESSAGE(3, t.alive, "one heartbeat per second");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, t.request, "no request once the link is up");
}

void test_start_arms_exactly_one_request_and_then_clears(void) {
  // data[2] == 0x01 sets START. It has NEVER been observed in any capture — data[2] is the
  // panel's filler on all 791 instances — but [REF] tests for it, so the path exists and
  // is pinned here with a FULL-LENGTH frame.
  CarRig r;
  r.d.begin();
  r.d.poll();
  drain(r.link);

  r.link.inject(mk(0x3CF, {0x61, 0x11, 0x01, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3}));
  r.clk.advance(1000);
  r.d.poll();
  SyncTally t = tally(r.link, 0x3AF, 0xB9, 0xBA);
  TEST_ASSERT_EQUAL_INT_MESSAGE(3, t.hello, "the 61 11 still gets its hello reply");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, t.alive, "heartbeat");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, t.request, "START arms the request even though synced");

  r.link.inject(affatest::panelPeerAlive());
  r.clk.advance(1000);
  r.d.poll();
  t = tally(r.link, 0x3AF, 0xB9, 0xBA);
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, t.request, "START is consumed, not sticky");
}

// ---------------------------------------------------------------------------
// THE SHORT-DLC TRAP
// ---------------------------------------------------------------------------

void test_short_dlc_sync_request_must_not_latch_start(void) {
  // The OEM corpus holds 0x3CF with DLC 1 and DLC 2. The legacy shim read data[2] without
  // checking len, so START latched off whatever the stack happened to hold. Here data[2]
  // is deliberately set to 0x01 — the value that WOULD set START — while len says the
  // panel only sent two bytes. Reading it is the bug; the test is that we do not.
  CarRig r;
  r.d.begin();
  r.d.poll();
  drain(r.link);

  Frame f = mk(0x3CF, {0x61, 0x11, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01});
  f.len = 2;
  r.link.inject(f);
  r.clk.advance(1000);
  r.d.poll();

  SyncTally t = tally(r.link, 0x3AF, 0xB9, 0xBA);
  TEST_ASSERT_TRUE_MESSAGE(r.d.synced(), "a DLC-2 `61 11` still clears FAILED");
  TEST_ASSERT_EQUAL_INT_MESSAGE(3, t.hello, "and still gets the hello reply");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, t.request,
                                "START must NOT latch from a byte the panel never sent");
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
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, t.alive, "one heartbeat, not ten");
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
  r.d.setPower(true);
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
  // And the machine asks to resynchronise, because it is FAILED again.
  drain(r.link);
  r.clk.advance(1000);
  r.d.poll();
  SyncTally t = tally(r.link, 0x3AF, 0xB9, 0xBA);
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, t.request, "FAILED re-arms the sync request");
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
  RUN_TEST(test_request_is_emitted_only_while_failed_or_start);
  RUN_TEST(test_start_arms_exactly_one_request_and_then_clears);
  RUN_TEST(test_short_dlc_sync_request_must_not_latch_start);
  RUN_TEST(test_short_dlc_peer_alive_is_honoured);
  RUN_TEST(test_a_foreign_cluster_token_is_not_answered);
  RUN_TEST(test_updatelist_heartbeat_is_paced_by_the_clock);
  RUN_TEST(test_a_stalled_loop_does_not_produce_a_catch_up_burst);
  RUN_TEST(test_peer_deadline_holds_at_4999ms);
  RUN_TEST(test_peer_deadline_fires_at_5001ms_and_drops_funcsreg);
  RUN_TEST(test_peer_loss_is_not_a_poll_count);
  RUN_TEST(test_the_request_argument_asymmetry_is_preserved);
  return UNITY_END();
}
