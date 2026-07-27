#pragma once
#include <cstdint>
#include <atomic>

namespace affa {

// Lock-free single-producer / single-consumer ring.
//
// Producer = the esp32_can general callback, running in task_CAN (prio 15, unpinned, so
//            possibly a different core from the consumer).
// Consumer = whatever task calls AffaDisplayBase::poll().
//
// Exactly one thread may call push(); exactly one may call pop(). Two producers or two
// consumers corrupt it silently — there is no lock and there is not going to be one,
// because push() runs in a driver task that must never block. It must also stay
// consistent if that task is deleted mid-push, which the publish-last store below
// guarantees: at worst the in-flight frame is lost.
//
// N must be a power of two: the modulo is a mask, and the head/tail counters are
// free-running so a full ring is distinguishable from an empty one without a spare slot
// or a separate count.
template <typename T, uint16_t N>
class AffaRing {
  static_assert(N >= 2 && (N & (N - 1)) == 0, "AffaRing capacity must be a power of two");

 public:
  // Producer side. Returns false and bumps overflow() when full; the frame is LOST.
  //
  // DROPPING THE NEWEST IS DELIBERATE. Overwriting the oldest would hand the protocol
  // layer a sequence with a hole in the middle of an ISO-TP transfer, which decodes into
  // a plausible-looking wrong screen. Losing the tail of a burst instead shows up as a
  // missed ACK, which the transmit FSM already handles as a timeout, and as a non-zero
  // ringOverflow counter that says exactly what happened.
  bool push(const T& v) {
    const uint32_t h = _head.load(std::memory_order_relaxed);
    const uint32_t t = _tail.load(std::memory_order_acquire);
    if (static_cast<uint32_t>(h - t) >= N) {
      _overflow.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    _buf[h & (N - 1)] = v;
    _head.store(h + 1, std::memory_order_release);   // publish last
    return true;
  }

  // Consumer side.
  bool pop(T& out) {
    const uint32_t t = _tail.load(std::memory_order_relaxed);
    const uint32_t h = _head.load(std::memory_order_acquire);
    if (h == t) return false;
    out = _buf[t & (N - 1)];
    _tail.store(t + 1, std::memory_order_release);
    return true;
  }

  bool empty() const {
    return _head.load(std::memory_order_acquire) == _tail.load(std::memory_order_acquire);
  }
  uint32_t size() const {
    return _head.load(std::memory_order_acquire) - _tail.load(std::memory_order_acquire);
  }
  static constexpr uint16_t capacity() { return N; }
  uint32_t overflow() const { return _overflow.load(std::memory_order_relaxed); }

  // Consumer side only, and only while the producer is known idle (before begin()).
  void reset() {
    _head.store(0, std::memory_order_relaxed);
    _tail.store(0, std::memory_order_relaxed);
    _overflow.store(0, std::memory_order_relaxed);
  }

 private:
  T _buf[N];
  std::atomic<uint32_t> _head{0};   // written by producer only
  std::atomic<uint32_t> _tail{0};   // written by consumer only
  std::atomic<uint32_t> _overflow{0};
};

} // namespace affa
