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


struct Rig {
  LoopbackLink<256> link;
  affatest::FakeClock clk;
  CarminatDisplay d;
  Rig() : d(link, clk) {}

  void up(bool echo, bool selfAck = false) {
    d.begin();
    // The 0x70 registrations leave with the final hello frame on this family, so a rig that
    // wants them acknowledged must arm the emulator before the opening, not after.
    d.setSelfAck(selfAck);
    affatest::completeCarminatAuth(d, link, clk);
    link.setEcho(echo);            // AFTER the handshake, so the hello frames do not echo
    d.onKey(&countKey, nullptr);
    drain(link);
    g_keys = 0;
  }

  void registerFuncs() {
    (void)d.setPower(true);
    affatest::settleCarminatRegistration(d, clk);
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
  r.up(false, /*selfAck=*/true);
  r.registerFuncs();

  uint8_t payload[22];
  for (uint8_t i = 0; i < sizeof(payload); ++i) payload[i] = static_cast<uint8_t>(i + 1);
  const TxTicket t = r.d.enqueue(0x151, payload, sizeof(payload)).ticket;
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
  // Direction::Rx must mean "what the other node actually sent" on EVERY link, echoing or
  // not. If an echo were presented as Rx, a sniffer would double-count the bus and a tap
  // that filters on Rx would fire on our own traffic.
  Rig r;
  r.up(true);                       // echoing
  r.d.onFrame(&tapDir, nullptr);

  g_rx = g_tx = 0;
  ASSERT_RESULT(Ok, r.d.pressKey(Key::Pause, KeyEdge::Click, KeySource::Wire));
  pump(r.d, 4);

  // One transmit, then the same frame back off the echoing link — and BOTH are Tx.
  TEST_ASSERT_EQUAL_INT_MESSAGE(2, g_tx, "the transmit and its echo are both Direction::Tx");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_rx, "nothing inbound happened on this bus");

  // And a genuine inbound frame on the same id IS Rx. This half used to be asserted through
  // a `dir = Rx` Layer 1 subscription; the tap already carries the direction, which is why
  // the subscription table it needed was never more than a filter somebody else could write.
  r.link.inject(mk(0x1C1, {0x03, 0x89, 0x00, 0x05, 0xA3, 0xA3, 0xA3, 0xA3}));
  r.d.poll();
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_rx, "a frame the panel sent is Direction::Rx");
}

// ---------------------------------------------------------------------------
// A held detent has no wire representation
// ---------------------------------------------------------------------------

// This was test_nav_refuses_a_coarse_step_on_the_wire, asserted through
// AffaDisplayBase::nav(NavCommand, KeySource). That call is gone — driving a menu and
// impersonating the panel at another radio were two jobs in one function, and the coarse
// step is the seam where they contradicted each other. The property it was really testing
// belongs to transmitKey() and is asserted here directly.
void test_a_held_wheel_detent_cannot_be_put_on_the_wire(void) {
  // 0x0101|0xC0 and 0x0141|0xC0 are BOTH 0x01C1, so a hold on a wheel code cannot be
  // encoded at all: transmitting it would send the CLICK form and step fine where the
  // caller asked coarse. The coarse step exists in a menu, which is software; the panel
  // physically cannot produce it.
  Rig r;
  r.up(false);
  ASSERT_RESULT(NotSupported, r.d.pressKey(Key::RollDown, KeyEdge::Hold, KeySource::Wire));
  ASSERT_RESULT(NotSupported, r.d.pressKey(Key::RollUp,   KeyEdge::Hold, KeySource::Both));
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, r.link.sentCount(), "and nothing reaches the bus");

  // The click edge does map to a real frame.
  ASSERT_RESULT(Ok, r.d.pressKey(Key::RollDown, KeyEdge::Click, KeySource::Wire));
  Frame f;
  TEST_ASSERT_TRUE(r.link.takeSent(f));
  static const uint8_t kWant[8] = {0x03, 0x89, 0x01, 0x41, 0x00, 0x00, 0x00, 0x00};
  TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(kWant, f.data, 8, "a RollDown click");
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
  RUN_TEST(test_a_held_wheel_detent_cannot_be_put_on_the_wire);
  return UNITY_END();
}
