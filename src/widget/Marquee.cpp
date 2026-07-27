// Entire body gated, so a build without the marquee compiles this to an empty object.
#include "Marquee.h"

#if AFFA_ENABLE_MARQUEE

#include "../util/AffaText.h"
#include <cstring>

namespace affa {
namespace widget {
namespace {

// Sanitise ONCE, at construction, rather than defending in every method. A zero step
// divides by zero in windowAt(); a zero width draws nothing. The upper bound is the text
// buffer: the gap is reserved out of it, so at or above this the marquee has no room for
// one window of actual text and could never move.
MarqueeGeometry sane(MarqueeGeometry g) {
  if (g.width  == 0) g.width  = 1;
  if (g.stepMs == 0) g.stepMs = 1;
  if (static_cast<unsigned>(g.width) + g.gap >= AFFA_TEXT_MAX) {
    g.gap   = 0;
    g.width = static_cast<uint8_t>(AFFA_TEXT_MAX - 1);
  }
  return g;
}

}  // namespace

Marquee::Marquee(const MarqueeGeometry& g) : _geom(sane(g)) {}

bool Marquee::setText(const char* text, uint32_t now) {
  char buf[AFFA_TEXT_MAX];

  // normalizeTitle strips the video annotations and trims, on top of transliterating. The
  // gap is reserved out of the buffer up front so appending it can never truncate
  // mid-sequence.
  const size_t n = normalizeTitle(text, buf, sizeof(buf) - _geom.gap);
  if (n == 0) {
    // Nothing to scroll. The caller deliberately transmits nothing on this: blanking the
    // panel because an application published an empty title would be a decision the
    // application did not make.
    const bool changed = _text[0] != '\0';
    _text[0] = '\0';
    _len     = 0;
    _base    = 0;
    return changed;
  }

  size_t i = n;
  for (uint8_t g = 0; g < _geom.gap && i + 1 < sizeof(buf); ++g) buf[i++] = ' ';
  buf[i] = '\0';

  if (std::strcmp(buf, _text) == 0) return false;   // the line that lets it keep scrolling

  std::memcpy(_text, buf, i + 1);
  _len     = static_cast<uint16_t>(i);
  _base    = 0;
  _epochMs = now;
  return true;
}

void Marquee::setActive(bool on, uint32_t now) {
  if (on == _active) return;
  // Resuming: _base is already the frozen position, so re-basing the epoch is the whole of
  // it. Pausing: freeze where we are first.
  if (!on) _base = windowAt(now);
  _epochMs = now;
  _active  = on;
}

uint16_t Marquee::windowAt(uint32_t now) const {
  if (_len == 0) return 0;
  const uint32_t steps = (now - _epochMs) / _geom.stepMs;
  return static_cast<uint16_t>((static_cast<uint32_t>(_base) + steps) % _len);
}

void Marquee::window(uint16_t pos, char* out, uint8_t outSize) const {
  if (!out || outSize == 0) return;
  if (_len == 0) { out[0] = '\0'; return; }

  uint8_t n = _geom.width;
  if (n > outSize - 1) n = static_cast<uint8_t>(outSize - 1);
  for (uint8_t i = 0; i < n; ++i) out[i] = _text[(pos + i) % _len];
  out[n] = '\0';
}

}  // namespace widget
}  // namespace affa

#endif  // AFFA_ENABLE_MARQUEE
