// media_render.h — the 48x48 nav pane, drawn on the device, one frame at a time.
//
// Everything here writes into a 288-byte buffer in the OEM's own packing: row-major, 6
// bytes per row, MSB-first. That is the format carminat::showNavBitmap() sends, and it is
// the format tools/gen_navicons.js emits, so a frame drawn here and a frame baked into
// flash are the same thing.
//
// WHY DRAW ON THE DEVICE AT ALL. Baking frames into flash costs 288 bytes each and can only
// ever animate what was decided at build time. A media pane has to show a clock that
// advances, a progress bar that fills and a spectrum that moves — none of which is known
// until it is running. 288 bytes of RAM and a 5x7 font buys all three.
#pragma once
#include <stdint.h>
#include <string.h>

namespace media {

constexpr uint16_t kW = 48, kH = 48, kStride = 6, kBytes = kStride * kH;

// ---------------------------------------------------------------------------
// A 5x7 font — digits, a colon, a slash and the handful of letters the pane uses.
// One byte per row, top five bits significant.
// ---------------------------------------------------------------------------
struct Glyph { uint8_t r[7]; };

inline constexpr Glyph kDigits[10] = {
  {{0x70,0x88,0x98,0xA8,0xC8,0x88,0x70}}, // 0
  {{0x20,0x60,0x20,0x20,0x20,0x20,0x70}}, // 1
  {{0x70,0x88,0x08,0x10,0x20,0x40,0xF8}}, // 2
  {{0x70,0x88,0x08,0x30,0x08,0x88,0x70}}, // 3
  {{0x10,0x30,0x50,0x90,0xF8,0x10,0x10}}, // 4
  {{0xF8,0x80,0xF0,0x08,0x08,0x88,0x70}}, // 5
  {{0x30,0x40,0x80,0xF0,0x88,0x88,0x70}}, // 6
  {{0xF8,0x08,0x10,0x20,0x40,0x40,0x40}}, // 7
  {{0x70,0x88,0x88,0x70,0x88,0x88,0x70}}, // 8
  {{0x70,0x88,0x88,0x78,0x08,0x10,0x60}}, // 9
};
inline constexpr Glyph kColon = {{0x00,0x20,0x20,0x00,0x20,0x20,0x00}};
inline constexpr Glyph kSlash = {{0x08,0x08,0x10,0x20,0x40,0x80,0x80}};
inline constexpr Glyph kBlank = {{0,0,0,0,0,0,0}};

// ---------------------------------------------------------------------------
// Primitives
// ---------------------------------------------------------------------------
inline void clear(uint8_t* b) { memset(b, 0, kBytes); }

inline void px(uint8_t* b, int x, int y) {
  if (x < 0 || x >= static_cast<int>(kW) || y < 0 || y >= static_cast<int>(kH)) return;
  b[y * kStride + (x >> 3)] |= 0x80 >> (x & 7);
}

inline void hline(uint8_t* b, int x0, int x1, int y) { for (int x = x0; x <= x1; ++x) px(b, x, y); }
inline void vline(uint8_t* b, int x, int y0, int y1) { for (int y = y0; y <= y1; ++y) px(b, x, y); }

inline void rect(uint8_t* b, int x0, int y0, int x1, int y1, bool fill) {
  if (fill) { for (int y = y0; y <= y1; ++y) hline(b, x0, x1, y); return; }
  hline(b, x0, x1, y0); hline(b, x0, x1, y1);
  vline(b, x0, y0, y1); vline(b, x1, y0, y1);
}

inline void glyph(uint8_t* b, const Glyph& g, int x, int y) {
  for (int r = 0; r < 7; ++r)
    for (int c = 0; c < 5; ++c)
      if (g.r[r] & (0x80 >> c)) px(b, x + c, y + r);
}

// "M:SS" / "MM:SS" from a second count, left-aligned at x.
inline void timeAt(uint8_t* b, int x, int y, uint32_t secs) {
  const uint32_t m = (secs / 60) % 100, s = secs % 60;
  int at = x;
  if (m >= 10) { glyph(b, kDigits[m / 10], at, y); at += 6; }
  glyph(b, kDigits[m % 10], at, y); at += 6;
  glyph(b, kColon, at, y);          at += 4;
  glyph(b, kDigits[s / 10], at, y); at += 6;
  glyph(b, kDigits[s % 10], at, y);
}

// "nn/nn", right-aligned so the track counter does not jitter as it grows.
inline void trackAt(uint8_t* b, int x, int y, uint8_t n, uint8_t total) {
  int at = x;
  glyph(b, kDigits[(n / 10) % 10], at, y);     at += 6;
  glyph(b, kDigits[n % 10], at, y);            at += 6;
  glyph(b, kSlash, at, y);                     at += 6;
  glyph(b, kDigits[(total / 10) % 10], at, y); at += 6;
  glyph(b, kDigits[total % 10], at, y);
}

// ---------------------------------------------------------------------------
// The pane
// ---------------------------------------------------------------------------
// 48x48 is not much, so every band earns its rows:
//
//   0..6    elapsed left, remaining right          the two numbers a listener wants
//   9..30   eight spectrum bars                    the thing that makes it look alive
//   33..36  progress bar                           where you are in the track
//   39..45  transport glyph + track counter        state, at a glance
//
// The spectrum is the only part that is a lie — there is no audio here to analyse — so it
// is driven by a per-bar random walk rather than pretending to be an FFT. It moves like a
// spectrum because a spectrum's neighbouring bins are correlated and its peaks decay, and
// both of those are cheap to imitate.
struct Bars {
  uint8_t h[8]    = {4, 9, 14, 11, 7, 12, 6, 3};   // current height, 0..kBarMax
  uint8_t peak[8] = {0};                            // peak-hold marker, decays slower
  uint32_t rng    = 0x1F1A5EED;

  static constexpr uint8_t kBarMax = 21;

  uint32_t next() {                                 // xorshift32 — deterministic and cheap
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return rng;
  }

  void step(bool playing) {
    for (int i = 0; i < 8; ++i) {
      if (playing) {
        // Neighbour-correlated: each bar is pulled toward the one below it, then kicked.
        const int pull  = (i > 0) ? (h[i - 1] - h[i]) / 3 : 0;
        const int kick  = static_cast<int>(next() % 9) - 4;
        int v = static_cast<int>(h[i]) + pull + kick;
        if (v < 1) v = 1;
        if (v > kBarMax) v = kBarMax;
        h[i] = static_cast<uint8_t>(v);
      } else if (h[i] > 1) {
        h[i]--;                                     // paused: settle, do not freeze
      }
      if (h[i] > peak[i]) peak[i] = h[i];
      else if (peak[i] > 0 && (next() & 3) == 0) peak[i]--;   // slow decay
    }
  }
};

inline void drawMedia(uint8_t* b, const Bars& bars, uint32_t elapsed, uint32_t total,
                      uint8_t track, uint8_t trackCount, bool playing) {
  clear(b);

  // --- times ---------------------------------------------------------------
  timeAt(b, 0, 0, elapsed);
  const uint32_t left = (total > elapsed) ? total - elapsed : 0;
  timeAt(b, 26, 0, left);

  // --- spectrum ------------------------------------------------------------
  // Eight bars, 5 px wide with a 1 px gap: 8*6-1 = 47, one column short of the pane, which
  // is what keeps the block visually centred without a special case.
  constexpr int kBase = 30;                 // bars grow upward from here
  for (int i = 0; i < 8; ++i) {
    const int x0 = i * 6, x1 = x0 + 4;
    const int top = kBase - bars.h[i];
    rect(b, x0, top, x1, kBase, true);
    const int pk = kBase - bars.peak[i] - 2;
    if (pk >= 9 && pk < top - 1) hline(b, x0, x1, pk);       // peak-hold marker
  }

  // --- progress ------------------------------------------------------------
  rect(b, 0, 33, 47, 36, false);
  if (total) {
    const uint32_t fill = (46u * (elapsed < total ? elapsed : total)) / total;
    if (fill) rect(b, 1, 34, 1 + static_cast<int>(fill), 35, true);
  }

  // --- transport glyph + counter ------------------------------------------
  if (playing) {
    for (int r = 0; r < 7; ++r) {                   // a triangle, narrowing as it goes
      const int w = 4 - (r > 3 ? r - 3 : 3 - r);
      for (int c = 0; c <= w; ++c) px(b, 1 + c, 39 + r);
    }
  } else {
    rect(b, 1, 39, 2, 45, true);
    rect(b, 5, 39, 6, 45, true);
  }
  trackAt(b, 18, 39, track, trackCount);
}

}  // namespace media
