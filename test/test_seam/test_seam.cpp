// The observation seam — ONE LAYER, as of 2.0, plus the callbacks that ship with it.
//
// There were three. Layer 1 was a fixed table of FrameMatch subscriptions and Layer 2 was a
// decoded event sink; both were deleted, and the evidence is in the comment above Direction
// in core/AffaTypes.h — across nineteen shipped examples neither was called once, while
// Layer 0 was called by seven.
//
// WHAT THIS SUITE NOW PROVES, and it is the claim the deletion rests on: everything Layer 1
// did is three lines inside a Layer 0 tap. The two re-entrancy tests below used to hook a
// transmitted frame with `subscribe(exactId(0x151, Direction::Tx), cb)`. They now hook it
// with `onFrame(cb)` and an `if` — same assertions, same failures caught, one seam.

#include "../affa_test_support.h"

#include "carminat/CarminatDisplay.h"
#include <cstring>

using namespace affa;
using affatest::mk;
using affatest::drain;
using affatest::pumpUntilIdle;

namespace {

// ---------------------------------------------------------------------------
// Recorders
// ---------------------------------------------------------------------------

int g_tapRx = 0, g_tapTx = 0;
uint32_t g_tapLastId = 0;
void tapAll(const Frame& f, Direction d, void*) {
  if (d == Direction::Rx) ++g_tapRx; else ++g_tapTx;
  g_tapLastId = f.id;
}
int g_tap2 = 0;
void tapSecond(const Frame&, Direction, void*) { ++g_tap2; }

// The two callbacks that DO ship with the library, recorded so the tests below can assert
// when they fire rather than that they exist.
struct CbLog {
  int       sync       = 0;
  SyncState now        = SyncState::None;
  int       completes  = 0;
  TxTicket  lastTicket = kNoTicket;
  Result    lastResult = Result::Ok;
};
CbLog g_cb;

void recordSync(SyncState s, void*) { ++g_cb.sync; g_cb.now = s; }
void recordComplete(TxTicket t, Result r, void*) {
  ++g_cb.completes;
  g_cb.lastTicket = t;
  g_cb.lastResult = r;
}

struct Rig {
  LoopbackLink<256> link;
  affatest::FakeClock clk;
  CarminatDisplay d;
  Rig() : d(link, clk) {}

  void sync(bool selfAck = false) {
    d.begin();
    // The 0x70 registrations leave with the final hello frame on this family, so a rig that
    // wants them acknowledged must arm the emulator before the opening, not after.
    d.setSelfAck(selfAck);
    affatest::completeCarminatAuth(d, link, clk);
    TEST_ASSERT_TRUE(d.synced());
    drain(link);
  }
  void up() {
    sync(/*selfAck=*/true);
    (void)d.setPower(true);
    affatest::settleCarminatRegistration(d, clk);
    pumpUntilIdle(d);
    TEST_ASSERT_TRUE(d.registered());
    drain(link);
  }
};

}  // namespace

// ---------------------------------------------------------------------------
// Re-entrancy from a transmitted frame
//
// docs/API.md §4.3 permits enqueue(), abortPending() and abortAll() from ANY callback. The
// tap fires from inside observe(), which txFrame() calls from inside pumpTx() — i.e. while
// the job that produced the frame is sitting at _queue[0] and the transmit pump still holds
// a reference to it. Everything that decides whether a job is preemptable keys off
// TxJob::started, so `started` MUST already be true by the time that callback can run.
// ---------------------------------------------------------------------------

namespace {

struct TxReentry {
  CarminatDisplay* d       = nullptr;
  int              hits    = 0;
  uint8_t          aborted = 0xFF;
  Result           second  = Result::NotSupported;
};
TxReentry g_re;

// THE THREE LINES THAT REPLACED LAYER 1. A direction test and an id test, written where the
// frame is, instead of a FrameMatch built and stored in a table that was scanned twice per
// frame to reach exactly this comparison.
inline bool ourTxFrame(const Frame& f, Direction d) {
  return d == Direction::Tx && f.id == 0x151;
}

void abortFromTxCallback(const Frame& f, Direction d, void*) {
  if (!ourTxFrame(f, d)) return;
  if (++g_re.hits != 1) return;              // only on the very first transmitted frame
  g_re.aborted = g_re.d->abortPending();
}

void renderFromTxCallback(const Frame& f, Direction d, void*) {
  if (!ourTxFrame(f, d)) return;
  if (++g_re.hits != 1) return;
  g_re.second = g_re.d->showMenu("XXX", "YYY", "ZZZ").result;
}

}  // namespace

void test_abortPending_from_a_tx_callback_spares_the_frame_it_is_watching(void) {
  Rig r;
  r.up();
  g_re = TxReentry{};
  g_re.d = &r.d;

  const Submitted sent = r.d.showMenu("ONE", "TWO", "SIX");  // multi-frame, RenderSlot::Menu
  ASSERT_RESULT(Ok, sent);
  const TxTicket menu = sent.ticket;
  ASSERT_RESULT(Ok, r.d.setTime("1234"));                 // queued behind it
  TEST_ASSERT_EQUAL_UINT8(1, r.d.queued());

  r.d.onFrame(&abortFromTxCallback, nullptr);

  r.d.poll();   // pumpTx sends the menu's frame 0; the tap fires inside that send

  TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_re.hits, "the Tx callback must have fired");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(
      1, g_re.aborted,
      "only the queued clock render is preemptable — the menu already has a frame on the "
      "wire and abortPending() must not touch it");
  TEST_ASSERT_TRUE_MESSAGE(r.d.busy(), "the in-flight menu survives its own Tx callback");

  r.d.onFrame(nullptr, nullptr);            // stop counting; the drain below is not the test
  pumpUntilIdle(r.d);
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(menu, r.d.lastTicket(),
                                   "and completes, rather than being reported Aborted");
  ASSERT_RESULT(Ok, r.d.lastResult());
}

void test_a_render_from_a_tx_callback_cannot_coalesce_into_the_frame_on_the_wire(void) {
  // Same slot, same funcId, coalescing on. findCoalescable() must skip the started job:
  // overwriting its payload mid-ISO-TP would send the tail of a DIFFERENT screen at the
  // offsets the first one already declared.
  Rig r;
  r.up();
  g_re = TxReentry{};
  g_re.d = &r.d;

  ASSERT_RESULT(Ok, r.d.showMenu("ONE", "TWO", "SIX"));
  TEST_ASSERT_EQUAL_UINT8(0, r.d.queued());

  r.d.onFrame(&renderFromTxCallback, nullptr);

  r.d.poll();

  TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_re.hits, "the Tx callback must have fired");
  ASSERT_RESULT(Ok, g_re.second);
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(
      1, r.d.queued(),
      "the second render must QUEUE behind the message on the wire, not replace it");
}

// ---------------------------------------------------------------------------
// The tap
// ---------------------------------------------------------------------------

void test_the_tap_sees_both_directions_and_only_one_tap_exists(void) {
  Rig r;
  r.up();
  g_tapRx = g_tapTx = 0;
  g_tap2 = 0;
  r.d.onFrame(&tapAll, nullptr);

  r.link.inject(mk(0x1C1, {0x70, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3}));
  r.d.poll();                     // one inbound, one auto-ACK out
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_tapRx, "the inbound frame");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_tapTx, "and the 0x5C1 ACK it produced");
  TEST_ASSERT_EQUAL_HEX32(0x5C1, g_tapLastId);

  // A second call REPLACES the first; there is exactly one tap.
  r.d.onFrame(&tapSecond, nullptr);
  const int rxBefore = g_tapRx;
  r.link.inject(mk(0x1C1, {0x70, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3}));
  r.d.poll();
  TEST_ASSERT_EQUAL_INT_MESSAGE(rxBefore, g_tapRx, "the first tap was replaced");
  TEST_ASSERT_EQUAL_INT(2, g_tap2);

  // nullptr removes it.
  r.d.onFrame(nullptr, nullptr);
  r.link.inject(mk(0x1C1, {0x70, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3}));
  r.d.poll();
  TEST_ASSERT_EQUAL_INT(2, g_tap2);
  drain(r.link);
}

void test_a_refused_transmission_is_never_observed(void) {
  // A frame the link REFUSED never existed on the bus, so the tap must not see it — a
  // sniffer that showed it would be showing traffic that was not there.
  Rig r;
  r.sync();
  g_tapRx = g_tapTx = 0;
  r.d.onFrame(&tapAll, nullptr);

  r.link.setLive(false);
  r.clk.advance(1000);
  r.d.poll();                     // pumpSync tries to transmit the heartbeat and fails
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_tapTx, "a refused frame is not observed");
}

// ---------------------------------------------------------------------------
// The callbacks that ship with the library
// ---------------------------------------------------------------------------

// This asserted the same three moments through the Layer 2 event sink until 2.0. The sink
// carried nothing SyncCb and CompleteCb did not already carry — SyncChanged duplicated
// SyncCb, TxComplete duplicated CompleteCb byte for byte — so the assertions moved onto the
// callbacks that were always the real delivery, and the sink went.
void test_registration_latches_once_and_only_payload_tickets_complete(void) {
  Rig r;
  g_cb = CbLog{};
  r.d.begin();
  r.d.onSync(&recordSync, nullptr);
  r.d.onComplete(&recordComplete, nullptr);
  // Armed here, not after the burst: the opening itself emits the 0x70 probes with the final
  // B0, so an emulator armed later would leave the first one waiting for an ACK for ever.
  r.d.setSelfAck(true);

  // begin() writes SyncState::Failed DIRECTLY rather than transitioning to it — firing a
  // callback from inside setup() would surprise every application — so nothing has fired.
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_cb.sync, "begin() is a reset, not a transition");

  affatest::completeCarminatAuth(r.d, r.link, r.clk);
  TEST_ASSERT_TRUE(r.d.synced());
  TEST_ASSERT_FALSE_MESSAGE(r.d.registered(), "FUNCSREG has not latched yet");
  const int syncsBeforeRegistration = g_cb.sync;
  TEST_ASSERT_TRUE_MESSAGE(syncsBeforeRegistration > 0, "the opening moved the state word");

  const Submitted power = r.d.setPower(true);
  ASSERT_RESULT(Ok, power);
  affatest::settleCarminatRegistration(r.d, r.clk);
  pumpUntilIdle(r.d);

  TEST_ASSERT_TRUE_MESSAGE(r.d.registered(), "FUNCSREG latches");
  TEST_ASSERT_TRUE_MESSAGE(g_cb.sync > syncsBeforeRegistration,
                           "and the latch is itself a state change SyncCb reports");
  TEST_ASSERT_TRUE(hasFlag(g_cb.now, SyncState::FuncsReg));

  // THE POINT OF THE TICKET ASSERTIONS: the two 0x70 registration probes go out on the same
  // wire as the payload and are acknowledged the same way, but they carry kNoTicket and are
  // invisible to the application. One completion, and it is the setPower.
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_cb.completes, "one payload ticket completed");
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(power.ticket, g_cb.lastTicket,
                                   "registration jobs carry kNoTicket and are invisible");
  ASSERT_RESULT(Ok, g_cb.lastResult);
}


// ---------------------------------------------------------------------------

void setUp(void) {}
void tearDown(void) {}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_abortPending_from_a_tx_callback_spares_the_frame_it_is_watching);
  RUN_TEST(test_a_render_from_a_tx_callback_cannot_coalesce_into_the_frame_on_the_wire);
  RUN_TEST(test_the_tap_sees_both_directions_and_only_one_tap_exists);
  RUN_TEST(test_a_refused_transmission_is_never_observed);
  RUN_TEST(test_registration_latches_once_and_only_payload_tickets_complete);
  return UNITY_END();
}
