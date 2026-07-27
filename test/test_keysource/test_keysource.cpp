// KeySource, and the Frame::fromSelf rule underneath it.
//
// A real CAN controller does not receive its own transmissions. LoopbackLink can, and if
// the library did not drop self-sent frames, every host test would be lying about the
// target: pressKey(..., Both) would fire once on hardware and twice here.
//
// The rule is that a self-sent frame is dropped BEFORE the auto-ACK, BEFORE the ACK
// matcher AND before the key decoder — all three. Missing the ACK matcher makes a loopback
// transfer "succeed" after one frame; missing the auto-ACK makes the library acknowledge
// itself into a storm, which is the 0x7AF incident and it has already happened on real
// hardware. This suite asserts each of the three separately, because two of them are
// invisible to a test that only counts key callbacks.

#include "../affa_test_support.h"

#include "carminat/CarminatDisplay.h"

using namespace affa;
using affatest::mk;
using affatest::drain;
using affatest::pump;
using affatest::pumpUntilIdle;

namespace {

int g_keys = 0;
void countKey(Key, KeyEdge, void*) { ++g_keys; }

int g_rx = 0, g_tx = 0;
void tapDir(const Frame&, Direction d, void*) {
  if (d == Direction::Rx) ++g_rx; else ++g_tx;
}

int g_subRx = 0;
void countSubRx(const Frame&, void*) { ++g_subRx; }

struct Rig {
  LoopbackLink<256> link;
  affatest::FakeClock clk;
  CarminatDisplay d;
  Rig() : d(link, clk) {}

  void up(bool echo) {
    d.begin();
    link.inject(affatest::panelSyncRequest());
    d.poll();
    link.setEcho(echo);            // AFTER the handshake, so the hello frames do not echo
    d.onKey(&countKey, nullptr);
    drain(link);
    g_keys = 0;
  }

  void registerFuncs() {
    d.setSelfAck(true);
    (void)d.setPower(true);
    pumpUntilIdle(d);
    TEST_ASSERT_TRUE(d.registered());
    d.setSelfAck(false);
    drain(link);
  }
};

// One (source, link) cell of the table. `wantKeys` is how many times KeyCb must fire and
// `wantFrames` how many frames must reach the bus — and BOTH must be identical for the
// echoing and the plain link, which is the whole property being asserted.
void check(bool echo, KeySource src, int wantKeys, int wantFrames, const char* what) {
  Rig r;
  r.up(echo);

  ASSERT_RESULT(Ok, r.d.pressKey(Key::Pause, KeyEdge::Click, src));
  pump(r.d, 6);                    // plenty of passes for an echo to come back

  TEST_ASSERT_EQUAL_INT_MESSAGE(wantKeys, g_keys, what);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(static_cast<uint32_t>(wantFrames), r.link.sentCount(),
                                   what);
}

}  // namespace

// ---------------------------------------------------------------------------
// The table: every KeySource on both links, and the two columns must agree
// ---------------------------------------------------------------------------

void test_local_fires_once_on_both_links(void) {
  // Local is the DEFAULT because in the radio role a key press IS a local event: nothing
  // goes on the bus.
  check(false, KeySource::Local, 1, 0, "Local, plain link");
  check(true,  KeySource::Local, 1, 0, "Local, ECHOING link — identical");
}

void test_wire_never_fires_locally_on_either_link(void) {
  // Wire impersonates the PANEL at a real radio. It puts one frame on the bus and has no
  // local effect — and on an echoing link the frame that comes straight back is ours, so
  // it must not manufacture a local key either.
  check(false, KeySource::Wire, 0, 1, "Wire, plain link");
  check(true,  KeySource::Wire, 0, 1, "Wire, ECHOING link — still no local key");
}

void test_both_fires_exactly_once_on_both_links(void) {
  // The case the rule exists for. Without fromSelf this is 1 on hardware and 2 here.
  check(false, KeySource::Both, 1, 1, "Both, plain link");
  check(true,  KeySource::Both, 1, 1, "Both, ECHOING link — the echo delivers nothing");
}

// ---------------------------------------------------------------------------
// The three drop points, asserted one at a time
// ---------------------------------------------------------------------------

void test_fromSelf_is_dropped_before_the_auto_ack(void) {
  // 0x1C1 is not a sync id, does not carry the reply flag and is not in the function
  // table, so an INBOUND frame there is answered with `74` on 0x5C1. Our own echo of that
  // same id must not be: acknowledging our own transmissions is the 0x7AF incident.
  Rig r;
  r.up(false);

  // Baseline: a genuine inbound frame IS acknowledged.
  r.link.inject(mk(0x1C1, {0x70, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3}));
  r.d.poll();
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, r.link.sentCount(), "an inbound 0x1C1 is acknowledged");
  drain(r.link);

  // The same bytes, stamped as ours.
  Frame self = mk(0x1C1, {0x70, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3});
  self.fromSelf = true;
  r.link.inject(self);
  r.d.poll();
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, r.link.sentCount(),
                                   "we must never acknowledge our own transmission");
}

void test_fromSelf_is_dropped_before_the_ack_matcher(void) {
  // The failure this prevents is silent and it looks like success: a loopback transfer
  // that "completes" after one frame because our own echo was credited as the panel's ACK.
  Rig r;
  r.up(false);
  r.registerFuncs();

  uint8_t payload[22];
  for (uint8_t i = 0; i < sizeof(payload); ++i) payload[i] = static_cast<uint8_t>(i + 1);
  const TxTicket t = r.d.enqueue(0x151, payload, sizeof(payload));
  r.d.poll();                                     // frame 0 out, WaitAck
  TEST_ASSERT_TRUE(r.d.busy());
  TEST_ASSERT_EQUAL_UINT32(1, r.link.sentCount());

  // A perfectly formed DONE — but ours.
  Frame selfAck = mk(0x551, {0x74, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
  selfAck.fromSelf = true;
  r.link.inject(selfAck);
  r.d.poll();
  TEST_ASSERT_TRUE_MESSAGE(r.d.busy(), "our own ACK must not complete our own transfer");
  TEST_ASSERT_NOT_EQUAL(t, r.d.lastTicket());

  // The panel's identical frame does complete it, which is what makes the test about
  // fromSelf and not about the ACK bytes.
  r.link.inject(mk(0x551, {0x74, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3}));
  r.d.poll();
  TEST_ASSERT_FALSE(r.d.busy());
  TEST_ASSERT_EQUAL_UINT16(t, r.d.lastTicket());
  ASSERT_RESULT(Ok, r.d.lastResult());
}

void test_fromSelf_is_dropped_before_the_key_decoder(void) {
  Rig r;
  r.up(false);

  Frame self = mk(0x1C1, {0x03, 0x89, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00});
  self.fromSelf = true;
  r.link.inject(self);
  r.d.poll();
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_keys, "our own key frame is not a key press");

  r.link.inject(mk(0x1C1, {0x03, 0x89, 0x00, 0x05, 0xA3, 0xA3, 0xA3, 0xA3}));
  r.d.poll();
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_keys, "the panel's identical frame is");
}

// ---------------------------------------------------------------------------
// What the observation seam sees
// ---------------------------------------------------------------------------

void test_a_self_frame_arriving_inbound_is_presented_as_Tx(void) {
  // A `dir = Rx` subscription must mean "what the other node actually sent" on EVERY link,
  // echoing or not. If an echo were presented as Rx, a sniffer would double-count the bus
  // and a payload-matched subscription would fire on our own traffic.
  Rig r;
  r.up(true);                       // echoing
  r.d.onFrame(&tapDir, nullptr);

  FrameMatch m{};
  m.id     = 0x1C1;
  m.idMask = 0x7FF;
  m.dir    = Direction::Rx;
  const SubHandle h = r.d.subscribe(m, &countSubRx, nullptr);
  TEST_ASSERT_TRUE(h.valid());

  g_rx = g_tx = 0;
  g_subRx = 0;
  ASSERT_RESULT(Ok, r.d.pressKey(Key::Pause, KeyEdge::Click, KeySource::Wire));
  pump(r.d, 4);

  // One transmit, then the same frame back off the echoing link — and BOTH are Tx.
  TEST_ASSERT_EQUAL_INT_MESSAGE(2, g_tx, "the transmit and its echo are both Direction::Tx");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_rx, "nothing inbound happened on this bus");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_subRx, "a dir=Rx subscription must not see our echo");

  // And a genuine inbound frame on the same id does fire it.
  r.link.inject(mk(0x1C1, {0x03, 0x89, 0x00, 0x05, 0xA3, 0xA3, 0xA3, 0xA3}));
  r.d.poll();
  TEST_ASSERT_EQUAL_INT(1, g_subRx);
}

// ---------------------------------------------------------------------------
// nav() carries the same source semantics
// ---------------------------------------------------------------------------

void test_nav_refuses_a_coarse_step_on_the_wire(void) {
  // Increase/Decrease are a held detent, which has no wire representation at all. That is
  // also the reason input has to be a SEAM rather than a source: the coarse-step feature
  // exists in the menu and the panel physically cannot reach it.
  Rig r;
  r.up(false);
  ASSERT_RESULT(NotSupported, r.d.nav(NavCommand::Increase, KeySource::Wire));
  ASSERT_RESULT(NotSupported, r.d.nav(NavCommand::Decrease, KeySource::Both));
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, r.link.sentCount(), "and nothing reaches the bus");

  // The click-edge commands do map to real frames.
  ASSERT_RESULT(Ok, r.d.nav(NavCommand::Next, KeySource::Wire));
  Frame f;
  TEST_ASSERT_TRUE(r.link.takeSent(f));
  static const uint8_t kWant[8] = {0x03, 0x89, 0x01, 0x41, 0x00, 0x00, 0x00, 0x00};
  TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(kWant, f.data, 8, "Next is a RollDown click");
}

// ---------------------------------------------------------------------------

void setUp(void) {}
void tearDown(void) {}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_local_fires_once_on_both_links);
  RUN_TEST(test_wire_never_fires_locally_on_either_link);
  RUN_TEST(test_both_fires_exactly_once_on_both_links);
  RUN_TEST(test_fromSelf_is_dropped_before_the_auto_ack);
  RUN_TEST(test_fromSelf_is_dropped_before_the_ack_matcher);
  RUN_TEST(test_fromSelf_is_dropped_before_the_key_decoder);
  RUN_TEST(test_a_self_frame_arriving_inbound_is_presented_as_Tx);
  RUN_TEST(test_nav_refuses_a_coarse_step_on_the_wire);
  return UNITY_END();
}
