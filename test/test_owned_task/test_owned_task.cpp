// The owned-task mode, tested where it is portable.
//
// There is no FreeRTOS on the host, so this suite does NOT test the task. It tests the two
// pieces the task is assembled from, which is where the bugs would be:
//
//   * Command + applyCommand — the dispatch table and the by-value argument copy. A
//     pointer into a caller's stack that crossed a task boundary would be a use-after-free
//     that only ever shows up under load on the target.
//   * RequestTable — the TxTicket -> TxRequest mapping. A leaked slot makes a fixed table
//     fail slowly, which is the worst way for a table to fail.
//
// Plus the one part of the design that lives in core/ and is therefore testable here in
// full: poll() refusing a call from a task that is not the poll owner (CR §6.2).
//
// The key-latency acceptance criterion (CR §3) cannot be proved on the host — it is about
// task scheduling — and is asserted on hardware by examples/19_owned_task.

#include <unity.h>
#include <cstring>

#include "AffaConfig.h"
#include "core/AffaDisplayBase.h"
#include "link/LoopbackLink.h"
#include "rtos/AffaCommand.h"
#include "../affa_test_support.h"

using namespace affa;
using namespace affa::rtos;
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

struct Call {
  char        name[24] = {0};
  char        s0[80]   = {0};
  char        s1[80]   = {0};
  char        s2[80]   = {0};
  int         a = -1, b = -1, c = -1;
};

class RecordingDisplay final : public AffaDisplayBase {
 public:
  RecordingDisplay(ICanLink& l, IClock& c) : AffaDisplayBase(l, c, kProfile, kFuncIds, 2) {}
  bool supports(Feature) const override { return true; }

  Call     calls[8];
  uint8_t  n = 0;
  uint8_t  begins = 0;

  Result setText(const char* t, uint8_t d) override { return rec("setText", t, nullptr, nullptr, d); }
  Result setTime(const char* t) override            { return rec("setTime", t); }
  Result setPower(bool on) override                 { return rec("setPower", nullptr, nullptr, nullptr, on); }
  Result showMenu(const char* h, const char* r0, const char* r1, uint8_t s) override {
    return rec("showMenu", h, r0, r1, s);
  }
  Result highlightItem(uint8_t row) override        { return rec("highlight", nullptr, nullptr, nullptr, row); }
  Result showPopupText(const char* t, uint8_t i, uint8_t s, uint8_t f) override {
    return rec("popup", t, nullptr, nullptr, i, s, f);
  }
  Result hidePopup() override                       { return rec("hidePopup"); }
  Result showFullscreenText(const char* a, const char* b, const char* c) override {
    return rec("fullscreen", a, b, c);
  }
  Result hideFullscreenText() override              { return rec("hideFullscreen"); }
  Result showConfirmBox(const char* a, const char* b, const char* c) override {
    return rec("confirm", a, b, c);
  }
  Result showInfoPopup(const char* a, const char* b, const char* c) override {
    return rec("info", a, b, c);
  }
  Result hideInfoPopup() override                   { return rec("hideInfo"); }

  bool begin() override { ++begins; return AffaDisplayBase::begin(); }

 protected:
  uint8_t  packetFiller() const override { return 0x00; }
  uint16_t keyTxId()      const override { return 0x1C1; }

 private:
  Result rec(const char* name, const char* s0 = nullptr, const char* s1 = nullptr,
             const char* s2 = nullptr, int a = -1, int b = -1, int c = -1) {
    if (n >= 8) return Result::QueueFull;
    Call& k = calls[n++];
    std::snprintf(k.name, sizeof(k.name), "%s", name);
    if (s0) std::snprintf(k.s0, sizeof(k.s0), "%s", s0);
    if (s1) std::snprintf(k.s1, sizeof(k.s1), "%s", s1);
    if (s2) std::snprintf(k.s2, sizeof(k.s2), "%s", s2);
    k.a = a; k.b = b; k.c = c;
    return Result::Ok;
  }
};

// A display whose setText actually enqueues, so the ticket path is real rather than mocked.
class WireDisplay final : public AffaDisplayBase {
 public:
  WireDisplay(ICanLink& l, IClock& c) : AffaDisplayBase(l, c, kProfile, kFuncIds, 2) {}
  bool supports(Feature) const override { return true; }

  // ONE FRAME, deliberately: with LoopbackLink's auto-ACK answering DONE to everything, a
  // multi-frame message completes at frame 0 and its continuations never reach the wire.
  // Eight payload bytes keeps the whole message visible to an ordering assertion.
  Result setText(const char* t, uint8_t) override {
    uint8_t d[8] = {0x05, 0x77, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00};
    for (uint8_t i = 0; i < 4 && t && t[i]; ++i) d[2 + i] = static_cast<uint8_t>(t[i]);
    TxOptions o;
    o.slot     = RenderSlot::None;   // no coalescing: this suite is about ORDER
    o.coalesce = false;
    return (enqueue(0x151, d, sizeof(d), o) == kNoTicket) ? lastResult() : Result::Ok;
  }

 protected:
  uint8_t  packetFiller() const override { return 0x00; }
  uint16_t keyTxId()      const override { return 0x1C1; }
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

Command cmd(Op op) { Command c; c.op = op; return c; }

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

void test_apply_dispatches_every_op_with_its_arguments() {
  LoopbackLink<> link;
  FakeClock clk;
  RecordingDisplay d(link, clk);
  d.begin();

  TxTicket t = 1;

  Command a = cmd(Op::SetText);  setArg(a.s0, "HELLO"); a.a = 3;
  ASSERT_RESULT(Ok, applyCommand(d, a, t));

  Command b = cmd(Op::ShowMenu);
  setArg(b.s0, "HDR"); setArg(b.s1, "R0"); setArg(b.s2, "R1"); b.a = 0x0C;
  ASSERT_RESULT(Ok, applyCommand(d, b, t));

  Command p = cmd(Op::ShowPopupText); setArg(p.s0, "VOL 28");
  p.a = 0x09; p.b = 0xFF; p.c = 0x60;
  ASSERT_RESULT(Ok, applyCommand(d, p, t));

  Command f = cmd(Op::ShowFullscreenText);
  setArg(f.s0, "ONE"); setArg(f.s1, "TWO"); setArg(f.s2, "THREE");
  ASSERT_RESULT(Ok, applyCommand(d, f, t));

  Command h = cmd(Op::HidePopup);
  ASSERT_RESULT(Ok, applyCommand(d, h, t));

  TEST_ASSERT_EQUAL_UINT8(5, d.n);

  TEST_ASSERT_EQUAL_STRING("setText", d.calls[0].name);
  TEST_ASSERT_EQUAL_STRING("HELLO",   d.calls[0].s0);
  TEST_ASSERT_EQUAL_INT(3,            d.calls[0].a);

  TEST_ASSERT_EQUAL_STRING("showMenu", d.calls[1].name);
  TEST_ASSERT_EQUAL_STRING("HDR", d.calls[1].s0);
  TEST_ASSERT_EQUAL_STRING("R0",  d.calls[1].s1);
  TEST_ASSERT_EQUAL_STRING("R1",  d.calls[1].s2);
  TEST_ASSERT_EQUAL_INT(0x0C,     d.calls[1].a);

  TEST_ASSERT_EQUAL_STRING("popup",  d.calls[2].name);
  TEST_ASSERT_EQUAL_STRING("VOL 28", d.calls[2].s0);
  TEST_ASSERT_EQUAL_INT(0x09, d.calls[2].a);
  TEST_ASSERT_EQUAL_INT(0xFF, d.calls[2].b);
  TEST_ASSERT_EQUAL_INT(0x60, d.calls[2].c);

  TEST_ASSERT_EQUAL_STRING("fullscreen", d.calls[3].name);
  TEST_ASSERT_EQUAL_STRING("THREE",      d.calls[3].s2);

  TEST_ASSERT_EQUAL_STRING("hidePopup", d.calls[4].name);
}

void test_a_key_command_fires_the_callback_and_never_a_ticket() {
  LoopbackLink<> link;
  FakeClock clk;
  RecordingDisplay d(link, clk);
  d.begin();

  static int fired = 0;
  static Key seen = Key::Load;
  static KeyEdge seenEdge = KeyEdge::Click;
  fired = 0;
  d.onKey([](Key k, KeyEdge e, void*) { ++fired; seen = k; seenEdge = e; }, nullptr);

  Command c = cmd(Op::PressKey);
  c.a = 0x01; c.b = 0x41;                 // RollDown = 0x0141
  c.c = 0;                                 // click
  c.d = static_cast<uint8_t>(KeySource::Local);

  TxTicket t = 0xBEEF;
  ASSERT_RESULT(Ok, applyCommand(d, c, t));

  // KeyCb fires SYNCHRONOUSLY, inside the call, on this task. That is the whole acceptance
  // criterion of the owned-task design, stated at the only layer the host can see it.
  TEST_ASSERT_EQUAL_INT(1, fired);
  TEST_ASSERT_EQUAL_HEX16(0x0141, static_cast<uint16_t>(seen));
  TEST_ASSERT_TRUE(seenEdge == KeyEdge::Click);
  TEST_ASSERT_EQUAL_UINT16(kNoTicket, t);   // a key enqueues nothing, ever
}

void test_arguments_are_copied_by_value_and_truncated() {
  Command c = cmd(Op::SetText);

  char scratch[AFFA_TASK_ARG_MAX * 2];
  std::memset(scratch, 'A', sizeof(scratch));
  scratch[sizeof(scratch) - 1] = '\0';

  setArg(c.s0, scratch);
  // Bounded and NUL-terminated: a queued command outlives its caller's stack frame, so a
  // copy that ran off the end here would be a use-after-free on the target only.
  TEST_ASSERT_EQUAL_UINT32(AFFA_TASK_ARG_MAX - 1, std::strlen(c.s0));

  std::memset(scratch, 'B', sizeof(scratch));   // the source is gone; the copy is not
  TEST_ASSERT_EQUAL_CHAR('A', c.s0[0]);
  TEST_ASSERT_EQUAL_CHAR('A', c.s0[AFFA_TASK_ARG_MAX - 2]);

  setArg(c.s1, nullptr);
  TEST_ASSERT_EQUAL_STRING("", c.s1);
}

// ---------------------------------------------------------------------------
// Tickets
// ---------------------------------------------------------------------------

void test_apply_reports_the_ticket_the_enqueue_issued() {
  LoopbackLink<> link;
  FakeClock clk;
  WireDisplay d(link, clk);
  bringUpSync(d, link, clk);

  Command c = cmd(Op::SetText); setArg(c.s0, "ONE");
  TxTicket t = kNoTicket;
  ASSERT_RESULT(Ok, applyCommand(d, c, t));

  TEST_ASSERT_NOT_EQUAL(kNoTicket, t);
  TEST_ASSERT_EQUAL_UINT16(d.lastEnqueued(), t);
}

void test_a_render_into_a_dead_link_is_held_not_refused() {
  LoopbackLink<> link;
  FakeClock clk;
  WireDisplay d(link, clk);
  d.begin();                                  // begun, but never synced

  // The owned task posts this from an application thread that has no idea whether the panel
  // is awake, and it must not have to care: the command is accepted, the caller gets a
  // handle, and the library holds the job until the link is usable.
  Command c = cmd(Op::SetText); setArg(c.s0, "ONE");
  TxTicket t = kNoTicket;
  ASSERT_RESULT(Ok, applyCommand(d, c, t));
  TEST_ASSERT_NOT_EQUAL(kNoTicket, t);

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
  TEST_ASSERT_EQUAL_UINT16(kNoTicket, d.enqueue(0x151, nullptr, 4));
  ASSERT_RESULT(BadArgument, d.lastResult());

  uint8_t big[AFFA_MAX_PAYLOAD];
  std::memset(big, 0x5A, sizeof(big));
  TEST_ASSERT_EQUAL_UINT16(kNoTicket, d.enqueue(0x151, big, AFFA_MAX_PAYLOAD + 1));
  ASSERT_RESULT(TooLong, d.lastResult());

  uint8_t one = 0x11;
  TEST_ASSERT_EQUAL_UINT16(kNoTicket, d.enqueue(0x999, &one, 1));
  ASSERT_RESULT(UnknownFunc, d.lastResult());
}

void test_commands_drain_in_submission_order() {
  LoopbackLink<> link;
  FakeClock clk;
  WireDisplay d(link, clk);
  bringUpSync(d, link, clk);
  link.setAutoAck(true);

  // Get the lazy registration burst out of the way first, so what is left on the wire is
  // exactly the payloads this test is about.
  ASSERT_RESULT(Ok, d.setText("WARM", 255));
  affatest::pumpUntilIdle(d);
  affatest::drain(link);
  TEST_ASSERT_TRUE(d.registered());

  TxTicket t[3] = {0, 0, 0};
  const char* words[3] = {"AAAA", "BBBB", "CCCC"};
  for (uint8_t i = 0; i < 3; ++i) {
    Command c = cmd(Op::SetText); setArg(c.s0, words[i]);
    ASSERT_RESULT(Ok, applyCommand(d, c, t[i]));
  }

  // Tickets are issued in submission order and the transmit queue holds all three: the
  // drain loop does not reorder, coalesce or lose anything on its way to enqueue().
  TEST_ASSERT_TRUE(t[0] < t[1] && t[1] < t[2]);
  TEST_ASSERT_TRUE(d.busy());
  TEST_ASSERT_EQUAL_UINT8(2, d.queued());     // two behind the active one

  affatest::pumpUntilIdle(d);

  Frame f;
  int found = 0;
  while (link.takeSent(f)) {
    if (f.id != 0x151 || f.data[0] != 0x05 || f.data[1] != 0x77) continue;
    TEST_ASSERT_EQUAL_CHAR(words[found][0], static_cast<char>(f.data[2]));
    ++found;
  }
  TEST_ASSERT_EQUAL_INT(3, found);
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
// RequestTable
// ---------------------------------------------------------------------------

void test_request_table_round_trip_frees_the_slot() {
  RequestTable<4> m;
  TEST_ASSERT_TRUE(m.bind(10, 100));
  TEST_ASSERT_TRUE(m.bind(11, 101));
  TEST_ASSERT_EQUAL_UINT8(2, m.used());

  TEST_ASSERT_EQUAL_UINT16(100, m.take(10));
  TEST_ASSERT_EQUAL_UINT8(1, m.used());

  // Taken once, and only once: a completion arrives exactly once per ticket, and a table
  // that answered twice would report a stale request against a later, unrelated ticket.
  TEST_ASSERT_EQUAL_UINT16(kNoRequest, m.take(10));
  TEST_ASSERT_EQUAL_UINT16(101, m.take(11));
  TEST_ASSERT_EQUAL_UINT8(0, m.used());
}

void test_request_table_reports_unknown_tickets_as_no_request() {
  RequestTable<4> m;
  // A render the LIBRARY made — a menu redraw, the close banner — completes with a ticket
  // nobody bound. It must be reported as kNoRequest, not as somebody else's request.
  TEST_ASSERT_EQUAL_UINT16(kNoRequest, m.take(42));
  TEST_ASSERT_FALSE(m.bind(kNoTicket, 1));
  TEST_ASSERT_FALSE(m.bind(1, kNoRequest));
}

void test_request_table_full_refuses_rather_than_evicting() {
  RequestTable<2> m;
  TEST_ASSERT_TRUE(m.bind(1, 11));
  TEST_ASSERT_TRUE(m.bind(2, 12));
  TEST_ASSERT_FALSE(m.bind(3, 13));

  // The two live entries survive: the refusal costs ticket 3 its request handle, not the
  // two callers who were already waiting.
  TEST_ASSERT_EQUAL_UINT16(11, m.take(1));
  TEST_ASSERT_EQUAL_UINT16(12, m.take(2));
}

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

void test_begun_reports_whether_begin_has_run() {
  LoopbackLink<> link;
  FakeClock clk;
  RecordingDisplay d(link, clk);
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
  RUN_TEST(test_apply_dispatches_every_op_with_its_arguments);
  RUN_TEST(test_a_key_command_fires_the_callback_and_never_a_ticket);
  RUN_TEST(test_arguments_are_copied_by_value_and_truncated);
  RUN_TEST(test_apply_reports_the_ticket_the_enqueue_issued);
  RUN_TEST(test_a_render_into_a_dead_link_is_held_not_refused);
  RUN_TEST(test_a_permanently_bad_render_is_still_refused_at_the_call);
  RUN_TEST(test_commands_drain_in_submission_order);
  RUN_TEST(test_a_timeout_is_retried_before_the_application_hears_about_it);
  RUN_TEST(test_retries_are_bounded_and_the_last_failure_is_reported);
  RUN_TEST(test_a_rejection_by_the_panel_is_never_retried);
  RUN_TEST(test_a_held_job_survives_a_resync_and_registers_itself_again);
  RUN_TEST(test_a_held_job_is_given_up_rather_than_waiting_for_ever);
  RUN_TEST(test_request_table_round_trip_frees_the_slot);
  RUN_TEST(test_request_table_reports_unknown_tickets_as_no_request);
  RUN_TEST(test_request_table_full_refuses_rather_than_evicting);
  RUN_TEST(test_poll_from_a_foreign_task_does_nothing_and_is_counted);
  RUN_TEST(test_begun_reports_whether_begin_has_run);
  return UNITY_END();
}
