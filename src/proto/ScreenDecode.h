// Reassembled bytes -> ScreenModel. One semantics, shared by the twins, the tests and any
// application sniffing another head unit.
//
// OFFSET ORIGIN — READ BEFORE TOUCHING A NUMBER HERE. Three origins are in circulation for
// the same fields (docs/PROTOCOL-NOTES.md §1.2):
//   PAYLOAD  offset 0 is the 0x10 first-frame byte      (payload = content + 2)
//   CONTENT  offset 0 is the screen command 0x21/0x74/… (content = tail + 6, confirm box)
//   TAIL     offset 0 is the first byte after a fixed header
// The reference decoder mixed two in one file (`OFF_SCROLL = 10` is a payload offset while
// the same field is content offset 8). Everything here is PAYLOAD origin, converted once
// at transcription; every constant states its origin and nothing downstream reconverts.
//
// THIS DECODER IS AN INDEPENDENT WITNESS. It exists to disagree with our encoder: a twin
// reporting a field one byte off from what a render call put there is a FINDING, not a
// calibration error. Do not move an offset to make a test pass. Every constant traces to
// docs/WIRE-SPEC.md, hence to a capture or to the third-party affa3.c.
#pragma once

#include "../AffaConfig.h"
#if AFFA_ENABLE_ISOTP_RX

#include "ScreenModel.h"
#include "../core/AffaTypes.h"

namespace affa {
namespace screen {

// ---------------------------------------------------------------------------
// Carminat windowed menu — command 0x21 mode 0x01. docs/WIRE-SPEC.md §8.5
// ---------------------------------------------------------------------------
// TWO LENGTHS, AND THE DIFFERENCE IS THE POINT. The builder always ends at exactly 96
// payload bytes, so that is what a self-ACK emulator or LoopbackLink receives (14 frames,
// last PCI 0x2D). A REAL PANEL terminates at the DECLARED FF_DL — payload[1] = 0x5A = 90
// content bytes, satisfied by 6 + 12*7, i.e. after frame 12, last PCI 0x2C — so it only
// ever holds payload[0..91] and payload[92..95] (the last four cells of row1) NEVER REACH
// IT. Same fact AffaConfig states as "the Carminat window row is 26 usable bytes".
// docs/WIRE-SPEC.md §8.5, §3.6.
//
// menu() therefore guards on 92, not 96, or a hardware-faithful twin could never decode a
// menu. kMenuMinLen stays 96 because docs/API.md §2.13 publishes that name and value as
// the BUILDER length, not the decode threshold.
constexpr uint8_t kMenuMinLen   = 96;  // PAYLOAD bytes our BUILDER emits (API.md §2.13)
constexpr uint8_t kMenuHwMinLen = 92;  // PAYLOAD bytes a REAL panel receives; menu()'s
                                       // actual guard = 2 + declared FF_DL 0x5A
constexpr uint8_t kMenuCmd     = 0x21;  // PAYLOAD [2]
constexpr uint8_t kMenuModeWin = 0x01;  // PAYLOAD [3] — 0x05 is the fullscreen variant
constexpr uint8_t kOffScroll   = 10;  // PAYLOAD (content 8)
constexpr uint8_t kOffHeader   = 11;  // PAYLOAD (content 9)  .. 36, 26 bytes
constexpr uint8_t kEndHeader   = 36;  // PAYLOAD (content 34)
constexpr uint8_t kOffRow0Mark = 38;  // PAYLOAD (content 36) — 0x7E
constexpr uint8_t kOffRow0     = 39;  // PAYLOAD (content 37) .. 63, 25 bytes
constexpr uint8_t kEndRow0     = 63;  // PAYLOAD (content 61)
constexpr uint8_t kOffRow1Mark = 65;  // PAYLOAD (content 63) — 0x7F
constexpr uint8_t kOffRow1     = 66;  // PAYLOAD (content 64) .. 95, 30 bytes
constexpr uint8_t kEndRow1     = 95;  // PAYLOAD (content 93)

// Every Carminat SCREEN — menu, now-playing box, notification — is a showMenu over 0x151,
// so this covers all of them. No-op if len < kMenuHwMinLen. Resets `sel`: a fresh screen
// clears the highlight, as the panel does. Fields extending past `len` come back short
// (row1 gives its 26 hardware-usable bytes at 92, all 30 at 96).
//
// Deliberately does NOT check payload[2]/[3] — the caller already selected this payload by
// its first frame, and docs/API.md §2.13 specifies a length-only guard. Use
// isMenuPayload() for the command guard.
void menu(const uint8_t* payload, uint8_t len, ScreenModel& out);

// True when the payload looks like a windowed-menu screen by COMMAND, not just length.
constexpr bool isMenuPayload(const uint8_t* p, uint8_t len) {
  return p && len >= 4 && p[0] == 0x10 && p[2] == kMenuCmd && p[3] == kMenuModeWin;
}

// ---------------------------------------------------------------------------
// Carminat window / full-window text — commands 0x74 and 0x77. WIRE-SPEC §8.1
// ---------------------------------------------------------------------------
//   PAYLOAD [0]=10 [1]=6+n [2]=74|77 [3]=rdsIcon [4]=55 [5]=srcIcon [6]=fmt [7]=01 [8..]=text
// 0x74 is the FULL window (the popup overlay), 0x77 the windowed radio-text line. Same
// 6-byte header, one decoder.
//
// Declared-length discrepancy that must not be "fixed": setText declares 0x0E (14) and
// transmits 20, and the panel renders only the first 8 text bytes. We decode what was
// TRANSMITTED; a test wanting "what the panel shows" truncates to 8 itself.
constexpr uint8_t kWinTextCmdFull   = 0x74;  // PAYLOAD [2] — full window / popup
constexpr uint8_t kWinTextCmdWindow = 0x77;  // PAYLOAD [2] — not-full window
constexpr uint8_t kOffWinIcon       = 3;     // PAYLOAD — RDS icon (0x45 AF-RDS, 0x55 none)
constexpr uint8_t kOffWinSrcIcon    = 5;     // PAYLOAD — source icon (0xFF none)
constexpr uint8_t kOffWinFmt        = 6;     // PAYLOAD — text format byte
constexpr uint8_t kOffWinText       = 8;     // PAYLOAD — first text cell
constexpr uint8_t kWinTextMinLen    = 9;     // header + at least one text byte

// Decodes into out.header and sets mode = Menu. Leaves row0/row1 alone: a windowed text is
// an OVERLAY and the screen underneath keeps what it had — the point of the 0x74/0x77
// window vs fullscreen distinction (PROTOCOL-NOTES §2.2). False if not a 0x74/0x77 text.
bool windowText(const uint8_t* payload, uint8_t len, ScreenModel& out);

// ---------------------------------------------------------------------------
// Carminat info-settings row — command 0x76. WIRE-SPEC §8.10
// ---------------------------------------------------------------------------
//   PAYLOAD [0]=10 [1]=0B [2]=76 [3]=prefix [4]=slot [5..12]=8 text bytes
// The three rows are THREE SEPARATE MESSAGES, so this is called three times and never sees
// all three at once, and nothing guarantees they stay adjacent on the wire.
constexpr uint8_t kInfoCmd     = 0x76;  // PAYLOAD [2]
constexpr uint8_t kInfoDeclLen = 0x0B;  // PAYLOAD [1] — 11 = 3 header + 8 text
constexpr uint8_t kOffInfoFmt  = 3;     // PAYLOAD — format byte (0x60 plain, OEM capture)
constexpr uint8_t kOffInfoSlot = 4;     // PAYLOAD — row slot: 0x41 / 0x44 / 0x48
constexpr uint8_t kOffInfoText = 5;     // PAYLOAD .. 12, 8 bytes
constexpr uint8_t kInfoMinLen  = 13;
// The three OEM row slots. Anything else appends at infoCount, so an application using
// its own slot codes still decodes in arrival order rather than being dropped.
constexpr uint8_t kInfoSlot0 = 0x41;
constexpr uint8_t kInfoSlot1 = 0x44;
constexpr uint8_t kInfoSlot2 = 0x48;

// Sets mode = Info and writes one row of out.info[]. False if not an info row. Both
// SPACE-padded (OEM capture) and NUL-padded (legacy builder) forms are in circulation;
// asciiz() trims either to the same string, which is why the oracle compares strings.
bool infoRow(const uint8_t* payload, uint8_t len, ScreenModel& out);

// ---------------------------------------------------------------------------
// UpdateList 8-segment setText — AFFA2 command 0x76. WIRE-SPEC §9.1
// ---------------------------------------------------------------------------
//   PAYLOAD [0]=10 [1]=19 [2]=76 [3]=chan [4]=loc [5..12]=old(8) [13]=10 [14..25]=new(12)
// `new` is the text the panel will show -> header; `old` -> row0.
constexpr uint8_t kSegCmd    = 0x76;  // PAYLOAD [2]
constexpr uint8_t kSegDecl   = 0x19;  // PAYLOAD [1] — 25, and CORRECT for once
constexpr uint8_t kSegMinLen = 26;
constexpr uint8_t kSegOld    = 5;     // PAYLOAD .. 12, 8 bytes
constexpr uint8_t kSegNew    = 14;    // PAYLOAD .. 25, 12 bytes
void segText(const uint8_t* payload, uint8_t len, ScreenModel& out);

// ---------------------------------------------------------------------------
// UpdateList text-plus-icons — AFFA2 command 0x7F. WIRE-SPEC §9.2, PROTOCOL-NOTES §2.2
// ---------------------------------------------------------------------------
//   PAYLOAD [0]=10 [1]=1C [2]=7F [3]=icons [4]=55 [5]=iconMode [6]=chan [7]=loc
//           [8..15]=old(8) [16]=10 [17..28]=new(12) [29]=00
//
// 0x76 versus 0x7F IS A STATEFUL DIFFERENTIAL ENCODING. 0x7F carries a three-byte icon
// header; 0x76 omits it and the PANEL KEEPS whatever the last 0x7F set, so a decoder that
// ignores this reports "no icons" for a screen that has them. The latch belongs to the
// twin (per-instance, invalidated on resync), not to this stateless function, which
// reports only what THIS message carried.
constexpr uint8_t kSegIconCmd     = 0x7F;  // PAYLOAD [2]
constexpr uint8_t kSegIconDecl    = 0x1C;  // PAYLOAD [1] — 28, also correct
constexpr uint8_t kSegIconMinLen  = 29;
constexpr uint8_t kOffSegIcons    = 3;     // PAYLOAD — icon bitmask
constexpr uint8_t kOffSegIconMode = 5;     // PAYLOAD — icon mode (0xFF = NONE)
constexpr uint8_t kSegIconOld     = 8;     // PAYLOAD .. 15, 8 bytes
constexpr uint8_t kSegIconNew     = 17;    // PAYLOAD .. 28, 12 bytes

// Same field meanings as segText(). `icons` / `iconMode` receive the latched-state bytes
// this message carries, for a caller that models the panel's icon latch; pass nullptr if
// you do not. Returns false if the payload is not a 0x7F text.
bool segTextIcons(const uint8_t* payload, uint8_t len, ScreenModel& out,
                  uint8_t* icons = nullptr, uint8_t* iconMode = nullptr);

// ---------------------------------------------------------------------------
// Single-frame control
// ---------------------------------------------------------------------------
// `07 29 01 <rowTag> 80 00 00 00` -> move the highlight bar. WIRE-SPEC §8.4.
// Sets out.sel to the ROW TAG (0x7E / 0x7F), which is what the wire carries.
constexpr uint8_t kHighlightSf   = 0x07;  // SF_DL = 7
constexpr uint8_t kHighlightCmd  = 0x29;
constexpr uint8_t kHighlightArg  = 0x01;
constexpr uint8_t kRowTag0       = 0x7E;
constexpr uint8_t kRowTag1       = 0x7F;
bool frame(const Frame& f, ScreenModel& out);

// Copy printable ASCII from payload[a..b] INCLUSIVE, stopping at the first NUL, then trim
// leading and trailing spaces. `dstSize` includes the NUL. Exposed because tests pin it.
//
// Non-printables are DROPPED, not substituted: panel filler (0xA3 on our bench unit, 0x84
// on an OEM cluster, 0xFF on the OEM radio) lands in these fields on short strings, and a
// substitution would put an artefact into an oracle meant to say what a human reads.
void asciiz(const uint8_t* payload, uint8_t len, uint8_t a, uint8_t b,
            char* dst, uint8_t dstSize);

}  // namespace screen
}  // namespace affa

#endif  // AFFA_ENABLE_ISOTP_RX
