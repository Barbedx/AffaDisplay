#pragma once
#include "../AffaConfig.h"
#include <cstddef>
#include <cstring>

namespace affa {

// The panel charset cannot render UTF-8. Every string that reaches the wire passes
// through toAscii(), inside the library, at the single choke point in each frame builder.
// Losing it looks like garbage on the screen, not a compile error — which is why it is
// mandatory rather than a courtesy.
//
// Pure C, allocation-free, no Arduino String anywhere.
//
// Contract, identical for both functions:
//   * returns the number of bytes written, NOT counting the terminating NUL;
//   * always NUL-terminates when outSize > 0;
//   * truncation never splits a multi-byte UTF-8 input sequence, and never emits half of
//     a multi-character replacement ("Sch" is written whole or not at all);
//   * unknown non-ASCII codepoints become '?';
//   * idempotent on 7-bit ASCII input;
//   * `in == nullptr` is treated as an empty string.

#if AFFA_ENABLE_TRANSLITERATION

size_t toAscii(const char* in, char* out, size_t outSize);

// toAscii plus the title cleanup: strips the Apple Music video annotations and trims
// surrounding whitespace. The annotation list is Ukrainian/English as observed in the
// wild; it is deliberately a fixed list rather than a pattern, because a pattern that
// guessed would eat legitimate titles.
size_t normalizeTitle(const char* in, char* out, size_t outSize);

#else

// AFFA_ENABLE_TRANSLITERATION = 0 — a bounded copy that passes bytes through unchanged.
// Defined inline here rather than in the .cpp, whose entire body is compiled out, so that
// call sites keep linking. UTF-8 that reaches the wire in this mode renders as garbage on
// the panel; set the gate to 0 only if every string you pass is already 7-bit ASCII.
inline size_t toAscii(const char* in, char* out, size_t outSize) {
  if (!out || outSize == 0) return 0;
  size_t n = 0;
  if (in) {
    while (in[n] && n + 1 < outSize) { out[n] = in[n]; ++n; }
  }
  out[n] = '\0';
  return n;
}

inline size_t normalizeTitle(const char* in, char* out, size_t outSize) {
  return toAscii(in, out, outSize);
}

#endif // AFFA_ENABLE_TRANSLITERATION

} // namespace affa
