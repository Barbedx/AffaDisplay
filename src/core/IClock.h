#pragma once
#include <cstdint>

namespace affa {

// There is deliberately NO delayMs(). Nothing in this library is allowed to sleep; the
// legacy port had one, and it is what let a 2000 ms ACK wait sit inside the only code
// path that could have delivered the ACK. If an implementation of something in this
// library wants a delay, the state machine is wrong.
struct IClock {
  virtual ~IClock() = default;
  virtual uint32_t millis() const = 0;
};

// All time comparisons in the library are wrap-safe and written exactly one way:
//
//     if (affa::expired(now, deadline)) { ... }
//
// Never `now > deadline`, and never `now - last > interval` with an unsigned `last`
// sourced from a different epoch.
inline bool expired(uint32_t now, uint32_t deadline) {
  return static_cast<int32_t>(now - deadline) >= 0;
}

} // namespace affa
