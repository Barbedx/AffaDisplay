// A DECODER THAT DISAGREES WITH THE ENCODER — the test oracle, and nothing else.
//
// This used to be src/proto/ (IsoTp + ScreenDecode + ScreenModel, 509 lines), and it was
// deleted from the library because its RUNTIME half had no consumers: AFFA_ENABLE_ISOTP_RX
// is 0 on every target, so `onText` was never compiled into a single firmware, and nothing
// ever installed it. What it was genuinely being used for is this — reassembling what the
// library TRANSMITTED and reading it back as a screen.
//
// SO IT LIVES HERE NOW, where its only callers are. That is the same move that retired
// CarminatVirtualPanel: a panel twin shipped as library surface, replaced by thirty lines of
// decoder in the suite that needed it.
//
// THE POINT IS THE DISAGREEMENT. This is written from docs/WIRE-SPEC.md, not from the
// builders, and it shares no code with them. A test that asserted the builder's output using
// the builder's own offsets would pass with both of them wrong in the same direction; this
// one has to be wrong in the SAME way, independently, which is the property worth the
// duplication.
#pragma once
#include <unity.h>
#include <cstdint>
#include <cstring>

#include "AffaConfig.h"
#include "core/AffaTypes.h"

namespace affadec {

// ---------------------------------------------------------------------------
// ISO-TP reassembly
// ---------------------------------------------------------------------------
// The PCI byte is KEPT as payload byte 0, because every offset below is measured from it —
// docs/WIRE-SPEC.md counts that way and so does the OEM corpus.
class Reassembler {
 public:
  // True when the frame was consumed as an ISO-TP data frame.
  bool onFrame(const affa::Frame& f) {
    if (f.len == 0) return false;

    // THE NIBBLE, NOT THE WHOLE BYTE. ISO 15765-2 puts a 12-bit length in a first frame:
    // the high nibble is 1, the low nibble is the top four bits of FF_DL. Everything our own
    // drivers build is short enough for 0x10, which is why matching the whole byte survived
    // for a long time — but the OEM's 302-byte 0x1F1 screen opens `11 2E 21 0B ...`, and
    // under the stricter test that first frame was refused, `_active` stayed false, and all
    // 43 continuations were refused with it. The message decoded to nothing at all.
    //
    // Safe because a single frame's PCI is its length, and every single-frame payload in
    // either family is <= 7 bytes, so byte 0 is never in 0x11..0x1F.
    if ((f.data[0] & 0xF0) == 0x10) {
      _len    = 0;
      _active = true;
      for (uint8_t i = 0; i < 8 && i < f.len && _len < AFFA_MAX_PAYLOAD; ++i)
        _buf[_len++] = f.data[i];
      return true;
    }

    // A continuation appends data[1..7]. The counter in data[0] is deliberately NOT checked
    // — it wraps every sixteen frames by design. `_active` is checked: a continuation with
    // no first frame in front of it belongs to some other transfer.
    if ((f.data[0] & 0xF0) == 0x20 && _active) {
      for (uint8_t i = 1; i < 8 && i < f.len && _len < AFFA_MAX_PAYLOAD; ++i)
        _buf[_len++] = f.data[i];
      return true;
    }
    return false;
  }

  const uint8_t* buffer() const { return _buf; }
  uint8_t        len()    const { return _len; }
  bool           active() const { return _active; }
  void           reset()        { _len = 0; _active = false; }

 private:
  uint8_t _buf[AFFA_MAX_PAYLOAD] = {0};
  uint8_t _len    = 0;
  bool    _active = false;
};

// How many CAN frames `len` payload bytes take: 8 in the first, 7 per continuation.
constexpr uint8_t frameCount(uint8_t len) {
  return len <= 8 ? 1 : static_cast<uint8_t>(1 + (len - 8 + 6) / 7);
}

// Split `payload` into frames the way a sender must. Returns how many were written.
inline uint8_t fragment(uint16_t id, const uint8_t* payload, uint8_t len, uint8_t filler,
                        affa::Frame* out, uint8_t cap) {
  if (!payload || len == 0 || !out) return 0;
  const uint8_t want = frameCount(len);
  if (want > cap) return 0;

  uint8_t sent = 0, n = 0;
  while (sent < len && n < want) {
    affa::Frame f;
    f.id  = id;
    f.len = 8;
    std::memset(f.data, filler, 8);
    uint8_t at = 0;
    if (n > 0) f.data[at++] = static_cast<uint8_t>(0x20 | (n & 0x0F));
    while (at < 8 && sent < len) f.data[at++] = payload[sent++];
    out[n++] = f;
  }
  return n;
}

// ---------------------------------------------------------------------------
// The two-row menu screen — docs/WIRE-SPEC.md §8.5
// ---------------------------------------------------------------------------
// Offsets are from payload byte 0, which is the PCI. Transcribed from the spec, not from
// CarminatDisplay's builder.
constexpr uint8_t kMenuCmd      = 0x21;   // payload[2]
constexpr uint8_t kMenuModeWin  = 0x01;   // payload[3] of a WINDOWED menu
constexpr uint8_t kHighlightSf  = 0x07;   // a standalone highlight is a single frame
constexpr uint8_t kHighlightCmd = 0x29;
constexpr uint8_t kHighlightArg = 0x01;

// A REAL PANEL STOPS AT THE DECLARED FF_DL and never receives the final four bytes, so the
// oracle must accept 92 rather than the 96 the builder writes. Asserting 96 here would fail
// against hardware and pass against the self-ACK emulator — the exact wrong way round.
constexpr uint8_t kMenuHwMinLen = 92;

constexpr uint8_t kOffScroll   = 10;
constexpr uint8_t kOffHeader   = 11, kEndHeader   = 37;
constexpr uint8_t kOffRow0Mark = 37, kOffRow0     = 38, kEndRow0 = 64;
constexpr uint8_t kOffRow1Mark = 64, kOffRow1     = 65, kEndRow1 = 91;

struct ScreenModel {
  char    header[32] = {0};
  char    row0[32]   = {0};
  char    row1[32]   = {0};
  uint8_t scroll     = 0;
  uint8_t row0Id     = 0;
  uint8_t row1Id     = 0;
};

// Copy [from, to) as a NUL-terminated string, stopping at the first NUL and trimming the
// trailing spaces the panel pads rows with.
inline void asciiz(const uint8_t* p, uint8_t len, uint8_t from, uint8_t to, char* out,
                   size_t cap) {
  size_t n = 0;
  for (uint8_t i = from; i < to && i < len && n + 1 < cap; ++i) {
    if (p[i] == 0x00) break;
    out[n++] = static_cast<char>(p[i]);
  }
  while (n && out[n - 1] == ' ') --n;
  out[n] = '\0';
}

inline void menu(const uint8_t* payload, uint8_t len, ScreenModel& m) {
  if (!payload || len < kMenuHwMinLen) return;
  m.scroll = payload[kOffScroll];
  asciiz(payload, len, kOffHeader, kEndHeader, m.header, sizeof(m.header));
  m.row0Id = payload[kOffRow0Mark];
  asciiz(payload, len, kOffRow0, kEndRow0, m.row0, sizeof(m.row0));
  m.row1Id = payload[kOffRow1Mark];
  asciiz(payload, len, kOffRow1, kEndRow1, m.row1, sizeof(m.row1));
}

}  // namespace affadec
