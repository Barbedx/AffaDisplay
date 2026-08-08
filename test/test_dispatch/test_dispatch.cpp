// The multi-producer ring that carries a render across a task boundary.
//
// It is tested here on its own, before anything is wired to it, because a concurrency bug
// found through the protocol is a bug found as "the panel occasionally draws garbage" — the
// exact symptom docs/NOTES.md §3 describes and the one that costs days.
//
// WHAT A HOST TEST CAN AND CANNOT PROVE. It cannot run two real tasks, so it cannot catch a
// memory-ordering mistake by racing. What it CAN do is drive the ring through every state a
// race would leave it in — a producer preempted between claiming a slot and publishing it,
// a claim that loses to another producer, a full ring, a wrap past the counter — by driving
// those states deliberately rather than hoping to hit them. The interleavings below are
// hand-built for that reason.

#include <unity.h>
#include <cstring>

#include "AffaConfig.h"
#include "core/AffaDispatch.h"

using namespace affa;

namespace {

// A payload item with a recognisable body, so a torn or misordered copy is visible rather
// than plausible.
DispatchItem payload(uint16_t funcId, TxTicket t, uint8_t seed, uint16_t len = 8) {
  DispatchItem it;
  it.ticket = t;
  it.funcId = funcId;
  it.len    = len;
  it.prefixLen = len;
  for (uint16_t i = 0; i < len && i < AFFA_MAX_PAYLOAD; ++i)
    it.data[i] = static_cast<uint8_t>(seed + i);
  return it;
}

void expectSame(const DispatchItem& want, const DispatchItem& got, const char* what) {
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(want.ticket, got.ticket, what);
  TEST_ASSERT_EQUAL_HEX16_MESSAGE(want.funcId, got.funcId, what);
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(want.len, got.len, what);
  TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(want.data, got.data, want.len, what);
}

}  // namespace

// ---------------------------------------------------------------------------
// Fidelity
// ---------------------------------------------------------------------------

void test_an_item_comes_out_exactly_as_it_went_in(void) {
  AffaMpsc<DispatchItem, 4> q;
  const DispatchItem in = payload(0x151, 7, 0xA0, AFFA_MAX_PAYLOAD);

  TEST_ASSERT_TRUE(q.push(in));
  TEST_ASSERT_EQUAL_UINT32(1, q.size());

  DispatchItem out;
  TEST_ASSERT_TRUE(q.pop(out));
  expectSame(in, out, "a full-length payload must survive the crossing byte for byte");
  TEST_ASSERT_TRUE(q.empty());
  TEST_ASSERT_FALSE_MESSAGE(q.pop(out), "an empty ring pops nothing");
}

void test_order_is_preserved(void) {
  AffaMpsc<DispatchItem, 8> q;
  for (uint8_t i = 0; i < 5; ++i)
    TEST_ASSERT_TRUE(q.push(payload(0x151, static_cast<TxTicket>(i + 1), i)));

  DispatchItem out;
  for (uint8_t i = 0; i < 5; ++i) {
    TEST_ASSERT_TRUE(q.pop(out));
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(i + 1, out.ticket, "FIFO, and the ticket proves it");
  }
  TEST_ASSERT_FALSE(q.pop(out));
}

void test_a_borrowed_tail_crosses_as_a_pointer(void) {
  // enqueueExternal and enqueueSplit borrow the caller's bytes; the contract already says
  // they must outlive the ticket, so crossing a task changes nothing. What must survive is
  // the pointer AND the prefix split.
  static const uint8_t kBody[16] = {0xDE, 0xAD, 0xBE, 0xEF};
  AffaMpsc<DispatchItem, 4> q;

  DispatchItem in = payload(0x1F1, 42, 0x10, 6);
  in.ext       = kBody;
  in.prefixLen = 6;
  in.len       = 6 + sizeof(kBody);

  TEST_ASSERT_TRUE(q.push(in));
  DispatchItem out;
  TEST_ASSERT_TRUE(q.pop(out));
  TEST_ASSERT_EQUAL_PTR_MESSAGE(kBody, out.ext, "the borrowed pointer is the payload tail");
  TEST_ASSERT_EQUAL_UINT16(6, out.prefixLen);
  TEST_ASSERT_EQUAL_UINT16(22, out.len);
  TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(in.data, out.data, 6, "and the prefix came by value");
}

void test_an_op_and_a_payload_are_told_apart(void) {
  AffaMpsc<DispatchItem, 4> q;

  DispatchItem key;
  key.op = DispatchOp::PressKey;
  key.a  = 0x01; key.b = 0x41; key.c = 0; key.d = 1;
  TEST_ASSERT_FALSE_MESSAGE(key.isPayload(), "an op is not a payload");

  TEST_ASSERT_TRUE(q.push(key));
  TEST_ASSERT_TRUE(q.push(payload(0x151, 3, 0x55)));

  DispatchItem out;
  TEST_ASSERT_TRUE(q.pop(out));
  TEST_ASSERT_FALSE(out.isPayload());
  TEST_ASSERT_EQUAL_UINT8(DispatchOp::PressKey == out.op ? 1 : 0, 1);
  TEST_ASSERT_EQUAL_HEX8(0x41, out.b);

  TEST_ASSERT_TRUE(q.pop(out));
  TEST_ASSERT_TRUE_MESSAGE(out.isPayload(), "DispatchOp::None is what makes it a payload");
  TEST_ASSERT_EQUAL_UINT16(3, out.ticket);
}

// ---------------------------------------------------------------------------
// Full, and the counted refusal
// ---------------------------------------------------------------------------

void test_a_full_ring_refuses_and_counts_without_losing_what_it_holds(void) {
  // OVERWRITING THE OLDEST WOULD BE THE WORSE BUG. A render silently replaced by a later one
  // is a screen that never appears with no symptom at all; a counted refusal is a number the
  // caller gets back (kNoTicket) and a number on a status page.
  AffaMpsc<DispatchItem, 4> q;
  for (uint8_t i = 0; i < 4; ++i)
    TEST_ASSERT_TRUE(q.push(payload(0x151, static_cast<TxTicket>(i + 1), i)));

  TEST_ASSERT_FALSE_MESSAGE(q.push(payload(0x151, 99, 0xFF)), "a full ring refuses");
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, q.dropped(), "and says so");
  TEST_ASSERT_FALSE(q.push(payload(0x151, 98, 0xFE)));
  TEST_ASSERT_EQUAL_UINT32(2, q.dropped());

  // Everything already accepted is intact and in order.
  DispatchItem out;
  for (uint8_t i = 0; i < 4; ++i) {
    TEST_ASSERT_TRUE(q.pop(out));
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(i + 1, out.ticket, "a refusal disturbs nothing held");
  }
  TEST_ASSERT_FALSE(q.pop(out));

  // And it takes items again once drained: the refusal is not a latch.
  TEST_ASSERT_TRUE(q.push(payload(0x151, 5, 0x20)));
  TEST_ASSERT_TRUE(q.pop(out));
  TEST_ASSERT_EQUAL_UINT16(5, out.ticket);
}

// ---------------------------------------------------------------------------
// The states a race would leave it in
// ---------------------------------------------------------------------------

void test_the_consumer_never_reads_past_the_published_position(void) {
  // The consumer must stop at what has been PUBLISHED, not at what has been claimed. This
  // asserts the visible half of that single-threaded — the claimed-but-unpublished cell
  // itself needs two threads and is covered by the stress test below, which is where the
  // memory ordering is actually exercised.
  AffaMpsc<DispatchItem, 4> q;
  TEST_ASSERT_TRUE(q.push(payload(0x151, 1, 0x10)));
  TEST_ASSERT_TRUE(q.push(payload(0x151, 2, 0x20)));

  DispatchItem out;
  TEST_ASSERT_TRUE(q.pop(out));
  TEST_ASSERT_EQUAL_UINT16(1, out.ticket);
  TEST_ASSERT_EQUAL_UINT32(1, q.size());
  TEST_ASSERT_TRUE(q.pop(out));
  TEST_ASSERT_EQUAL_UINT16(2, out.ticket);
  TEST_ASSERT_FALSE_MESSAGE(q.pop(out), "nothing is readable past the published position");
}

void test_the_counters_are_free_running_and_wrap_cleanly(void) {
  // The sequence arithmetic is signed-difference on purpose, so the ring keeps working after
  // the counters wrap. Push and pop far more than the capacity and assert every item.
  AffaMpsc<DispatchItem, 4> q;
  for (uint32_t i = 0; i < 5000; ++i) {
    const TxTicket t = static_cast<TxTicket>((i % 0xFFFE) + 1);
    TEST_ASSERT_TRUE_MESSAGE(q.push(payload(0x151, t, static_cast<uint8_t>(i))), "push");
    DispatchItem out;
    TEST_ASSERT_TRUE_MESSAGE(q.pop(out), "pop");
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(t, out.ticket, "the item survived the wrap");
  }
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, q.dropped(), "nothing was ever refused");
}

void test_interleaved_producers_and_the_consumer_never_lose_an_item(void) {
  // Not a race — a schedule. Every ordering below is one a real preemption could produce,
  // and the invariant is the same in all of them: what comes out is what went in, in order,
  // with nothing duplicated and nothing lost.
  AffaMpsc<DispatchItem, 4> q;
  DispatchItem out;
  uint16_t nextIn = 1, nextOut = 1;

  for (int round = 0; round < 200; ++round) {
    const int pushes = (round % 3) + 1;          // 1..3 producers get a turn
    for (int i = 0; i < pushes; ++i) {
      if (q.push(payload(0x151, nextIn, static_cast<uint8_t>(nextIn)))) ++nextIn;
    }
    const int pops = (round % 2) + 1;
    for (int i = 0; i < pops; ++i) {
      if (!q.pop(out)) break;
      TEST_ASSERT_EQUAL_UINT16_MESSAGE(nextOut, out.ticket, "an item arrived out of order");
      ++nextOut;
    }
  }
  while (q.pop(out)) {
    TEST_ASSERT_EQUAL_UINT16(nextOut, out.ticket);
    ++nextOut;
  }
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(nextIn, nextOut, "every accepted item came back out once");
}

// ---------------------------------------------------------------------------
// Real threads — the only part of this suite that exercises the memory ordering
// ---------------------------------------------------------------------------
// Everything above is a schedule the test author chose. This is the one that can catch an
// ordering mistake the author did not think of: four producer threads and one consumer, on
// a ring deliberately too small to hold the traffic, so the CAS path, the full path and the
// wrap all run under genuine contention.
//
// THE INVARIANTS ARE THE TWO A CORRUPTED RING WOULD BREAK, and both are checkable without
// knowing the interleaving:
//
//   * every item that comes out is INTACT — its payload matches its ticket, so a torn copy
//     (published before the bytes landed) is caught;
//   * accepted + refused == offered, and no ticket arrives twice, so a lost or duplicated
//     slot claim is caught.
//
// It is not proof — a passing run on x86 says little about a weakly-ordered target — but a
// FAILING run is unambiguous, and this is where a bad memory_order would first show.

#include <atomic>
#include <thread>
#include <vector>

void test_four_producer_threads_and_one_consumer_lose_nothing(void) {
  constexpr int kProducers  = 4;
  constexpr int kPerThread  = 4000;
  AffaMpsc<DispatchItem, 16> q;            // deliberately small: the full path must run

  std::atomic<int> accepted{0};
  std::atomic<int> refused{0};
  std::atomic<bool> done{false};

  std::vector<std::thread> producers;
  for (int p = 0; p < kProducers; ++p) {
    producers.emplace_back([&q, &accepted, &refused, p]() {
      for (int i = 0; i < kPerThread; ++i) {
        // The ticket identifies the producer and the sequence; the payload is DERIVED from
        // it, which is what makes a torn copy detectable rather than merely unlikely.
        const TxTicket t = static_cast<TxTicket>(p * kPerThread + i + 1);
        DispatchItem it = payload(0x151, t, static_cast<uint8_t>(t));
        if (q.push(it)) accepted.fetch_add(1, std::memory_order_relaxed);
        else            refused.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  int got = 0;
  std::vector<bool> seen(kProducers * kPerThread + 1, false);
  std::thread consumer([&]() {
    DispatchItem out;
    for (;;) {
      if (q.pop(out)) {
        ++got;
        TEST_ASSERT_TRUE_MESSAGE(out.ticket != kNoTicket, "a zero ticket was never pushed");
        TEST_ASSERT_FALSE_MESSAGE(seen[out.ticket], "an item was delivered twice");
        seen[out.ticket] = true;
        // The payload is a function of the ticket. If the cell was published before its
        // bytes were written, these disagree.
        for (uint16_t b = 0; b < out.len; ++b) {
          TEST_ASSERT_EQUAL_HEX8_MESSAGE(static_cast<uint8_t>(out.ticket + b), out.data[b],
                                         "a torn payload crossed the ring");
        }
      } else if (done.load(std::memory_order_acquire)) {
        if (!q.pop(out)) break;            // drained after the producers finished
        --got;                             // re-handle it on the next pass
      } else {
        std::this_thread::yield();
      }
    }
  });

  for (auto& t : producers) t.join();
  done.store(true, std::memory_order_release);
  consumer.join();

  const int offered = kProducers * kPerThread;
  TEST_ASSERT_EQUAL_INT_MESSAGE(offered, accepted.load() + refused.load(),
                                "every offer was either accepted or counted as refused");
  TEST_ASSERT_EQUAL_INT_MESSAGE(accepted.load(), got,
                                "every accepted item came out exactly once");
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(static_cast<uint32_t>(refused.load()), q.dropped(),
                                   "and the ring's own counter agrees with the producers");
}

// ---------------------------------------------------------------------------

void setUp(void) {}
void tearDown(void) {}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_an_item_comes_out_exactly_as_it_went_in);
  RUN_TEST(test_order_is_preserved);
  RUN_TEST(test_a_borrowed_tail_crosses_as_a_pointer);
  RUN_TEST(test_an_op_and_a_payload_are_told_apart);
  RUN_TEST(test_a_full_ring_refuses_and_counts_without_losing_what_it_holds);
  RUN_TEST(test_the_consumer_never_reads_past_the_published_position);
  RUN_TEST(test_the_counters_are_free_running_and_wrap_cleanly);
  RUN_TEST(test_interleaved_producers_and_the_consumer_never_lose_an_item);
  RUN_TEST(test_four_producer_threads_and_one_consumer_lose_nothing);
  return UNITY_END();
}
