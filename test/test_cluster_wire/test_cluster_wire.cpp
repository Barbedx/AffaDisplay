// The instrument cluster, pinned to the ONE capture there is.
//
// WHY THIS SUITE EXISTS AT ALL. The cluster is the only family with no hardware, so nothing
// exercised it — and it silently stopped compiling through the whole 2.0 refactor, because
// no build compiled the file. It looked alive for weeks. A panel nobody builds is a panel
// that rots, so it is in [env:native] now and these tests are what make that mean something.
//
// WHAT THESE ASSERTIONS ARE AND ARE NOT. Every expectation here is transcribed from
// docs/PROTOCOL-NOTES.md §9 — a single capture of an OEM radio talking to a cluster, supplied
// 2026-07-28, with the direction annotations belonging to whoever captured it. NOTHING here
// is bench-verified, and no byte of it has ever been on a bus from this library.
//
// So these tests do not prove the cluster works. They prove that what we SEND matches what
// the OEM radio was OBSERVED to send, which is a different and much weaker claim — and the
// only one the evidence supports. When a cluster reaches a bench, this is the file that says
// what we currently believe, so a disagreement has somewhere to be recorded.
#include "../affa_test_support.h"

#include "cluster/ClusterDisplay.h"

using namespace affa;
using affatest::mk;
using affatest::drain;

namespace {

struct Rig {
  LoopbackLink<256> link;
  affatest::FakeClock clk;
  ClusterDisplay d;
  Rig() : d(link, clk) {}

  // The opening as the capture shows it, which is the OTHER WAY ROUND from Carminat: the
  // RADIO asks. We send `3AF 5A 01`, the cluster answers on 0x3CF, and the hello follows.
  void up() {
    d.begin();
    d.setSelfAck(true);
    // `3CF 1 69` — the cluster's peer-alive, AND IT IS DLC 1. §9.1 flags it because it is
    // the short-DLC case handleSyncFrame() guards against, observed in the wild.
    Frame ping;
    ping.id = cluster::kIdSyncReply;
    ping.len = 1;
    ping.data[0] = 0x69;
    link.inject(ping);
    d.poll();
    clk.advance(AFFA_SYNC_INTERVAL_MS + 1);
    d.poll();
    drain(link);
  }

  // THE CLUSTER OPENS ITS OWN CHANNEL FIRST, and with registerAfterHello set we now genuinely
  // wait for it — so a rig that forgot this frame would hang in Registering for ever. That is
  // the gate working, and it is why this is a cluster-specific helper rather than the
  // Carminat one: the ids and the filler both differ.
  //
  // `1C1 70 84 84 84 84 84 84` — 0x84 is the CLUSTER's filler, which is how §9.2's third
  // "radio" registration is identified as the cluster's own.
  void openPeerChannel() {
    Frame own = mk(cluster::kIdKeyPressed,
                   {0x70, 0x84, 0x84, 0x84, 0x84, 0x84, 0x84, 0x84});
    link.inject(own);
    d.poll();
  }

};

}  // namespace

// ---------------------------------------------------------------------------
// §9.1 — the sync bytes are a THIRD pair on an id we already know
// ---------------------------------------------------------------------------

void test_the_sync_pair_is_59_5A_and_the_request_carries_01(void) {
  // Carminat is B9/BA with arg 0x00, UpdateList is 79/7A with arg 0x01, this is 59/5A with
  // arg 0x01. The X9-alive / XA-request pattern holding across three families is the most
  // useful thing this capture tells us, and it is why SyncProfile is data and not code.
  TEST_ASSERT_EQUAL_HEX8(0x59, cluster::kSync.aliveByte);
  TEST_ASSERT_EQUAL_HEX8(0x5A, cluster::kSync.requestByte);
  TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x01, cluster::kSync.requestArg,
                                 "the cluster's request arg is 01, NOT Carminat's 00");
  TEST_ASSERT_EQUAL_HEX16(0x3AF, cluster::kSync.syncId);
  TEST_ASSERT_EQUAL_HEX16(0x3CF, cluster::kSync.syncReplyId);
}

void test_the_radio_drives_this_family_rather_than_waiting(void) {
  // THE CAPTURE'S CLEAREST STRUCTURAL FACT, and it is inverted from Carminat. `3AF 5A 01` is
  // annotated "radio sync request" and the cluster never sends a `61 11` at all — so a
  // profile that waited for one would wait for ever. This was `waitForPanel` defaulting to
  // false by accident; it is false on purpose now.
  TEST_ASSERT_FALSE_MESSAGE(cluster::kSync.waitForPanel, "the radio opens this conversation");
  TEST_ASSERT_TRUE(cluster::kSync.sendSyncRequest);
  TEST_ASSERT_FALSE_MESSAGE(cluster::kSync.requireAuthRequest,
                            "there is no 61 11 on this bus to require");
  TEST_ASSERT_FALSE_MESSAGE(cluster::kSync.helloRequiresAnnounce,
                            "we are the one asking, so there is nothing to precede");
}

// ---------------------------------------------------------------------------
// §9.2 — registration, and what the FILLER proves about who spoke
// ---------------------------------------------------------------------------

void test_we_wait_for_the_clusters_own_channel_before_registering_ours(void) {
  // THE FILLER IS THE EVIDENCE. The capture reads:
  //
  //     1C1 8  70 84 84 84 84 84 84 84     <- 0x84 is the CLUSTER's filler
  //     121 8  70 FF FF FF FF FF FF FF     <- 0xFF is the RADIO's
  //     1B1 8  70 FF FF FF FF FF FF FF     <- ours
  //
  // Filler is a property of the SPEAKER, not of the bus, so 0x1C1 was sent BY THE CLUSTER:
  // it opens its own channel first and we register after. That is the same peer-channel gate
  // Carminat measures 4/4, and PROTOCOL-NOTES §9.2 reads it as "three functions" the radio
  // registered — the prose is wrong and the filler is right.
  TEST_ASSERT_TRUE_MESSAGE(cluster::kSync.registerAfterHello,
                           "the cluster's 1C1 comes first; ours follow it");
  TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xFF, cluster::kFiller, "we are the radio, so we pad FF");

  // Two functions, in the order the capture shows them. THE ORDER IS ON THE WIRE.
  TEST_ASSERT_EQUAL_UINT8(2, cluster::kFuncCount);
  TEST_ASSERT_EQUAL_HEX16(0x121, cluster::kFuncIds[0]);
  TEST_ASSERT_EQUAL_HEX16(0x1B1, cluster::kFuncIds[1]);
}

void test_the_three_hellos_are_paced_the_way_the_radio_paced_them(void) {
  // Seen three times ~30 ms apart. The gap was 0, which would put them back to back; whether
  // it MATTERS is unknown, and matching the OEM costs nothing while diverging costs an
  // unknown. Three rather than one is also the conservative reading — a hello the peer
  // ignores is harmless, a missing one is not.
  TEST_ASSERT_EQUAL_UINT8(3, cluster::kSync.helloCount);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(30, cluster::kSync.helloFrameGapMs,
                                   "the capture spaces them ~30 ms apart");
  static const uint8_t kWant[8] = {0x50, 0x29, 0x00, 0x23, 0x00, 0x00, 0x00, 0x69};
  for (uint8_t i = 0; i < 3; ++i)
    TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(kWant, cluster::kSync.hello[i], 8,
                                         "hello frame [CAP-VERBATIM]");
}

// ---------------------------------------------------------------------------
// THE BLOCKER: this family cannot open a session at all
// ---------------------------------------------------------------------------

void test_the_opening_cannot_complete_because_nothing_triggers_the_hello(void) {
  // FOUND 2026-08-08, AND IT IS THE ANSWER TO "does the algorithm match the log": NO.
  //
  // Both queueHello() call sites live inside one branch of handleSyncFrame():
  //
  //     if (f.data[0] == 0x61 && f.data[1] == 0x11) { ... queueHello(now); ... }
  //
  // THE CLUSTER CAPTURE CONTAINS NO `61 11` ANYWHERE. §9.1 shows the cluster sending only
  // `3CF 1 69`, a peer-alive at DLC 1. So the burst is never queued, registration never
  // follows it, and no profile field can change that — the trigger is in the FSM, not in
  // the data.
  //
  // WHAT THE CAPTURE ACTUALLY SHOWS is the radio driving:
  //
  //     3AF 2  59 00                       radio alive
  //     3AF 2  5A 01                       radio sync request
  //     3CF 1  69                          cluster answers
  //     3AF 8  50 29 00 23 00 00 00 69     radio hello
  //
  // so the trigger here is either the `69` or nothing at all — our own request is enough.
  // WHICH OF THE TWO IS NOT DECIDABLE FROM ONE SAMPLE, and this project has paid for
  // guessing a trigger before: four protocol bugs, all the same shape, a special case
  // standing in for a general rule. So it is recorded rather than invented.
  //
  // This test PASSES on the broken behaviour on purpose. It is a marker, not an
  // aspiration: when the rule is settled it flips to asserting the opening completes, and
  // until then it stops anyone believing this family works.
  Rig r;
  r.up();
  r.openPeerChannel();
  for (uint8_t i = 0; i < 40; ++i) { r.clk.advance(31); r.d.poll(); }
  TEST_ASSERT_FALSE_MESSAGE(r.d.registered(),
      "if this now PASSES registration, the hello trigger was fixed -- update this test");
  TEST_ASSERT_TRUE_MESSAGE(r.d.phase() < Phase::Registering,
      "the opening stalls before registration, waiting for a 61 11 that never comes");
}

// ---------------------------------------------------------------------------
// What the capture does NOT contain
// ---------------------------------------------------------------------------

void test_text_is_refused_because_the_encoding_is_unknown(void) {
  // The radio never renders in the sample, so the cluster's setText encoding is genuinely
  // unknown. Saying true here and guessing would be a CAPABILITY LIE — the exact failure the
  // legacy IDisplay had, where a silent no-op returned success.
  Rig r;
  r.up();
  TEST_ASSERT_FALSE_MESSAGE(r.d.supports(Feature::Text), "no text frame exists in the capture");
  ASSERT_RESULT(NotSupported, r.d.setText("HELLO"));
  ASSERT_RESULT(NotSupported, r.d.setTime("1234"));
  TEST_ASSERT_TRUE(r.d.supports(Feature::Power));
  TEST_ASSERT_TRUE_MESSAGE(r.d.supports(Feature::KeyTx), "the cluster owns 0x1C1");
}

void test_the_geometry_reports_nothing_it_cannot_draw(void) {
  // A zero surface and an unsupported feature are the same answer, and they must agree — a
  // fitter that trusted a non-zero mainChars here would compose text for a panel whose text
  // encoding nobody knows.
  Rig r;
  const PanelGeometry g = r.d.panelGeometry();
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, g.mainChars, "no known text surface");
  TEST_ASSERT_EQUAL_UINT8(0, g.listMaxItems);
  TEST_ASSERT_FALSE(g.hasImage());
}

// ---------------------------------------------------------------------------

void setUp(void) {}
void tearDown(void) {}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_the_sync_pair_is_59_5A_and_the_request_carries_01);
  RUN_TEST(test_the_radio_drives_this_family_rather_than_waiting);
  RUN_TEST(test_we_wait_for_the_clusters_own_channel_before_registering_ours);
  RUN_TEST(test_the_three_hellos_are_paced_the_way_the_radio_paced_them);
  RUN_TEST(test_the_opening_cannot_complete_because_nothing_triggers_the_hello);
  RUN_TEST(test_text_is_refused_because_the_encoding_is_unknown);
  RUN_TEST(test_the_geometry_reports_nothing_it_cannot_draw);
  return UNITY_END();
}
