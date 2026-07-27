#pragma once
#include "../AffaConfig.h"
#include <cstdint>

namespace affa {

// Levels are ordered; AFFA_LOG_LEVEL compiles out everything above it.
// 0 = off, 1 = error, 2 = warn, 3 = info, 4 = debug, 5 = trace.
//
// A sink is a LEAF. It may be called with the library's internal state mid-transition and
// must never call back into the library.
struct ILogSink {
  virtual ~ILogSink() = default;
  virtual void write(uint8_t level, const char* tag, const char* msg) = 0;
};

namespace detail {
#if AFFA_ENABLE_LOG
void setSink(ILogSink* s);
ILogSink* sink();
void emit(uint8_t level, const char* tag, const char* fmt, ...)
     __attribute__((format(printf, 3, 4)));
#else
inline void setSink(ILogSink*) {}
inline ILogSink* sink() { return nullptr; }
#endif
} // namespace detail

} // namespace affa

// THE DISABLED MACRO DISCARDS ITS ARGUMENTS ENTIRELY. A format string that is never
// referenced is never emitted into .rodata; that is the whole point of the knob.
// The consequence: NEVER put a side effect inside a log argument. AFFA_LOGD("X", "%d",
// counter++) compiles to nothing and the increment disappears with it. CI greps for
// ++, -- and = inside AFFA_LOG* argument lists for exactly this reason.
#if AFFA_ENABLE_LOG
#  define AFFA_LOG_(lvl, tag, ...) ::affa::detail::emit((lvl), (tag), __VA_ARGS__)
#else
#  define AFFA_LOG_(lvl, tag, ...) do {} while (0)
#endif

#if AFFA_ENABLE_LOG && AFFA_LOG_LEVEL >= 1
#  define AFFA_LOGE(tag, ...) AFFA_LOG_(1, tag, __VA_ARGS__)
#else
#  define AFFA_LOGE(tag, ...) do {} while (0)
#endif

#if AFFA_ENABLE_LOG && AFFA_LOG_LEVEL >= 2
#  define AFFA_LOGW(tag, ...) AFFA_LOG_(2, tag, __VA_ARGS__)
#else
#  define AFFA_LOGW(tag, ...) do {} while (0)
#endif

#if AFFA_ENABLE_LOG && AFFA_LOG_LEVEL >= 3
#  define AFFA_LOGI(tag, ...) AFFA_LOG_(3, tag, __VA_ARGS__)
#else
#  define AFFA_LOGI(tag, ...) do {} while (0)
#endif

#if AFFA_ENABLE_LOG && AFFA_LOG_LEVEL >= 4
#  define AFFA_LOGD(tag, ...) AFFA_LOG_(4, tag, __VA_ARGS__)
#else
#  define AFFA_LOGD(tag, ...) do {} while (0)
#endif

#if AFFA_ENABLE_LOG && AFFA_LOG_LEVEL >= 5
#  define AFFA_LOGT(tag, ...) AFFA_LOG_(5, tag, __VA_ARGS__)
#else
#  define AFFA_LOGT(tag, ...) do {} while (0)
#endif
