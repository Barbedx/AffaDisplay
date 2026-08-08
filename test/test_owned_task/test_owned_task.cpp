// The threading contract, tested where it is portable.
//
// There is no FreeRTOS on the host, so this suite does NOT test the task itself. It tests
// the two halves of the contract that live in core/ and are therefore testable in full:
//
//   * poll() refusing a call from a task that is not the poll owner, and counting it. Two
//     tasks pumping one instance corrupts the transmit FSM, silently.
//   * THE CROSS-TASK BOUNDARY inside enqueue() — a render from any other task accepted,
//     posted, and admitted on the owner with the ticket the caller was already given.
//
// It used to test Command / applyCommand / RequestTable as well: the command queue AffaTask
// published, its second handle space, and the table that mapped one onto the other. All
// three are gone. The boundary moved into enqueue() where every render already funnels, so
// there is no op table to keep in step with the panels and no second handle to translate —
// which is the bug those tests were guarding against rather than a feature they protected.
//
// The key-latency acceptance criterion cannot be proved on the host — it is about task
// scheduling — and is asserted on hardware by the demo.

#include <unity.h>
#include <cstring>

#include "AffaConfig.h"
#include "core/AffaDisplayBase.h"
#include "link/LoopbackLink.h"
#include "../affa_test_support.h"

using namespace affa;
using affatest::FakeClock;

namespace {

// ---------------------------------------------------------------------------
// A display that records what it was asked to do, and nothing else
// ---------------------------------------------------------------------------
constexpr uint8_t kHello[3][8] = {
  {0x70, 0x1A, 0x11, 0x00, 0x00, 0x00, 0x00, 0x01},
  {0xB0, 0x14, 0x11, 0x00, 0x1F, 0x00, 0x00, 0x00},
  {0xB0, 0x14, 0x11, 0x00, 0x1F, 0x00, 0x00, 0x00},
};
constexpr SyncProfile kProfile{0x3AF, 0x3CF, 0x0400, 0xB9, 0xBA, 0x00, 0x00, kHello, 3};
constexpr uint16_t kFuncIds[] = {0x151, 0x1F1};

// A display whose setText actually enqueues, so the ticket path is real rather than mocked.
class WireDisplay final : public AffaDisplayBase {
 public:
  WireDisplay(ICanLink& l, IClock& c) : AffaDisplayBase(l, c, kProfile, kFuncIds, 2) {}
  bool supports(Feature) const override { return true; }

  // NAME HIDING, not decoration — the same line CarminatDisplay carries and for the same
  // reason: the protected `bool onFrame(const Frame&)` below hides EVERY base member called
  // onFrame, including the public tap `void onFrame(FrameTap, void*)`.
  using AffaDisplayBase::onFrame;

  // ONE FRAME, deliberately: with LoopbackLink's auto-ACK answering DONE to everything, a
  // multi-frame message completes at frame 0 and its continuations never reach the wire.
  // Eight payload bytes keeps the whole message visible to an ordering assertion.
  Submitted setText(const char* t, uint8_t) override {
    uint8_t d[8] = {0x05, 0x77, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00};
    for (uint8_t i = 0; i < 4 && t && t[i]; ++i) d[2 + i] = static_cast<uint8_t>(t[i]);
    TxOptions o;
    o.slot     = RenderSlot::None;   // no coalescing: this suite is about ORDER
    o.coalesce = false;
    return enqueue(0x151, d, sizeof(d), o);
  }

 protected:
  uint8_t  packetFiller() const override { return 0x00; }
  uint16_t keyTxId()      const override { return 0x1C1; }

  // Route a key frame the way a real panel class does. Without this a `03 89` on 0x1C1 is
  // acknowledged and then dropped, so KeyCb never fires — which is exactly how the first
  // draft of the callback-attribution test below managed to measure nothing at all.
  bool onFrame(const Frame& f) override {
    if (f.id != 0x1C1) return false;
    Key k; KeyEdge e;
    if (!decodeKeyFrame(f, k, e)) return false;
    routeKey(k, e);
    return true;
  }
};

// The poll-owner seam, driven by hand. On the target this is xTaskGetCurrentTaskHandle();
// here it is a variable a test can move between two "tasks".
void* g_currentTask = nullptr;
void* currentTask() { return g_currentTask; }

template <class D, class L>
void bringUpSync(D& d, L& link, FakeClock& clk) {
  d.begin();
  link.inject(affatest::panelSyncRequest());
  d.poll();
  clk.advance(AFFA_SYNC_INTERVAL_MS + 1);
  d.poll();
  affatest::drain(link);
}

// Advance the clock the way a running system does: in small steps, polling, with the
// panel's ~1 Hz ping still arriving. A single big clk.advance() would trip the peer
// watchdog and quietly turn every retry test below into a sync test — which is exactly what
// the first draft of these did.
template <class D>
void advanceAlive(D& d, LoopbackLink<>& link, FakeClock& clk, uint32_t ms) {
  constexpr uint32_t kStep = 100;
  for (uint32_t t = 0; t < ms; t += kStep) {
    clk.advance(kStep);
    link.inject(affatest::panelPeerAlive());
    d.poll();
  }
}

// Payload frames of WireDisplay::setText, ignoring heartbeats, ACKs and registration.
uint32_t takeTextFrames(LoopbackLink<>& link) {
  uint32_t n = 0;
  Frame f;
  while (link.takeSent(f))
    if (f.id == 0x151 && f.data[0] == 0x05 && f.data[1] == 0x77) ++n;
  return n;
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Tickets
// ---------------------------------------------------------------------------

void test_a_render_into_a_dead_link_is_held_not_refused() {
  LoopbackLink<> link;
  FakeClock clk;
  WireDisplay d(link, clk);
  d.begin();                                  // begun, but never synced

  // The owned task posts this from an application thread that has no idea whether the panel
  // is awake, and it must not have to care: the command is accepted, the caller gets a
  // handle, and the library holds the job until the link is usable.
  const Submitted s = d.setText("ONE", 255);
  ASSERT_RESULT(Ok, s);
  TEST_ASSERT_NOT_EQUAL(kNoTicket, s.ticket);

  affatest::drain(link);
  for (int i = 0; i < 5; ++i) d.poll();
  Frame f;
  while (link.takeSent(f))
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0x151u, f.id, "a held job must not transmit before sync");
}

void test_a_permanently_bad_render_is_still_refused_at_the_call() {
  LoopbackLink<> link;
  FakeClock clk;
  WireDisplay d(link, clk);
  bringUpSync(d, link, clk);

  // The line between "hold it" and "refuse it" is whether waiting could ever help. A null
  // buffer, an over-long payload and an unknown function id are the caller's mistakes, and
  // holding one would turn a programming error into a silent eight-second delay.
  TEST_ASSERT_EQUAL_UINT16(kNoTicket, d.enqueue(0x151, nullptr, 4).ticket);
  ASSERT_RESULT(BadArgument, d.lastResult());

  uint8_t big[AFFA_MAX_PAYLOAD];
  std::memset(big, 0x5A, sizeof(big));
  TEST_ASSERT_EQUAL_UINT16(kNoTicket, d.enqueue(0x151, big, AFFA_MAX_PAYLOAD + 1).ticket);
  ASSERT_RESULT(TooLong, d.lastResult());

  uint8_t one = 0x11;
  TEST_ASSERT_EQUAL_UINT16(kNoTicket, d.enqueue(0x999, &one, 1).ticket);
  ASSERT_RESULT(UnknownFunc, d.lastResult());
}

// ---------------------------------------------------------------------------
// Delivery: what the library does about a transfer that did not land
// ---------------------------------------------------------------------------
// These are the tests for "the application is not the recovery layer". Every one of them
// describes something a consumer used to have to write, and got wrong at least once.

void test_a_timeout_is_retried_before_the_application_hears_about_it() {
  LoopbackLink<> link;
  FakeClock clk;
  WireDisplay d(link, clk);
  bringUpSync(d, link, clk);
  link.setAutoAck(true);
  ASSERT_RESULT(Ok, d.setText("WARM", 255));      // registration out of the way
  affatest::pumpUntilIdle(d);
  affatest::drain(link);

  static int completions = 0;
  static Result lastResult = Result::Ok;
  completions = 0;
  d.onComplete([](TxTicket, Result r, void*) { ++completions; lastResult = r; }, nullptr);

  link.setAutoAck(false);                         // the panel goes quiet
  ASSERT_RESULT(Ok, d.setText("ONE", 255));

  // Attempt 1 times out. The application hears NOTHING: the job is still in the queue with
  // a backoff on it, which is the entire point.
  advanceAlive(d, link, clk, AFFA_ACK_TIMEOUT_MS + 100);
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, completions, "a retryable failure is not reported");
  TEST_ASSERT_TRUE_MESSAGE(d.busy(), "the job is still queued for another attempt");

  // And it does not transmit again immediately either — that spin is what the backoff
  // exists to prevent, and what a consumer's own retry loop got wrong.
  affatest::drain(link);
  advanceAlive(d, link, clk, 200);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, takeTextFrames(link), "no retry before the backoff");

  // After the backoff it goes out again, and this time the panel answers.
  link.setAutoAck(true);
  advanceAlive(d, link, clk, AFFA_TX_RETRY_MS + AFFA_TX_DIRTY_QUIET_MS + 400);
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, completions, "exactly one verdict per ticket");
  ASSERT_RESULT(Ok, lastResult);
  d.onComplete(nullptr, nullptr);
}

void test_retries_are_bounded_and_the_last_failure_is_reported() {
  LoopbackLink<> link;
  FakeClock clk;
  WireDisplay d(link, clk);
  bringUpSync(d, link, clk);
  link.setAutoAck(true);
  ASSERT_RESULT(Ok, d.setText("WARM", 255));
  affatest::pumpUntilIdle(d);
  affatest::drain(link);

  static int completions = 0;
  static Result lastResult = Result::Ok;
  completions = 0;
  d.onComplete([](TxTicket, Result r, void*) { ++completions; lastResult = r; }, nullptr);

  link.setAutoAck(false);                         // the panel never answers again
  ASSERT_RESULT(Ok, d.setText("ONE", 255));

  // AFFA_TX_MAX_RETRIES + 1 attempts, then it gives up and says so ONCE. A library that
  // retried for ever would be a library that never tells you the panel is gone.
  for (int i = 0; i <= AFFA_TX_MAX_RETRIES; ++i)
    advanceAlive(d, link, clk,
                 AFFA_ACK_TIMEOUT_MS + AFFA_TX_RETRY_MAX_MS + AFFA_TX_DIRTY_QUIET_MS + 200);
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, completions, "one verdict, after the attempts are spent");
  ASSERT_RESULT(Timeout, lastResult);
  TEST_ASSERT_FALSE(d.busy());
  d.onComplete(nullptr, nullptr);
}

void test_a_rejection_by_the_panel_is_never_retried() {
  LoopbackLink<> link;
  FakeClock clk;
  WireDisplay d(link, clk);
  bringUpSync(d, link, clk);
  link.setAutoAck(true);
  ASSERT_RESULT(Ok, d.setText("WARM", 255));
  affatest::pumpUntilIdle(d);
  affatest::drain(link);

  static int completions = 0;
  static Result lastResult = Result::Ok;
  completions = 0;
  d.onComplete([](TxTicket, Result r, void*) { ++completions; lastResult = r; }, nullptr);

  link.setAutoAck(false);
  ASSERT_RESULT(Ok, d.setText("ONE", 255));
  d.poll();                                        // frame 0 goes out

  // The panel ANSWERED, and the answer was neither DONE nor PARTIAL. Re-sending identical
  // bytes to a panel that has just rejected them gets the same answer three more times and
  // buries the diagnostic. Reported immediately, once.
  Frame bad = affatest::mk(0x151 | affa::kReplyFlag, {0x7F, 0x00, 0x00, 0x00, 0, 0, 0, 0});
  link.inject(bad);
  d.poll();

  TEST_ASSERT_EQUAL_INT_MESSAGE(1, completions, "a rejection is reported at once");
  ASSERT_RESULT(SendFailed, lastResult);
  TEST_ASSERT_FALSE_MESSAGE(d.busy(), "and it is NOT left in the queue for another go");
  d.onComplete(nullptr, nullptr);
}

void test_a_held_job_survives_a_resync_and_registers_itself_again() {
  LoopbackLink<> link;
  FakeClock clk;
  WireDisplay d(link, clk);
  bringUpSync(d, link, clk);
  link.setAutoAck(true);
  ASSERT_RESULT(Ok, d.setText("WARM", 255));
  affatest::pumpUntilIdle(d);
  TEST_ASSERT_TRUE(d.registered());

  // The panel goes away long enough for the watchdog to declare it lost. FUNCSREG goes with
  // it — the panel has forgotten us.
  link.setAutoAck(false);
  clk.advance(AFFA_PEER_TIMEOUT_MS + AFFA_SYNC_INTERVAL_MS + 1);
  d.poll();
  TEST_ASSERT_FALSE_MESSAGE(d.synced(), "peer loss");
  TEST_ASSERT_FALSE_MESSAGE(d.registered(), "FUNCSREG goes with the peer");

  // A render issued while the panel is away. The application does not know and must not
  // have to: this used to be rejected with NoSync, and before that it would have been
  // destroyed by the peer-loss teardown.
  ASSERT_RESULT(Ok, d.setText("LATE", 255));
  affatest::drain(link);

  // The panel comes back.
  link.setAutoAck(true);
  link.inject(affatest::panelSyncRequest());
  clk.advance(AFFA_SYNC_INTERVAL_MS + 1);
  affatest::pumpUntilIdle(d, 800);

  // It re-registered ITSELF — the burst was spliced in front of the held render by the
  // transmit pump, not by the application noticing the sync event.
  TEST_ASSERT_TRUE_MESSAGE(d.registered(), "the library re-registered without being asked");

  bool sawReg = false, sawText = false;
  Frame f;
  while (link.takeSent(f)) {
    if (f.id == 0x151 && f.data[0] == affa::kRegisterByte) sawReg = true;
    if (f.id == 0x151 && f.data[0] == 0x05 && f.data[1] == 0x77 && f.data[2] == 'L')
      sawText = true;
  }
  TEST_ASSERT_TRUE_MESSAGE(sawReg,  "the registration probe went out again");
  TEST_ASSERT_TRUE_MESSAGE(sawText, "and the held render landed after it");
}

void test_a_flapping_link_does_not_spend_the_retry_budget() {
  LoopbackLink<> link;
  FakeClock clk;
  WireDisplay d(link, clk);
  bringUpSync(d, link, clk);
  link.setAutoAck(true);
  ASSERT_RESULT(Ok, d.setText("WARM", 255));
  affatest::pumpUntilIdle(d);
  affatest::drain(link);

  static int completions = 0;
  static Result lastResult = Result::Ok;
  completions = 0;
  d.onComplete([](TxTicket, Result r, void*) { ++completions; lastResult = r; }, nullptr);

  // autoAck OFF, so the job sits in WaitAck and the controller can drop out from under it —
  // which is the path this test is about. With it on, the render would complete before the
  // link ever had a chance to flap.
  link.setAutoAck(false);
  ASSERT_RESULT(Ok, d.setText("FLAP", 255));

  // The controller drops off the bus and comes back, over and over — 8% of the time not
  // RUNNING was the measured rig behaviour. More cycles than AFFA_TX_MAX_RETRIES, so if a
  // link fault spent an attempt this render would be given up as LinkDown, which is what it
  // did before: 45 renders reported failed in twelve minutes with nothing wrong with them.
  for (int i = 0; i < AFFA_TX_MAX_RETRIES + 2; ++i) {
    link.setLive(true);
    advanceAlive(d, link, clk, 700);      // long enough to clear the backoff and transmit
    link.setLive(false);
    advanceAlive(d, link, clk, 100);      // ...and then the controller goes away again
  }
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, completions, "a link fault is not the render's fault");

  // And when the link finally stays up and the panel answers, it lands.
  link.setLive(true);
  link.setAutoAck(true);
  advanceAlive(d, link, clk, AFFA_TX_RETRY_MAX_MS + AFFA_TX_DIRTY_QUIET_MS + 500);
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, completions, "and then it is delivered");
  ASSERT_RESULT(Ok, lastResult);
  d.onComplete(nullptr, nullptr);
}

void test_a_link_that_never_returns_still_gives_up() {
  LoopbackLink<> link;
  FakeClock clk;
  WireDisplay d(link, clk);
  bringUpSync(d, link, clk);
  link.setAutoAck(true);
  ASSERT_RESULT(Ok, d.setText("WARM", 255));
  affatest::pumpUntilIdle(d);

  static int completions = 0;
  static Result lastResult = Result::Ok;
  completions = 0;
  d.onComplete([](TxTicket, Result r, void*) { ++completions; lastResult = r; }, nullptr);

  ASSERT_RESULT(Ok, d.setText("GONE", 255));
  link.setLive(false);

  // Not spending a retry must not mean waiting for ever: the hold window is what bounds
  // this path, and it is deliberately NOT re-armed by a link fault.
  advanceAlive(d, link, clk, AFFA_TX_HOLD_MS + 1000);
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, completions, "the hold window still ends it");
  ASSERT_RESULT(LinkDown, lastResult);
  d.onComplete(nullptr, nullptr);
}

void test_a_held_job_is_given_up_rather_than_waiting_for_ever() {
  LoopbackLink<> link;
  FakeClock clk;
  WireDisplay d(link, clk);
  d.begin();                                       // never synced at all

  static int completions = 0;
  static Result lastResult = Result::Ok;
  completions = 0;
  d.onComplete([](TxTicket, Result r, void*) { ++completions; lastResult = r; }, nullptr);

  ASSERT_RESULT(Ok, d.setText("NEVER", 255));
  clk.advance(AFFA_TX_HOLD_MS / 2);
  affatest::pump(d, 3);
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, completions, "still holding, still hoping");

  // A screen that appears ninety seconds late is worse than one that never appeared, so the
  // hold is bounded and the application gets told.
  clk.advance(AFFA_TX_HOLD_MS);
  affatest::pump(d, 3);
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, completions, "the hold window is bounded");
  ASSERT_RESULT(NoSync, lastResult);
  d.onComplete(nullptr, nullptr);
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Poll ownership (CR §6.2)
// ---------------------------------------------------------------------------

void test_poll_from_a_foreign_task_does_nothing_and_is_counted() {
  LoopbackLink<> link;
  FakeClock clk;
  WireDisplay d(link, clk);
  d.begin();

  void* const owner   = reinterpret_cast<void*>(0xA1);
  void* const foreign = reinterpret_cast<void*>(0xB2);

  g_currentTask = owner;
  d.setPollOwner(owner, &currentTask);

  // The owner polls: the heartbeat goes out, as it always has.
  affatest::drain(link);
  d.poll();
  TEST_ASSERT_TRUE(link.sentCount() > 0);
  TEST_ASSERT_EQUAL_UINT32(0, d.foreignPolls());

  // Somebody else polls: NOTHING happens, and it is counted rather than tolerated. Two
  // tasks pumping one instance corrupts the transmit FSM silently, which is the whole
  // reason this guard exists.
  affatest::drain(link);
  clk.advance(AFFA_SYNC_INTERVAL_MS + 1);
  g_currentTask = foreign;
  d.poll();
  d.poll();
  TEST_ASSERT_EQUAL_UINT32(0, link.sentCount());
  TEST_ASSERT_EQUAL_UINT32(2, d.foreignPolls());

  // Ownership released: unchecked again, which is the caller-owned mode every other
  // example in this repository runs in.
  d.setPollOwner(nullptr, nullptr);
  d.poll();
  TEST_ASSERT_TRUE(link.sentCount() > 0);
  TEST_ASSERT_EQUAL_UINT32(2, d.foreignPolls());
  g_currentTask = nullptr;
}

// ---------------------------------------------------------------------------
// Cross-task dispatch — the point of the whole 2.0 refactor
// ---------------------------------------------------------------------------
// Before 2.0, poll() was guarded against a foreign task and enqueue() was NOT. An
// application holding the display — which every application did, because AffaTask published
// ten of CarminatDisplay's twenty-two renders — could call setText from an HTTP handler
// straight into the transmit queue while the owned task was pumping it. That is what
// examples/17_mediascreen actually did (docs/API.md §7).
//
// These drive the boundary with the same faked task identity the poll guard above uses.

namespace {
void* const kOwner   = reinterpret_cast<void*>(0xA1);
void* const kForeign = reinterpret_cast<void*>(0xB2);

// Bring a WireDisplay up to "renders are accepted", then hand ownership to kOwner.
// `ack` arms the panel emulator so a transfer can actually complete; the tests that only
// care about where a call LANDS leave it off.
void ownedAndSynced(WireDisplay& d, LoopbackLink<>& link, FakeClock& clk, bool ack = false) {
  g_currentTask = kOwner;
  bringUpSync(d, link, clk);
  if (ack) link.setAutoAck(true);
  d.setPollOwner(kOwner, &currentTask);
  affatest::drain(link);
}
}  // namespace

void test_a_render_from_a_foreign_task_is_accepted_and_applied_on_the_owner(void) {
  LoopbackLink<> link;
  FakeClock clk;
  WireDisplay d(link, clk);
  ownedAndSynced(d, link, clk, /*ack=*/true);

  const uint8_t qBefore = d.queued();

  // THE CALL THAT USED TO BE A DATA RACE. It returns a live ticket immediately and never
  // blocks — but nothing has touched the transmit queue yet.
  g_currentTask = kForeign;
  const Submitted s = d.setText("HI", 255);
  TEST_ASSERT_TRUE_MESSAGE(s.ok(), "a foreign task is never refused for being foreign");
  TEST_ASSERT_NOT_EQUAL(kNoTicket, s.ticket);
  ASSERT_RESULT(Ok, s);
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, d.dispatchQueued(), "it is posted, not applied");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(qBefore, d.queued(),
                                  "the transmit queue is untouched until the owner runs");

  // The owner's next poll admits it, and the ticket the caller holds is the one that ends
  // up on the job — not a second handle minted on this side.
  g_currentTask = kOwner;
  d.poll();
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, d.dispatchQueued(), "the ring drained");
  affatest::pumpUntilIdle(d);
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(s.ticket, d.lastTicket(),
                                   "the ticket minted for the caller is the one that completed");
  ASSERT_RESULT(Ok, d.lastResult());
  g_currentTask = nullptr;
}

void test_the_callers_own_mistakes_are_still_answered_synchronously(void) {
  // The three checks that need NO library state stay above the boundary, so a foreign
  // caller still learns about its own bug at the call site rather than through a callback.
  LoopbackLink<> link;
  FakeClock clk;
  WireDisplay d(link, clk);
  ownedAndSynced(d, link, clk);

  g_currentTask = kForeign;
  uint8_t big[AFFA_MAX_PAYLOAD + 1] = {0};
  const uint8_t one = 1;

  ASSERT_RESULT(BadArgument, d.enqueue(0x151, nullptr, 4));
  ASSERT_RESULT(TooLong,     d.enqueue(0x151, big, AFFA_MAX_PAYLOAD + 1));
  ASSERT_RESULT(UnknownFunc, d.enqueue(0x999, &one, 1));
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, d.dispatchQueued(),
                                  "a refused call posts nothing across the boundary");
  g_currentTask = nullptr;
}

void test_a_posted_render_the_queue_refuses_reports_through_onComplete(void) {
  // THE ONE HONEST COST OF THE BOUNDARY. A foreign caller is told "accepted" before the
  // queue has been consulted, so a QueueFull discovered at drain time is owed to it as a
  // completion — never swallowed. It is the same contract a delivery failure already had.
  static int   completions;
  static Result lastResult;
  static TxTicket lastTicket;
  completions = 0; lastResult = Result::Ok; lastTicket = kNoTicket;

  LoopbackLink<> link;
  FakeClock clk;
  WireDisplay d(link, clk);
  ownedAndSynced(d, link, clk);
  d.onComplete([](TxTicket t, Result r, void*) {
    ++completions; lastResult = r; lastTicket = t;
  }, nullptr);

  // Fill the transmit queue from the owner, with the link down so nothing drains.
  link.setLive(false);
  uint8_t p[4] = {0x03, 0x77, 0x41, 0x00};
  for (uint8_t i = 0; i < AFFA_TX_QUEUE_DEPTH; ++i) {
    TxOptions o; o.coalesce = false;
    (void)d.enqueue(0x151, p, sizeof(p), o);
  }
  TEST_ASSERT_EQUAL_UINT8(AFFA_TX_QUEUE_DEPTH - 1, d.queued());
  completions = 0;

  // Now post one from another task. It is ACCEPTED at the call — the ring had room.
  g_currentTask = kForeign;
  const Submitted s = d.setText("XX", 255);
  TEST_ASSERT_TRUE_MESSAGE(s.ok(), "the ring accepted it; the transmit queue has not seen it");

  // …and refused at the drain, which the caller hears about exactly once.
  g_currentTask = kOwner;
  d.poll();
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, completions, "the caller is owed exactly one verdict");
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(s.ticket, lastTicket, "and it names its own ticket");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(static_cast<uint8_t>(Result::QueueFull),
                                  static_cast<uint8_t>(lastResult), "with the real reason");
  g_currentTask = nullptr;
}

void test_a_full_dispatch_ring_refuses_at_the_call_and_counts_it(void) {
  LoopbackLink<> link;
  FakeClock clk;
  WireDisplay d(link, clk);
  ownedAndSynced(d, link, clk);

  g_currentTask = kForeign;
  for (uint16_t i = 0; i < AFFA_DISPATCH_DEPTH; ++i)
    TEST_ASSERT_TRUE_MESSAGE(d.setText("A", 255).ok(), "the ring has room");

  const Submitted over = d.setText("B", 255);
  TEST_ASSERT_FALSE_MESSAGE(over.ok(), "a full ring refuses rather than dropping silently");
  ASSERT_RESULT(QueueFull, over);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, d.dispatchDropped(), "and it is counted");
  g_currentTask = nullptr;
}

void test_a_call_on_the_owning_task_never_touches_the_ring(void) {
  // The direct path has to stay direct: a render from inside a KeyCb — which runs ON the
  // owner — must apply immediately, or the documented preemption pattern
  // (poll -> KeyCb -> abortPending -> CompleteCb) would defer half of itself by a poll.
  LoopbackLink<> link;
  FakeClock clk;
  WireDisplay d(link, clk);
  ownedAndSynced(d, link, clk);

  const Submitted s = d.setText("OK", 255);       // still g_currentTask == kOwner
  TEST_ASSERT_TRUE(s.ok());
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, d.dispatchQueued(), "the owner does not post to itself");
  TEST_ASSERT_TRUE_MESSAGE(d.busy(), "it is in the transmit queue already");
  g_currentTask = nullptr;
}

void test_with_no_owner_registered_every_task_takes_the_direct_path(void) {
  // Caller-owned mode, which is what a non-FreeRTOS port and every pre-2.0 build run in:
  // the contract is "one task, by convention", so there is nothing to cross and the ring is
  // never touched no matter which task calls.
  LoopbackLink<> link;
  FakeClock clk;
  WireDisplay d(link, clk);
  bringUpSync(d, link, clk);
  affatest::drain(link);

  g_currentTask = kForeign;                       // no setPollOwner() call at all
  TEST_ASSERT_TRUE(d.setText("HI", 255).ok());
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, d.dispatchQueued(), "unowned means unchecked");
  TEST_ASSERT_TRUE(d.busy());
  g_currentTask = nullptr;
}

// ---------------------------------------------------------------------------
// A blocking callback names itself
// ---------------------------------------------------------------------------
// It cannot be prevented — callbacks fire inside poll() by design, because that is what
// bounds key latency by the poll period alone. What used to happen instead was
// `pollLateMaxUs = 340000` and a whole application to search. These pin the missing word.

#if AFFA_CALLBACK_BUDGET_MS > 0
namespace {
FakeClock* g_slowClock = nullptr;
uint32_t   g_stallMs   = 0;

// A callback that "blocks": the FakeClock is the only clock the library reads, so moving it
// forward inside the callback IS a stall, exactly as the library measures one.
void stallingKey(Key, KeyEdge, void*) { if (g_slowClock) g_slowClock->advance(g_stallMs); }
void stallingTap(const Frame&, Direction, void*) {
  if (g_slowClock) g_slowClock->advance(g_stallMs);
}
}  // namespace

void test_a_blocking_callback_is_named_not_merely_counted(void) {
  LoopbackLink<> link;
  FakeClock clk;
  WireDisplay d(link, clk);
  g_slowClock = &clk;

  d.onKey(&stallingKey, nullptr);
  bringUpSync(d, link, clk);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, d.callbackOverruns(), "nothing has overrun yet");

  // A key arrives and the application sits in its KeyCb for well over the budget.
  g_stallMs = AFFA_CALLBACK_BUDGET_MS * 4;
  link.inject(affatest::mk(0x1C1, {0x03, 0x89, 0x00, 0x05, 0xA3, 0xA3, 0xA3, 0xA3}));
  d.poll();

  TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, d.callbackOverruns(), "the overrun is counted");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(static_cast<uint8_t>(CbKind::Key),
                                  static_cast<uint8_t>(d.slowestCallback()),
                                  "and ATTRIBUTED — this is the whole point");
  TEST_ASSERT_TRUE(d.slowestCallbackMs() >= g_stallMs);
  TEST_ASSERT_EQUAL_STRING("KeyCb", cbName(d.slowestCallback()));

  // THE TIMESTAMP IS NOT DECORATION. A peak at t=0 is WiFi associating and is nothing to
  // fix; a peak whose timestamp keeps moving is a callback that blocks every time. Without
  // it the two are the same number.
  TEST_ASSERT_TRUE_MESSAGE(d.slowestCallbackAtMs() > 0, "when it happened is recorded too");

  g_slowClock = nullptr;
  g_stallMs   = 0;
}

void test_the_worst_callback_wins_not_the_most_recent(void) {
  // A 400 ms callback that fired once at boot is the interesting one. Keeping only the most
  // recent would let a run of merely-slow ones bury it before anybody looked.
  LoopbackLink<> link;
  FakeClock clk;
  WireDisplay d(link, clk);
  g_slowClock = &clk;

  d.onFrame(&stallingTap, nullptr);
  bringUpSync(d, link, clk);

  g_stallMs = AFFA_CALLBACK_BUDGET_MS * 10;          // the bad one, first
  link.inject(affatest::mk(0x1C1, {0x70, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3}));
  d.poll();
  const uint32_t worst = d.slowestCallbackMs();
  TEST_ASSERT_TRUE(worst >= g_stallMs);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CbKind::FrameTap),
                          static_cast<uint8_t>(d.slowestCallback()));

  g_stallMs = AFFA_CALLBACK_BUDGET_MS + 1;           // several merely-slow ones after
  for (int i = 0; i < 5; ++i) {
    link.inject(affatest::mk(0x1C1, {0x70, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3}));
    d.poll();
  }
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(worst, d.slowestCallbackMs(),
                                   "the peak survives a run of smaller ones");

  // …and it is cleared deliberately, never by being read.
  d.resetCallbackPeak();
  TEST_ASSERT_EQUAL_UINT32(0, d.slowestCallbackMs());
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CbKind::None),
                          static_cast<uint8_t>(d.slowestCallback()));
  TEST_ASSERT_TRUE_MESSAGE(d.callbackOverruns() > 0,
                           "the running count is NOT reset with the peak");

  g_slowClock = nullptr;
  g_stallMs   = 0;
}

void test_a_callback_inside_the_budget_is_not_reported(void) {
  LoopbackLink<> link;
  FakeClock clk;
  WireDisplay d(link, clk);
  g_slowClock = &clk;

  d.onKey(&stallingKey, nullptr);
  bringUpSync(d, link, clk);

  g_stallMs = AFFA_CALLBACK_BUDGET_MS - 1;           // just inside
  link.inject(affatest::mk(0x1C1, {0x03, 0x89, 0x00, 0x05, 0xA3, 0xA3, 0xA3, 0xA3}));
  d.poll();
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, d.callbackOverruns(), "the budget is a threshold");
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CbKind::None),
                          static_cast<uint8_t>(d.slowestCallback()));

  g_slowClock = nullptr;
  g_stallMs   = 0;
}
#endif  // AFFA_CALLBACK_BUDGET_MS > 0

void test_begun_reports_whether_begin_has_run() {
  LoopbackLink<> link;
  FakeClock clk;
  WireDisplay d(link, clk);
  // AffaTask::start() refuses on false. A task polling a display that was never begun
  // transmits nothing and reports no reason — the failure the old #error existed to stop.
  TEST_ASSERT_FALSE(d.begun());
  d.begin();
  TEST_ASSERT_TRUE(d.begun());
}

}  // namespace

void setUp(void) {}
void tearDown(void) {}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_render_into_a_dead_link_is_held_not_refused);
  RUN_TEST(test_a_permanently_bad_render_is_still_refused_at_the_call);
  RUN_TEST(test_a_timeout_is_retried_before_the_application_hears_about_it);
  RUN_TEST(test_retries_are_bounded_and_the_last_failure_is_reported);
  RUN_TEST(test_a_rejection_by_the_panel_is_never_retried);
  RUN_TEST(test_a_flapping_link_does_not_spend_the_retry_budget);
  RUN_TEST(test_a_link_that_never_returns_still_gives_up);
  RUN_TEST(test_a_held_job_survives_a_resync_and_registers_itself_again);
  RUN_TEST(test_a_held_job_is_given_up_rather_than_waiting_for_ever);
  RUN_TEST(test_poll_from_a_foreign_task_does_nothing_and_is_counted);
  RUN_TEST(test_a_render_from_a_foreign_task_is_accepted_and_applied_on_the_owner);
  RUN_TEST(test_the_callers_own_mistakes_are_still_answered_synchronously);
  RUN_TEST(test_a_posted_render_the_queue_refuses_reports_through_onComplete);
  RUN_TEST(test_a_full_dispatch_ring_refuses_at_the_call_and_counts_it);
  RUN_TEST(test_a_call_on_the_owning_task_never_touches_the_ring);
  RUN_TEST(test_with_no_owner_registered_every_task_takes_the_direct_path);
#if AFFA_CALLBACK_BUDGET_MS > 0
  RUN_TEST(test_a_blocking_callback_is_named_not_merely_counted);
  RUN_TEST(test_the_worst_callback_wins_not_the_most_recent);
  RUN_TEST(test_a_callback_inside_the_budget_is_not_reported);
#endif
  RUN_TEST(test_begun_reports_whether_begin_has_run);
  return UNITY_END();
}
