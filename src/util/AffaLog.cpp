#include "../AffaConfig.h"          // the ONLY thing outside the gate
#if AFFA_ENABLE_LOG

#include "AffaLog.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>   // strlen, for the SerialSink FIFO check

#if defined(ARDUINO)
#include <Arduino.h>
#endif

namespace affa {
namespace detail {
namespace {

// One fixed buffer, no allocation. Anything longer is truncated — a log line is
// diagnostics, and losing its tail is preferable to a heap call on a RAM-tight target.
constexpr size_t kLineMax = 160;

#if defined(ARDUINO)
// Default sink on target. It is the only place in the library that touches Serial, and
// it does not open it: a library that calls Serial.begin() steals a baud rate the
// application already chose.
// IT DROPS RATHER THAN WAITS, AND THAT IS THE WHOLE POINT.
//
// Serial.print() busy-waits when the UART TX FIFO is full, and this sink is reached from
// INSIDE poll() — setSync() logs a state change, pumpRx() logs a ring overflow,
// handleAckFrame() logs a rejected ACK. At the default AFFA_LOG_LEVEL those are rare. At
// level 4 or 5 they are not: a held wheel detent logs a line per key frame, and once the
// 128-byte FIFO fills, each line stalls the RX drain for ~2.6 ms at 115200 baud. The
// 32-deep RX ring then overflows and Stats::ringOverflow climbs — the library reporting
// "poll() is being called too slowly" when the only thing being slow is its own default
// log sink. A library that forbids blocking must not ship a blocking sink.
//
// So: if the line does not fit in the FIFO right now, throw it away and count it. A lost
// diagnostic beats a stalled protocol, and `dropped()` makes the loss visible instead of
// leaving a silent gap in the trace. Application sinks (the bench console's ring) are
// unaffected — this is only what you get when you install nothing.
class SerialSink final : public ILogSink {
 public:
  void write(uint8_t level, const char* tag, const char* msg) override {
    static const char kLvl[] = {'?', 'E', 'W', 'I', 'D', 'T'};
    const char c = (level <= 5) ? kLvl[level] : '?';

    // "[X] tag: msg\r\n" — count it before writing any of it, so a line is emitted whole
    // or not at all. A half-written line in a trace is worse than a missing one.
    const size_t need = 5 + strlen(tag) + 2 + strlen(msg) + 2;
    if (static_cast<size_t>(Serial.availableForWrite()) < need) {
      ++_dropped;
      return;
    }
    Serial.print('[');
    Serial.print(c);
    Serial.print(']');
    Serial.print(' ');
    Serial.print(tag);
    Serial.print(F(": "));
    Serial.println(msg);
  }

  uint32_t dropped() const { return _dropped; }

 private:
  uint32_t _dropped = 0;
};
SerialSink s_default;
ILogSink*  s_sink = &s_default;
#else
// On the host the default is silence. A test that wants the trace installs its own sink;
// a test that does not must not have its output polluted by the code under test.
ILogSink* s_sink = nullptr;
#endif

} // namespace

void setSink(ILogSink* s) { s_sink = s; }
ILogSink* sink() { return s_sink; }

void emit(uint8_t level, const char* tag, const char* fmt, ...) {
  ILogSink* const s = s_sink;
  if (!s) return;              // format nothing if nobody is listening
  char line[kLineMax];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  s->write(level, tag, line);
}

} // namespace detail
} // namespace affa

#endif // AFFA_ENABLE_LOG
