// The task boundary and the long-payload pool.
//
// "Render from any task" is the 2.0 contract: a call off the poll owner's task mints its
// ticket on the spot, copies the finished bytes into the dispatch ring, and the owner admits
// them inside its next poll(). Nothing else may touch the queue, and a long payload must not
// lend the caller's memory to that other task. These tests pin both halves with a fake task
// identity, so the boundary is exercised exactly as AffaTask exercises it on hardware.

#include "../affa_test_support.h"

#include "carminat/CarminatDisplay.h"
#include "carminat/CarminatConstants.h"

#include <atomic>
#include <cstring>
#include <thread>

using namespace affa;
using affatest::drain;
using affatest::pumpUntilIdle;

namespace {

// The calling task's identity, as AffaTask's TaskIdFn would report it. Thread-local, so the
// stress test's real threads each are the task they claim to be.
thread_local void* g_task = nullptr;
void* currentTask() { return g_task; }
void* const kOwner   = reinterpret_cast<void*>(0x1);
void* const kForeign = reinterpret_cast<void*>(0x2);

struct Completions {
  TxTicket ticket[32];
  Result   result[32];
  uint8_t  n = 0;
  int count(TxTicket t) const {
    int c = 0;
    for (uint8_t i = 0; i < n; ++i) c += (ticket[i] == t);
    return c;
  }
  Result resultOf(TxTicket t) const {
    for (uint8_t i = 0; i < n; ++i) if (ticket[i] == t) return result[i];
    return Result::Ok;
  }
};

void onDone(TxTicket t, Result r, void* ctx) {
  auto* c = static_cast<Completions*>(ctx);
  if (c->n < 32) { c->ticket[c->n] = t; c->result[c->n] = r; ++c->n; }
}

struct Rig {
  LoopbackLink<512> link;
  affatest::FakeClock clk;
  CarminatDisplay d;
  Completions done;

  Rig() : d(link, clk) {}

  // The opening on the owner's task, registered and drained, with the owner claimed.
  void up() {
    g_task = kOwner;
    d.onComplete(&onDone, &done);
    d.begin();
    d.setSelfAck(true);
    affatest::completeCarminatAuth(d, link, clk);
    ASSERT_RESULT(Ok, d.setPower(true));
    affatest::settleCarminatRegistration(d, clk);
    pumpUntilIdle(d);
    TEST_ASSERT_TRUE(d.registered());
    drain(link);
    d.setPollOwner(kOwner, &currentTask);
    done.n = 0;
  }

  void ownerPumps() {
    g_task = kOwner;
    pumpUntilIdle(d);
  }
};

// Reassembles one ISO-TP message on `id` from what the link sent: FF data[2..7], then CF
// data[1..7], up to the length the FF declares. Returns the payload length.
uint16_t reassemble(LoopbackLink<512>& link, uint32_t id, uint8_t* out, uint16_t cap) {
  Frame f;
  uint16_t want = 0, got = 0;
  bool first = true;
  while (link.takeSent(f)) {
    if (f.id != id) continue;
    if (first) {
      want  = static_cast<uint16_t>(((f.data[0] & 0x0F) << 8) | f.data[1]);
      for (uint8_t i = 2; i < 8 && got < want && got < cap; ++i) out[got++] = f.data[i];
      first = false;
      continue;
    }
    for (uint8_t i = 1; i < 8 && got < want && got < cap; ++i) out[got++] = f.data[i];
    if (got >= want) break;
  }
  return got;
}

void pattern(uint8_t* b, uint16_t n, uint8_t seed) {
  for (uint16_t i = 0; i < n; ++i) b[i] = static_cast<uint8_t>(seed + i * 7);
}

}  // namespace

// ---------------------------------------------------------------------------
// The boundary
// ---------------------------------------------------------------------------

void test_a_render_from_another_task_is_posted_and_drained_by_the_owner(void) {
  Rig r;
  r.up();

  g_task = kForeign;
  const Submitted s = r.d.setText("HELLO");
  TEST_ASSERT_TRUE_MESSAGE(s.ok(), "the foreign caller gets its ticket at once");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, r.d.dispatchQueued(), "posted, not admitted");
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, r.link.sentCount(), "and nothing on the wire yet");

  r.d.poll();                                   // still the foreign task
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, r.d.foreignPolls(), "a foreign poll is refused");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, r.d.dispatchQueued(), "and drains nothing");

  r.ownerPumps();
  TEST_ASSERT_EQUAL_UINT8(0, r.d.dispatchQueued());
  TEST_ASSERT_TRUE_MESSAGE(r.link.sentCount() > 0, "the owner put it on the wire");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, r.done.count(s.ticket),
                                "one verdict, under the ticket the foreign caller holds");
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Result::Ok),
                          static_cast<uint8_t>(r.done.resultOf(s.ticket)));
}

void test_a_full_dispatch_ring_refuses_QueueFull_at_the_call(void) {
  Rig r;
  r.up();
  g_task = kForeign;
  char text[8];
  for (uint8_t i = 0; i < AFFA_DISPATCH_DEPTH; ++i) {
    std::snprintf(text, sizeof(text), "T%u", static_cast<unsigned>(i));
    TEST_ASSERT_TRUE(r.d.setText(text).ok());
  }
  const Submitted over = r.d.setText("OVER");
  TEST_ASSERT_FALSE_MESSAGE(over.ok(), "one past the ring is refused");
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Result::QueueFull),
                          static_cast<uint8_t>(over.result));
  TEST_ASSERT_EQUAL_UINT32(1, r.d.dispatchDropped());
}

// THE NON-RENDER CALLS CROSS TOO. pressKey, the aborts and a re-begin() rewrite state poll()
// reads; off the owner they used to run right there, on the caller's task. Each is posted,
// and its effect — KeyCb included — happens on the owner.
void* g_keyTask = nullptr;
int   g_keys    = 0;
void onKeyCb(Key, KeyEdge, void*) { g_keyTask = g_task; ++g_keys; }

void test_pressKey_from_another_task_runs_its_KeyCb_on_the_owner(void) {
  Rig r;
  r.d.onKey(&onKeyCb, nullptr);
  r.up();
  g_keys = 0;
  g_task = kForeign;
  const Submitted s = r.d.pressKey(Key::Pause, KeyEdge::Click);
  TEST_ASSERT_TRUE_MESSAGE(s.ok(), "posted: a ticket, like a render");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_keys, "posted, not delivered on the caller's task");
  g_task = kOwner;
  r.d.poll();
  TEST_ASSERT_EQUAL_INT(1, g_keys);
  TEST_ASSERT_TRUE_MESSAGE(g_keyTask == kOwner, "KeyCb ran on the owner");
  TEST_ASSERT_EQUAL_INT(1, r.done.count(s.ticket));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Result::Ok),
                          static_cast<uint8_t>(r.done.resultOf(s.ticket)));
}

// The posted call can only say "posted"; the owner's refusal is that ticket's one completion.
void test_a_refused_posted_pressKey_completes_with_its_verdict(void) {
  Rig r;
  r.up();
  r.link.setLive(false);                        // the Wire half will be LinkDown
  g_task = kForeign;
  const Submitted s = r.d.pressKey(Key::Pause, KeyEdge::Click, KeySource::Wire);
  TEST_ASSERT_TRUE(s.ok());
  g_task = kOwner;
  r.d.poll();
  TEST_ASSERT_EQUAL_INT(1, r.done.count(s.ticket));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Result::LinkDown),
                          static_cast<uint8_t>(r.done.resultOf(s.ticket)));
}

// On the owner the verdict is immediate: a refusal is the result and burns no completion.
void test_a_pressKey_on_the_owner_answers_at_once(void) {
  Rig r;
  r.up();
  r.link.setLive(false);
  g_task = kOwner;
  const Submitted s = r.d.pressKey(Key::Pause, KeyEdge::Click, KeySource::Wire);
  TEST_ASSERT_FALSE(s.ok());
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Result::LinkDown),
                          static_cast<uint8_t>(s.result));
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, r.done.n, "a refused call never completes");
}

void test_abortPending_from_another_task_is_applied_by_the_owner(void) {
  Rig r;
  r.up();
  g_task = kOwner;
  const Submitted a = r.d.setText("ONE");       // owner-queued, not yet started
  ASSERT_RESULT(Ok, r.d.setTime("1234"));
  g_task = kForeign;
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, r.d.abortPending(), "posted: nothing dropped here");
  TEST_ASSERT_TRUE(r.d.pending(RenderSlot::Text));
  g_task = kOwner;
  r.d.poll();
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Result::Aborted),
                          static_cast<uint8_t>(r.done.resultOf(a.ticket)));
}

void test_begin_from_another_task_resets_on_the_owner(void) {
  Rig r;
  r.up();
  TEST_ASSERT_TRUE(r.d.registered());
  g_task = kForeign;
  TEST_ASSERT_TRUE_MESSAGE(r.d.begin(), "the reset was posted");
  TEST_ASSERT_TRUE_MESSAGE(r.d.registered(), "and nothing changed on the caller's task");
  g_task = kOwner;
  r.d.poll();
  TEST_ASSERT_FALSE_MESSAGE(r.d.registered(), "the owner ran it: the session starts over");
}

// ---------------------------------------------------------------------------
// The long-payload pool
// ---------------------------------------------------------------------------

#if AFFA_ENABLE_NAV
void test_the_nav_image_is_copied_so_the_caller_may_reuse_it(void) {
  Rig r;
  r.up();
  uint8_t img[carminat::kNavBitmapBytes];
  pattern(img, sizeof(img), 0x10);
  uint8_t want[sizeof(img)];
  std::memcpy(want, img, sizeof(img));

  ASSERT_RESULT(Ok, r.d.showNavBitmap(img));
  std::memset(img, 0xFF, sizeof(img));          // the caller reuses its buffer at once
  r.ownerPumps();

  uint8_t got[400];
  const uint16_t n = reassemble(r.link, carminat::kIdNav, got, sizeof(got));
  TEST_ASSERT_EQUAL_UINT16(14 + carminat::kNavBitmapBytes, n);
  TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(want, got + 14, sizeof(want),
                                       "the wire carries the image as it was at the call");
}

void test_a_nav_image_from_another_task_crosses_as_a_copy(void) {
  Rig r;
  r.up();
  uint8_t img[carminat::kNavBitmapBytes];
  pattern(img, sizeof(img), 0x33);
  uint8_t want[sizeof(img)];
  std::memcpy(want, img, sizeof(img));

  g_task = kForeign;
  const Submitted s = r.d.showNavBitmap(img);
  TEST_ASSERT_TRUE(s.ok());
  std::memset(img, 0x00, sizeof(img));          // a web handler's stack, gone
  r.ownerPumps();

  uint8_t got[400];
  const uint16_t n = reassemble(r.link, carminat::kIdNav, got, sizeof(got));
  TEST_ASSERT_EQUAL_UINT16(14 + carminat::kNavBitmapBytes, n);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(want, got + 14, sizeof(want));
  TEST_ASSERT_EQUAL_INT(1, r.done.count(s.ticket));
}

void test_the_pool_refuses_past_its_slots_and_gets_them_back(void) {
  Rig r;
  r.up();
  uint8_t img[carminat::kNavBitmapBytes] = {0};

  // Nothing is polled, so every accepted render still holds its slot.
  for (uint8_t i = 0; i < AFFA_BULK_SLOTS; ++i) ASSERT_RESULT(Ok, r.d.showNavBitmap(img));
  const Submitted over = r.d.showNavBitmap(img);
  TEST_ASSERT_FALSE_MESSAGE(over.ok(), "every slot is in flight");
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Result::QueueFull),
                          static_cast<uint8_t>(over.result));

  r.ownerPumps();                               // both complete; both slots come back
  ASSERT_RESULT(Ok, r.d.showNavBitmap(img));
}

void test_an_aborted_long_render_returns_its_slot(void) {
  Rig r;
  r.up();
  uint8_t img[carminat::kNavBitmapBytes] = {0};
  for (uint8_t i = 0; i < AFFA_BULK_SLOTS; ++i) ASSERT_RESULT(Ok, r.d.showNavBitmap(img));
  TEST_ASSERT_EQUAL_UINT8(AFFA_BULK_SLOTS, r.d.abortPending());
  ASSERT_RESULT(Ok, r.d.showNavBitmap(img));    // the drop released them
}
#endif  // AFFA_ENABLE_NAV

#if AFFA_ENABLE_BIGMENU
void test_showMenuN_needs_no_buffer_from_the_caller(void) {
  Rig r;
  r.up();
  const char* items[] = {"ONE", "TWO", "THREE", "FOUR"};
  ASSERT_RESULT(Ok, r.d.showMenuN("LIST", items, 4));
  r.ownerPumps();
  uint8_t got[400];
  const uint16_t n = reassemble(r.link, carminat::kIdSetText, got, sizeof(got));
  TEST_ASSERT_EQUAL_UINT16(CarminatDisplay::menuScreenBytes(4) - 2, n);
  TEST_ASSERT_EQUAL_HEX8(0x84, got[6]);          // 0x80 | count
  TEST_ASSERT_EQUAL_STRING_LEN("THREE", reinterpret_cast<const char*>(got + 36 + 27 * 2 + 1), 5);
}
#endif

// ---------------------------------------------------------------------------
// Real threads
// ---------------------------------------------------------------------------

// THREE PRODUCERS AND THE OWNER, ON REAL THREADS. Every accepted render must reach exactly
// one verdict — Ok, or Aborted when a newer text superseded it — and nothing may be lost or
// doubled on the way through the ring. A producer that is refused (ring full) retries.
void test_concurrent_producers_each_ticket_gets_exactly_one_verdict(void) {
  static Rig r;                                 // big: keep it off the stack
  r.up();

  constexpr int kProducers = 3, kEach = 150;
  static TxTicket issued[kProducers][kEach];
  static std::atomic<int> seen[0x10000];
  for (auto& s : seen) s.store(0);
  r.d.onComplete([](TxTicket t, Result, void*) { seen[t].fetch_add(1); }, nullptr);

  std::atomic<bool> stop{false};
  std::thread owner([&] {
    g_task = kOwner;
    while (!stop.load()) r.d.poll();
    while (r.d.busy()) r.d.poll();
  });
  std::thread prod[kProducers];
  for (int p = 0; p < kProducers; ++p) {
    prod[p] = std::thread([&, p] {
      g_task = reinterpret_cast<void*>(static_cast<uintptr_t>(0x100 + p));
      char text[8];
      for (int i = 0; i < kEach; ++i) {
        std::snprintf(text, sizeof(text), "P%dN%d", p, i);
        Submitted s;
        do { s = r.d.setText(text); if (!s) std::this_thread::yield(); } while (!s);
        issued[p][i] = s.ticket;
      }
    });
  }
  for (auto& t : prod) t.join();
  stop.store(true);
  owner.join();

  for (int p = 0; p < kProducers; ++p)
    for (int i = 0; i < kEach; ++i)
      TEST_ASSERT_EQUAL_INT_MESSAGE(1, seen[issued[p][i]].load(),
                                    "every accepted render gets exactly one verdict");
}

void setUp(void) { g_task = nullptr; }
void tearDown(void) {}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_render_from_another_task_is_posted_and_drained_by_the_owner);
  RUN_TEST(test_a_full_dispatch_ring_refuses_QueueFull_at_the_call);
  RUN_TEST(test_pressKey_from_another_task_runs_its_KeyCb_on_the_owner);
  RUN_TEST(test_a_refused_posted_pressKey_completes_with_its_verdict);
  RUN_TEST(test_a_pressKey_on_the_owner_answers_at_once);
  RUN_TEST(test_abortPending_from_another_task_is_applied_by_the_owner);
  RUN_TEST(test_begin_from_another_task_resets_on_the_owner);
#if AFFA_ENABLE_NAV
  RUN_TEST(test_the_nav_image_is_copied_so_the_caller_may_reuse_it);
  RUN_TEST(test_a_nav_image_from_another_task_crosses_as_a_copy);
  RUN_TEST(test_the_pool_refuses_past_its_slots_and_gets_them_back);
  RUN_TEST(test_an_aborted_long_render_returns_its_slot);
#endif
#if AFFA_ENABLE_BIGMENU
  RUN_TEST(test_showMenuN_needs_no_buffer_from_the_caller);
#endif
  RUN_TEST(test_concurrent_producers_each_ticket_gets_exactly_one_verdict);
  return UNITY_END();
}
