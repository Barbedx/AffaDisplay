// The library's headline guarantee, stated as the sentence a test asserts:
//
//   the number of poll() calls between a key frame entering the RX ring and the key
//   callback firing is EXACTLY ONE, regardless of transmit queue depth, of any message in
//   flight, or of a WaitAck with 1900 ms left on its deadline.
//
// That is what pumpRx() strictly preceding pumpTx() buys, and it is why the poll() order is
// a contract rather than an implementation detail. Everything else here is the preemption
// machinery that keeps it true under load: coalescing, abortPending() and abortAll().

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

struct Aborts {
  int      count = 0;
  TxTicket seen[AFFA_TX_QUEUE_DEPTH] = {0};
  int      nestedResult = -1;
};
Aborts g_ab;
CarminatDisplay* g_d = nullptr;

void recordAbort(TxTicket t, Result r, void*) {
  if (r != Result::Aborted) return;
  if (g_ab.count < AFFA_TX_QUEUE_DEPTH) g_ab.seen[g_ab.count] = t;
  ++g_ab.count;
  // The queue is mutated BEFORE any callback fires, so a nested abortPending() from inside
  // one of them finds nothing. Recorded on the first callback only.
  if (g_ab.nestedResult < 0) g_ab.nestedResult = static_cast<int>(g_d->abortPending());
}

struct Rig {
  LoopbackLink<256> link;
  affatest::FakeClock clk;
  CarminatDisplay d;
  Rig() : d(link, clk) {}

  void sync() {
    d.begin();
    link.inject(affatest::panelSyncRequest());
    d.poll();
    TEST_ASSERT_TRUE(d.synced());
    drain(link);
  }
  void up() {
    sync();
    d.setSelfAck(true);
    d.setPower(true);
    pumpUntilIdle(d);
    TEST_ASSERT_TRUE(d.registered());
    drain(link);
  }
};

// Injects a Pause key frame and returns how many poll() calls it took to reach KeyCb.
int pollsUntilKey(Rig& r) {
  g_keys = 0;
  r.link.inject(mk(0x1C1, {0x03, 0x89, 0x00, 0x05, 0xA3, 0xA3, 0xA3, 0xA3}));
  for (int n = 1; n <= 16; ++n) {
    r.d.poll();
    if (g_keys) return n;
  }
  return -1;
}

uint8_t g_payload[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};

}  // namespace

// ---------------------------------------------------------------------------
// The guarantee
// ---------------------------------------------------------------------------

void test_a_key_reaches_the_callback_in_one_poll_with_an_empty_queue(void) {
  Rig r;
  r.up();
  r.d.onKey(&countKey, nullptr);
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, pollsUntilKey(r), "empty queue: one poll");
}

void test_a_key_reaches_the_callback_in_one_poll_with_a_full_queue(void) {
  Rig r;
  r.up();
  r.d.onKey(&countKey, nullptr);

  // A 96-byte showMenu ON THE WIRE, in WaitAck, with nothing about to acknowledge it —
  // the worst case the transmit FSM can be in.
  r.d.setSelfAck(false);
  ASSERT_RESULT(Ok, r.d.showMenu("Main Menu", "Voltage:0V", "Boost:0mbar", 0x0B));
  r.d.poll();
  TEST_ASSERT_TRUE(r.d.busy());
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, r.link.sentCount(), "frame 0 is out and waiting");

  // Fill every remaining slot. RenderSlot::None never coalesces, so these really do stack.
  for (int i = 0; i < AFFA_TX_QUEUE_DEPTH - 1; ++i) {
    g_payload[0] = static_cast<uint8_t>(i);
    TEST_ASSERT_NOT_EQUAL(kNoTicket, r.d.enqueue(0x151, g_payload, 4));
  }
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(AFFA_TX_QUEUE_DEPTH - 1, r.d.queued(), "the queue is full");
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(kNoTicket, r.d.enqueue(0x151, g_payload, 4),
                                   "and the next one is refused");
  ASSERT_RESULT(QueueFull, r.d.lastResult());
  drain(r.link);

  TEST_ASSERT_EQUAL_INT_MESSAGE(1, pollsUntilKey(r),
                                "IDENTICAL to the empty-queue case, which is the guarantee");
  TEST_ASSERT_TRUE_MESSAGE(r.d.busy(), "and the in-flight job is untouched");
  TEST_ASSERT_EQUAL_UINT8(AFFA_TX_QUEUE_DEPTH - 1, r.d.queued());
}

void test_a_burst_that_arrived_between_polls_is_delivered_in_full(void) {
  // pumpRx drains to EMPTY, not one frame per call, so a key that arrived behind an ACK is
  // still delivered in the same poll.
  Rig r;
  r.up();
  r.d.onKey(&countKey, nullptr);
  g_keys = 0;

  r.link.inject(mk(0x551, {0x74, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3}));
  for (int i = 0; i < 5; ++i)
    r.link.inject(mk(0x1C1, {0x03, 0x89, 0x00, 0x05, 0xA3, 0xA3, 0xA3, 0xA3}));
  r.d.poll();
  TEST_ASSERT_EQUAL_INT_MESSAGE(5, g_keys, "the whole burst, in one poll, in arrival order");
}

// ---------------------------------------------------------------------------
// Coalescing
// ---------------------------------------------------------------------------

void test_coalescing_keeps_one_entry_carrying_the_last_value(void) {
  // A repeated render of one slot occupies exactly ONE queue slot regardless of render
  // rate, and it always holds the NEWEST value. Without this the panel keeps displaying
  // superseded values for a second after the key, which reads as a key-handling defect and
  // is really a queueing one.
  Rig r;
  r.up();
  g_ab = Aborts{};
  g_d  = &r.d;
  r.d.onComplete(&recordAbort, nullptr);

  ASSERT_RESULT(Ok, r.d.showMenu("Main Menu", "Voltage:0V", "Boost:0mbar", 0x0B));
  r.d.poll();                       // the menu is started and no longer preemptable

  ASSERT_RESULT(Ok, r.d.setText("AAA"));
  ASSERT_RESULT(Ok, r.d.setText("BBB"));
  ASSERT_RESULT(Ok, r.d.setText("CCC"));
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, r.d.queued(), "three renders, ONE queue slot");
  TEST_ASSERT_TRUE(r.d.pending(RenderSlot::Text));
  TEST_ASSERT_EQUAL_INT_MESSAGE(2, g_ab.count, "the two superseded renders report Aborted");

  pumpUntilIdle(r.d);

  // Walk to the setText message and check which string survived.
  Frame f;
  bool found = false;
  while (r.link.takeSent(f)) {
    if (f.data[0] == 0x10 && f.data[2] == 0x77) {
      TEST_ASSERT_TRUE(r.link.takeSent(f));
      static const uint8_t kWant[8] = {0x21, 'C', 'C', 'C', 0x00, 0x00, 0x00, 0x00};
      TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(kWant, f.data, 8, "the NEWEST value survives");
      found = true;
      break;
    }
  }
  TEST_ASSERT_TRUE_MESSAGE(found, "the coalesced render must still be transmitted");
  drain(r.link);
}

void test_different_slots_never_coalesce_against_each_other(void) {
  // A highlight must not replace a pending full redraw, and a full redraw must not replace
  // a pending highlight — they are ~14x apart in bus time and mean different things.
  Rig r;
  r.up();
  r.d.showMenu("Main Menu", "a", "b", 0x0B);
  r.d.poll();                                   // in flight

  ASSERT_RESULT(Ok, r.d.showMenu("Main Menu", "c", "d", 0x0B));   // Menu
  ASSERT_RESULT(Ok, r.d.highlightItem(1));                        // Highlight
  ASSERT_RESULT(Ok, r.d.setText("X"));                            // Text
  ASSERT_RESULT(Ok, r.d.setTime("1234"));                         // Clock
  ASSERT_RESULT(Ok, r.d.setPower(true));                          // Control
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(5, r.d.queued(), "five distinct slots, five queue slots");
  TEST_ASSERT_TRUE(r.d.pending(RenderSlot::Menu));
  TEST_ASSERT_TRUE(r.d.pending(RenderSlot::Highlight));
  TEST_ASSERT_TRUE(r.d.pending(RenderSlot::Text));
  TEST_ASSERT_TRUE(r.d.pending(RenderSlot::Clock));
  TEST_ASSERT_TRUE(r.d.pending(RenderSlot::Control));
  pumpUntilIdle(r.d);
  drain(r.link);
}

// ---------------------------------------------------------------------------
// abortPending
// ---------------------------------------------------------------------------

void test_abortPending_reports_Aborted_once_per_dropped_ticket(void) {
  Rig r;
  r.up();
  g_ab = Aborts{};
  g_d  = &r.d;

  r.d.showMenu("Main Menu", "a", "b", 0x0B);
  r.d.poll();                                   // started: not preemptable

  r.d.onComplete(&recordAbort, nullptr);
  TxTicket t[3];
  for (int i = 0; i < 3; ++i) {
    g_payload[0] = static_cast<uint8_t>(0xA0 + i);
    t[i] = r.d.enqueue(0x151, g_payload, 4);
    TEST_ASSERT_NOT_EQUAL(kNoTicket, t[i]);
  }

  TEST_ASSERT_EQUAL_UINT8_MESSAGE(3, r.d.abortPending(), "three jobs dropped");
  TEST_ASSERT_EQUAL_INT_MESSAGE(3, g_ab.count, "and exactly three callbacks");
  for (int i = 0; i < 3; ++i)
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(t[i], g_ab.seen[i], "each ticket reported once, in order");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_ab.nestedResult,
                                "a nested abortPending() finds nothing: queue first, callbacks second");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, r.d.abortPending(), "a second call finds nothing either");

  // The message on the wire is untouched and finishes normally.
  pumpUntilIdle(r.d);
  ASSERT_RESULT(Ok, r.d.lastResult());
}

void test_abortPending_never_touches_a_registration_job(void) {
  // A payload that reached the panel before its function was registered is REJECTED, and
  // the resulting SendFailed looks exactly like a wire-format bug. So the probes survive
  // everything the application can do to the queue.
  Rig r;
  r.sync();
  r.d.setSelfAck(true);
  TEST_ASSERT_FALSE(r.d.registered());

  TEST_ASSERT_NOT_EQUAL(kNoTicket, r.d.enqueue(0x151, g_payload, 4));
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(2, r.d.queued(), "2 probes + 1 payload are queued");

  TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, r.d.abortPending(), "only the PAYLOAD is dropped");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, r.d.queued(), "both registration jobs survive");

  pumpUntilIdle(r.d);
  static const Frame kProbes[] = {
      {0x151, 8, {0x70, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, false},
      {0x1F1, 8, {0x70, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, false},
  };
  affatest::expectFrames(r.link, kProbes, 2, "the probes still went out, in table order");
  TEST_ASSERT_TRUE_MESSAGE(r.d.registered(), "and FUNCSREG still latched");
}

void test_urgent_overtakes_normal_but_never_a_registration_job(void) {
  // Urgent jumps the queue ahead of Normal work, never splits a message already on the
  // wire, and never overtakes a pending function registration.
  Rig r;
  r.sync();
  r.d.setSelfAck(true);
  TEST_ASSERT_FALSE(r.d.registered());

  uint8_t normal[4] = {0xA1, 0xA2, 0xA3, 0xA4};
  TEST_ASSERT_NOT_EQUAL(kNoTicket, r.d.enqueue(0x151, normal, sizeof(normal)));

  uint8_t hot[4] = {0xE1, 0xE2, 0xE3, 0xE4};
  TxOptions opt;
  opt.priority = Priority::Urgent;
  TEST_ASSERT_NOT_EQUAL(kNoTicket, r.d.enqueue(0x151, hot, sizeof(hot), opt));

  pumpUntilIdle(r.d);
  static const Frame kOrder[] = {
      {0x151, 8, {0x70, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, false},
      {0x1F1, 8, {0x70, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, false},
      {0x151, 8, {0xE1, 0xE2, 0xE3, 0xE4, 0x00, 0x00, 0x00, 0x00}, false},
      {0x151, 8, {0xA1, 0xA2, 0xA3, 0xA4, 0x00, 0x00, 0x00, 0x00}, false},
  };
  affatest::expectFrames(r.link, kOrder, 4,
                         "both probes first, then Urgent, then the Normal it overtook");
}

// ---------------------------------------------------------------------------
// abortAll
// ---------------------------------------------------------------------------

void test_abortAll_abandons_at_a_frame_boundary_and_the_next_message_starts_clean(void) {
  // The continuation counter is reset on EVERY completion path, abandonment included: an
  // abandoned ISO-TP sequence that left a counter behind would corrupt the NEXT message's
  // first frame, which would then start at a stray 0x2n instead of its own byte 0.
  Rig r;
  r.up();

  uint8_t big[22];
  for (uint8_t i = 0; i < sizeof(big); ++i) big[i] = static_cast<uint8_t>(0x10 + i);
  const TxTicket t = r.d.enqueue(0x151, big, sizeof(big));
  r.d.poll();                                   // frame 0 out
  TEST_ASSERT_EQUAL_UINT32(1, r.link.sentCount());

  TEST_ASSERT_TRUE_MESSAGE(r.d.abortAll(), "a started job was abandoned");
  r.d.poll();                                   // honoured at the next frame boundary
  ASSERT_RESULT(Aborted, r.d.lastResult());
  TEST_ASSERT_EQUAL_UINT16(t, r.d.lastTicket());
  TEST_ASSERT_FALSE(r.d.busy());
  drain(r.link);

  uint8_t next[4] = {0x99, 0x88, 0x77, 0x66};
  TEST_ASSERT_NOT_EQUAL(kNoTicket, r.d.enqueue(0x151, next, sizeof(next)));
  pumpUntilIdle(r.d);
  static const Frame kWant[] = {
      {0x151, 8, {0x99, 0x88, 0x77, 0x66, 0x00, 0x00, 0x00, 0x00}, false},
  };
  affatest::expectFrames(r.link, kWant, 1, "the next message starts at ITS OWN frame 0");
}

void test_abortAll_with_nothing_started_returns_false(void) {
  Rig r;
  r.up();
  TEST_ASSERT_FALSE(r.d.abortAll());
  r.d.enqueue(0x151, g_payload, 4);
  TEST_ASSERT_FALSE_MESSAGE(r.d.abortAll(),
                            "a queued-but-unstarted job is dropped, not abandoned");
  TEST_ASSERT_FALSE(r.d.busy());
}

// ---------------------------------------------------------------------------

void setUp(void) {}
void tearDown(void) {}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_key_reaches_the_callback_in_one_poll_with_an_empty_queue);
  RUN_TEST(test_a_key_reaches_the_callback_in_one_poll_with_a_full_queue);
  RUN_TEST(test_a_burst_that_arrived_between_polls_is_delivered_in_full);
  RUN_TEST(test_coalescing_keeps_one_entry_carrying_the_last_value);
  RUN_TEST(test_different_slots_never_coalesce_against_each_other);
  RUN_TEST(test_abortPending_reports_Aborted_once_per_dropped_ticket);
  RUN_TEST(test_abortPending_never_touches_a_registration_job);
  RUN_TEST(test_urgent_overtakes_normal_but_never_a_registration_job);
  RUN_TEST(test_abortAll_abandons_at_a_frame_boundary_and_the_next_message_starts_clean);
  RUN_TEST(test_abortAll_with_nothing_started_returns_false);
  return UNITY_END();
}
