// Crossing a task boundary, as data.
//
// THE PROBLEM THIS SOLVES, stated once. Every render in this library is a pure byte-builder:
// it reads its arguments and constant tables, writes a payload into a buffer on the CALLING
// task's stack, and hands it to AffaDisplayBase::enqueue(). Only that last step touches
// shared state — the transmit queue, which the poll task also owns. So the whole cross-task
// problem is one function wide, and it is solved by copying the finished bytes across
// instead of the call.
//
// WHY NOT A LOCK. Two reasons, and both are load-bearing:
//
//   * enqueue() completes a superseded ticket when a render coalesces, which fires the
//     application's CompleteCb. Holding a lock across an arbitrary user callback is exactly
//     the deadlock docs/NOTES.md §3 rejected mutexes for.
//   * enqueue() reads linkReady(), the sync state, the phase and the clock — all of it
//     mutated by pumpTx() on the poll task. A lock that made that safe would have to cover
//     the poll task's whole transmit pump, so a low-priority web handler could hold off a
//     2 ms protocol task. That is priority inversion on the one deadline the library cannot
//     miss.
//
// Copying the intent across leaves every state-dependent decision on the task that owns the
// state, which is the only place it was ever correct.
//
// NOTHING IN THIS FILE TOUCHES FreeRTOS. It is std::atomic and C++17, so it compiles and is
// tested on the host — which is the whole reason the interesting half of the threading model
// can be tested at all.
#pragma once
#include "../AffaConfig.h"
#include "AffaTypes.h"
#include <atomic>
#include <cstdint>

namespace affa {

// ---------------------------------------------------------------------------
// The bounded multi-producer / single-consumer ring
// ---------------------------------------------------------------------------
// AffaRing next door is single-producer: one RX task pushes, poll() pops, and each index is
// written by exactly one thread. That is not this problem. Here ANY number of tasks may push
// — a web handler, a BLE callback, the Arduino loop — while exactly one (the poll owner)
// pops.
//
// So each cell carries its own sequence number and producers claim a slot by CAS before
// writing it. A cell is readable only once its sequence says so, which is what makes a
// producer that is preempted between claiming and publishing harmless: the consumer sees the
// cell as not-yet-ready and stops there rather than reading a half-written payload.
//
// N must be a power of two: the index is a mask, and the counters are free-running so a full
// ring is distinguishable from an empty one without a spare slot.
template <typename T, uint16_t N>
class AffaMpsc {
  static_assert(N >= 2 && (N & (N - 1)) == 0, "AffaMpsc capacity must be a power of two");

 public:
  AffaMpsc() { reset(); }

  // PRODUCER SIDE, CALLABLE FROM ANY TASK, AND IT NEVER BLOCKS. False when the ring is full;
  // the item is refused, never overwritten, and the refusal is counted.
  //
  // Refusing rather than overwriting is the same choice AffaRing made and for the same
  // reason: a render silently replaced by a later one is a screen that never appears with no
  // symptom, whereas a counted refusal is a number on a status page.
  bool push(const T& v) {
    uint32_t pos = _enq.load(std::memory_order_relaxed);
    for (;;) {
      Cell& c = _buf[pos & (N - 1)];
      const uint32_t seq = c.seq.load(std::memory_order_acquire);
      const int32_t diff = static_cast<int32_t>(seq) - static_cast<int32_t>(pos);
      if (diff == 0) {
        // The slot is ours if the claim lands. compare_exchange_weak updates `pos` on
        // failure, so a lost race retries against the position the winner left behind.
        if (_enq.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
          c.data = v;
          // PUBLISH LAST. Everything above must be visible to the consumer before the
          // sequence says the cell is readable.
          c.seq.store(pos + 1, std::memory_order_release);
          return true;
        }
      } else if (diff < 0) {
        _dropped.fetch_add(1, std::memory_order_relaxed);
        return false;                       // full
      } else {
        pos = _enq.load(std::memory_order_relaxed);   // another producer moved on
      }
    }
  }

  // CONSUMER SIDE — the poll owner, and only the poll owner. `_deq` is deliberately a plain
  // uint32_t: one thread writes it, and the cell sequence carries the ordering.
  bool pop(T& out) {
    Cell& c = _buf[_deq & (N - 1)];
    const uint32_t seq = c.seq.load(std::memory_order_acquire);
    if (static_cast<int32_t>(seq) - static_cast<int32_t>(_deq + 1) != 0) return false;
    out = c.data;
    // Release the cell for the producer N positions ahead.
    c.seq.store(_deq + N, std::memory_order_release);
    ++_deq;
    return true;
  }

  // Items refused because the ring was full. Free-running; never sampled and reset, so a
  // burst that filled it once is still visible an hour later.
  uint32_t dropped() const { return _dropped.load(std::memory_order_relaxed); }

  // Approximate — producers may be mid-claim. For a status page, never for a decision.
  uint32_t size() const {
    const uint32_t e = _enq.load(std::memory_order_acquire);
    return e - _deq;
  }
  bool empty() const { return size() == 0; }
  static constexpr uint16_t capacity() { return N; }

  // Only while no producer can be running — begin(), or before the task starts.
  void reset() {
    for (uint16_t i = 0; i < N; ++i) _buf[i].seq.store(i, std::memory_order_relaxed);
    _enq.store(0, std::memory_order_relaxed);
    _deq = 0;
    _dropped.store(0, std::memory_order_relaxed);
  }

 private:
  struct Cell {
    std::atomic<uint32_t> seq;
    T                     data;
  };

  Cell                  _buf[N];
  std::atomic<uint32_t> _enq{0};        // claimed by producers, by CAS
  uint32_t              _deq = 0;       // consumer only
  std::atomic<uint32_t> _dropped{0};
};

// ---------------------------------------------------------------------------
// What crosses
// ---------------------------------------------------------------------------

// The handful of calls that are NOT byte-builders. Everything else — every render in every
// panel, present and future — crosses as a payload and needs no entry here, which is the
// whole point of putting the boundary at enqueue() instead of over a hand-written table of
// operations. The pre-2.0 command queue had fourteen render ops and had to be edited for
// every new screen; twelve were added and none were mirrored (docs/API.md §7).
//
// This list is closed and short because it is exactly the calls that mutate library state
// rather than build bytes.
enum class DispatchOp : uint8_t {
  None = 0,
  PressKey,        // a,b = the raw key code; c = hold; d = KeySource
  AbortPending,
  AbortAll,
  Resync,          // re-runs begin() on the owning task
};

// ONE POSTED INTENT. A POD, copied by value: no pointer into a caller's stack ever crosses a
// task boundary — except the two that are explicitly BORROWED, and their contract already
// says the bytes must outlive the ticket.
//
// SIZE IS THE PAYLOAD PLUS ABOUT SIXTEEN BYTES, and it replaced a record carrying three
// 48-byte strings. A queued render used to truncate at AFFA_TASK_ARG_MAX; it now truncates
// where the wire does, because what crosses is the finished frame rather than the arguments
// somebody would have built it from.
struct DispatchItem {
  // The ticket was minted by the calling task before this was posted, so the caller already
  // holds it. The drain must NOT mint a second one — that is the two-handle-space bug 2.0
  // deleted (see Submitted in AffaTypes.h).
  TxTicket   ticket = kNoTicket;

  DispatchOp op     = DispatchOp::None;   // None == this is a payload
  uint8_t    a = 0, b = 0, c = 0, d = 0;  // op arguments; unused for a payload

  uint16_t   funcId = 0;
  TxOptions  opt{};
  // Non-null when the tail of the payload is BORROWED — enqueueExternal() and
  // enqueueSplit(). Bytes [0, prefixLen) come from `data`, the rest from here. Crossing a
  // task changes nothing about that contract: the caller already owed us those bytes until
  // the ticket completes.
  const uint8_t* ext = nullptr;
  uint16_t   prefixLen = 0;
  uint16_t   len       = 0;
  uint8_t    data[AFFA_MAX_PAYLOAD] = {0};

  bool isPayload() const { return op == DispatchOp::None; }
};

}  // namespace affa
