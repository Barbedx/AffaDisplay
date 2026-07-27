#include "../AffaConfig.h"          // the ONLY thing outside the gate
#if AFFA_ENABLE_LOG

#include "AffaLog.h"
#include <cstdarg>
#include <cstdio>

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
class SerialSink final : public ILogSink {
 public:
  void write(uint8_t level, const char* tag, const char* msg) override {
    static const char kLvl[] = {'?', 'E', 'W', 'I', 'D', 'T'};
    const char c = (level <= 5) ? kLvl[level] : '?';
    Serial.print('[');
    Serial.print(c);
    Serial.print(']');
    Serial.print(' ');
    Serial.print(tag);
    Serial.print(F(": "));
    Serial.println(msg);
  }
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
