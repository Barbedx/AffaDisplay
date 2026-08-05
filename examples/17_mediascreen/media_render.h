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

// ---------------------------------------------------------------------------
// The other scenes
// ---------------------------------------------------------------------------
// Every one of these is drawn from a frame counter and a little state, never from a table of
// baked frames. 288 bytes per baked frame is the reason: at 4 fps a ten-second loop would be
// 11 kB of flash for one animation, and none of it could show a clock that is actually right.
//
// They also exist to answer a question a still image cannot: how much MOTION this pane will
// carry. A 44-frame ISO-TP transfer per image at BlockSize 1 is a hard rate limit, so the
// interesting scenes are the ones that still read at 3-5 fps — which is why several of these
// lean on persistence (trails, peak-hold, sweeps) rather than on smoothness.

inline int isin(int deg) {              // sine * 1024, integer, 1-degree resolution
  static const int16_t q[91] = {
    0,18,36,54,71,89,107,125,143,160,178,195,213,230,248,265,282,299,316,333,350,367,384,400,
    416,433,449,465,481,496,512,527,543,558,573,587,602,616,630,644,658,672,685,698,711,724,
    737,749,761,773,784,796,807,818,828,839,849,859,868,878,887,896,904,912,920,928,935,942,
    949,955,962,967,973,978,983,987,992,995,999,1002,1005,1008,1010,1012,1013,1014,1015,1016,
    1016,1016,1016};
  deg = ((deg % 360) + 360) % 360;
  if (deg <= 90)  return q[deg];
  if (deg <= 180) return q[180 - deg];
  if (deg <= 270) return -q[deg - 180];
  return -q[360 - deg];
}
inline int icos(int deg) { return isin(deg + 90); }

inline void line(uint8_t* b, int x0, int y0, int x1, int y1) {
  int dx = x1 - x0, dy = y1 - y0;
  const int n = (abs(dx) > abs(dy) ? abs(dx) : abs(dy));
  if (n == 0) { px(b, x0, y0); return; }
  for (int i = 0; i <= n; ++i) px(b, x0 + dx * i / n, y0 + dy * i / n);
}

inline void circle(uint8_t* b, int cx, int cy, int r) {
  for (int a = 0; a < 360; a += 4)
    px(b, cx + r * icos(a) / 1024, cy + r * isin(a) / 1024);
}

// A VU meter: arc scale, swinging needle, and a peak pip that lags behind it. The needle is
// the part that reads at a low frame rate — a swinging line carries motion where a bar chart
// only carries a value.
struct Vu {
  int angle = 0, target = 0, peak = 0;
  uint32_t rng = 0xC0FFEE11;
  void step(bool live) {
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    if (live) target = 20 + static_cast<int>(rng % 120);
    else      target = 15;
    angle += (target - angle) / 2;            // critically damped enough to look mechanical
    if (angle > peak) peak = angle;
    else if (peak > angle + 2) peak -= 3;
  }
};

inline void drawVu(uint8_t* b, const Vu& v) {
  clear(b);
  const int cx = 24, cy = 42, r = 34;
  for (int a = 20; a <= 160; a += 2)          // the arc
    px(b, cx + r * icos(a) / 1024, cy - r * isin(a) / 1024);
  for (int a = 20; a <= 160; a += 20) {       // ticks
    const int x0 = cx + (r - 5) * icos(a) / 1024, y0 = cy - (r - 5) * isin(a) / 1024;
    const int x1 = cx + r * icos(a) / 1024,       y1 = cy - r * isin(a) / 1024;
    line(b, x0, y0, x1, y1);
  }
  const int a = 160 - v.angle;                // needle, drawn thick so it survives the pane
  line(b, cx, cy, cx + (r - 7) * icos(a) / 1024, cy - (r - 7) * isin(a) / 1024);
  line(b, cx - 1, cy, cx + (r - 7) * icos(a) / 1024, cy - (r - 7) * isin(a) / 1024);
  const int pa = 160 - v.peak;
  px(b, cx + (r - 3) * icos(pa) / 1024, cy - (r - 3) * isin(pa) / 1024);
  rect(b, cx - 2, cy - 2, cx + 2, cy + 2, true);
  rect(b, 0, 0, 47, 47, false);
}

// A scrolling waveform. The buffer shifts one column per frame, so the shape drifts across
// the pane and the eye reads continuous motion out of four frames a second.
struct Wave {
  uint8_t h[48];
  int phase = 0;
  Wave() { for (int i = 0; i < 48; ++i) h[i] = 24; }
  void step(bool live) {
    for (int i = 0; i < 47; ++i) h[i] = h[i + 1];
    phase = (phase + 23) % 360;
    const int amp = live ? 18 : 3;
    const int v = 24 + (amp * isin(phase) / 1024) + (amp / 2) * isin(phase * 3) / 1024;
    h[47] = static_cast<uint8_t>(v < 1 ? 1 : (v > 46 ? 46 : v));
  }
};

inline void drawWave(uint8_t* b, const Wave& w) {
  clear(b);
  hline(b, 0, 47, 24);
  for (int x = 0; x < 48; ++x) {
    const int y = w.h[x];
    if (y < 24) rect(b, x, y, x, 24, true); else rect(b, x, 24, x, y, true);
  }
}

// An analogue clock with a second hand — the scene that makes the frame rate visible, and
// the one worth pointing at when somebody asks how live this pane really is.
inline void drawClockFace(uint8_t* b, uint32_t secs) {
  clear(b);
  const int cx = 23, cy = 23, r = 22;
  circle(b, cx, cy, r);
  for (int k = 0; k < 12; ++k) {
    const int a = k * 30;
    line(b, cx + (r - 4) * isin(a) / 1024, cy - (r - 4) * icos(a) / 1024,
            cx + (r - 2) * isin(a) / 1024, cy - (r - 2) * icos(a) / 1024);
  }
  const uint32_t s = secs % 60, m = (secs / 60) % 60, h = (secs / 3600) % 12;
  const int ha = static_cast<int>(h * 30 + m / 2), ma = static_cast<int>(m * 6), sa = static_cast<int>(s * 6);
  line(b, cx, cy, cx + 10 * isin(ha) / 1024, cy - 10 * icos(ha) / 1024);
  line(b, cx + 1, cy, cx + 10 * isin(ha) / 1024, cy - 10 * icos(ha) / 1024);
  line(b, cx, cy, cx + 16 * isin(ma) / 1024, cy - 16 * icos(ma) / 1024);
  line(b, cx, cy, cx + 19 * isin(sa) / 1024, cy - 19 * icos(sa) / 1024);
  rect(b, cx - 1, cy - 1, cx + 1, cy + 1, true);
}

// A starfield, integer perspective, same idea as 13_starfield but in two dimensions of pane
// instead of three rows of text. The depth cue is the RATE, not the size.
struct Stars {
  static constexpr int kN = 28;
  int16_t x[kN], y[kN], z[kN];
  uint32_t rng = 0x5EED1F1A;
  uint32_t next() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
  Stars() { for (int i = 0; i < kN; ++i) respawn(i, true); }
  void respawn(int i, bool anywhere) {
    x[i] = static_cast<int16_t>(next() % 2048) - 1024;
    y[i] = static_cast<int16_t>(next() % 2048) - 1024;
    z[i] = anywhere ? static_cast<int16_t>(1 + next() % 1024) : 1024;
  }
  void step() {
    for (int i = 0; i < kN; ++i) {
      z[i] -= 40;
      if (z[i] < 40) respawn(i, false);
    }
  }
};

inline void drawStars(uint8_t* b, const Stars& s) {
  clear(b);
  for (int i = 0; i < Stars::kN; ++i) {
    const int sx = 24 + (s.x[i] * 24) / s.z[i];
    const int sy = 24 + (s.y[i] * 24) / s.z[i];
    if (sx < 0 || sx > 47 || sy < 0 || sy > 47) continue;
    px(b, sx, sy);
    if (s.z[i] < 400) { px(b, sx + 1, sy); px(b, sx, sy + 1); }   // near stars are fatter
    if (s.z[i] < 180) px(b, sx + 1, sy + 1);
  }
}

// A bouncing box with a decaying trail. The trail is what makes it read at 4 fps: without
// persistence the box appears to teleport.
struct Bounce {
  int x = 8, y = 8, dx = 3, dy = 2, n = 0;
  int8_t tx[8], ty[8];
  Bounce() { for (int i = 0; i < 8; ++i) { tx[i] = 8; ty[i] = 8; } }
  void step() {
    x += dx; y += dy;
    if (x < 0)  { x = 0;  dx = -dx; }
    if (x > 37) { x = 37; dx = -dx; }
    if (y < 0)  { y = 0;  dy = -dy; }
    if (y > 37) { y = 37; dy = -dy; }
    tx[n] = static_cast<int8_t>(x); ty[n] = static_cast<int8_t>(y);
    n = (n + 1) & 7;
  }
};

inline void drawBounce(uint8_t* b, const Bounce& s) {
  clear(b);
  for (int i = 0; i < 8; ++i) {                 // trail: older marks get sparser
    const int k = (s.n + i) & 7;
    if (i < 5) { px(b, s.tx[k] + 5, s.ty[k] + 5); px(b, s.tx[k] + 5, s.ty[k] + 6); }
    else rect(b, s.tx[k], s.ty[k], s.tx[k] + 10, s.ty[k] + 10, false);
  }
  rect(b, s.x, s.y, s.x + 10, s.y + 10, true);
}

// Expanding rings. Cheap, hypnotic, and the one scene where the low frame rate is invisible
// because each ring is somewhere new every time.
inline void drawRings(uint8_t* b, uint32_t frame) {
  clear(b);
  for (int k = 0; k < 3; ++k) {
    const int r = static_cast<int>((frame * 3 + k * 11) % 33);
    if (r > 1) circle(b, 23, 23, r);
  }
  rect(b, 21, 21, 25, 25, false);
}

}  // namespace media
