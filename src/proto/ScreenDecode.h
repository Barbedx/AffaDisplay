// Reassembled bytes -> ScreenModel. One semantics, shared by the twins, the tests and any
// application that wants to sniff another head unit.
//
// ------------------------------------------------------------------------------------
// OFFSET ORIGIN. READ THIS BEFORE TOUCHING A NUMBER IN THIS FILE.
// ------------------------------------------------------------------------------------
// Three origins are in circulation for the same fields (docs/PROTOCOL-NOTES.md §1.2):
//
//   PAYLOAD  offset 0 is the 0x10 first-frame byte      (payload = content + 2)
//   CONTENT  offset 0 is the screen command 0x21/0x74/… (content = tail + 6, confirm box)
//   TAIL     offset 0 is the first byte after a fixed header
//
// The reference decoder mixed two of them in ONE FILE — `ScreenDecode::OFF_SCROLL = 10` is
// a payload offset while the same field is content offset 8. Everything below is stated in
// PAYLOAD origin, converted exactly once, here, at transcription; every constant carries
// its origin in its comment and nothing downstream converts again.
//
// ------------------------------------------------------------------------------------
// THIS DECODER IS AN INDEPENDENT WITNESS.
// ------------------------------------------------------------------------------------
// It exists to disagree with our encoder. If a twin built on it reports a field one byte
// off from what a render call put there, that is a FINDING, not a calibration error — do
// not move an offset here to make a test pass. Every constant below is traceable to
// docs/WIRE-SPEC.md, which is traceable to a capture or to the third-party affa3.c.
#pragma once

#include "../AffaConfig.h"
#if (AFFA_ENABLE_VIRTUAL_PANEL || AFFA_ENABLE_ISOTP_RX)

#include "ScreenModel.h"
#include "../core/AffaTypes.h"

namespace affa {
namespace screen {

// ---------------------------------------------------------------------------
// Carminat windowed menu — command 0x21 mode 0x01. docs/WIRE-SPEC.md §8.5
// ---------------------------------------------------------------------------
// TWO LENGTHS, AND THE DIFFERENCE IS THE WHOLE POINT.
//
// The builder always ends at exactly 96 payload bytes regardless of string lengths, so 96
// is what our encoder emits and what a self-ACK emulator or LoopbackLink receives (14
// frames, last PCI 0x2D). A REAL PANEL terminates the transfer at the DECLARED FF_DL —
// payload[1] = 0x5A = 90 content bytes — which is satisfied by 6 + 12*7, i.e. after frame
// 12, last PCI 0x2C. It therefore only ever holds payload[0..91] = 92 bytes, and
// payload[92..95] (the last four cells of row1) NEVER REACH IT. That is the same fact
// AffaConfig records from the other side as "the Carminat window row is 26 usable bytes".
// docs/WIRE-SPEC.md §8.5, §3.6.
//
// So menu() guards on 92, not 96, or a hardware-faithful twin could never decode a menu at
// all. kMenuMinLen is kept at 96 because docs/API.md §2.13 publishes that name and value —
// it is the BUILDER length, not the decode threshold. See openIssues: API.md's
// "no-op if len < kMenuMinLen" is written against the emulator, not against hardware.
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
// so this one function covers all of them. No-op if len < kMenuHwMinLen. Resets `sel`: a
// fresh screen clears the highlight, exactly as the panel does.
//
// Fields that extend past `len` are simply short: row1 decodes to its 26 hardware-usable
// bytes at 92 and to all 30 at 96, which is the truthful answer in both cases.
//
// Deliberately does NOT check payload[2]/[3] against kMenuCmd/kMenuModeWin — the caller
// has already selected this payload by its first frame, and a length-only guard is what
// docs/API.md §2.13 specifies. Use isMenuPayload() if you want the command guard.
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
// 6-byte header, so one decoder.
//
// NOTE the declared-length discrepancy that must not be "fixed": setText declares 0x0E
// (14) and transmits 20, and the panel renders only the first 8 text bytes. We decode
// what was TRANSMITTED, because that is what a byte-level oracle is for; a test that wants
// "what the panel shows" truncates to 8 itself.
constexpr uint8_t kWinTextCmdFull   = 0x74;  // PAYLOAD [2] — full window / popup
constexpr uint8_t kWinTextCmdWindow = 0x77;  // PAYLOAD [2] — not-full window
constexpr uint8_t kOffWinIcon       = 3;     // PAYLOAD — RDS icon (0x45 AF-RDS, 0x55 none)
constexpr uint8_t kOffWinSrcIcon    = 5;     // PAYLOAD — source icon (0xFF none)
constexpr uint8_t kOffWinFmt        = 6;     // PAYLOAD — text format byte
constexpr uint8_t kOffWinText       = 8;     // PAYLOAD — first text cell
constexpr uint8_t kWinTextMinLen    = 9;     // header + at least one text byte

// Decodes into out.header (the text) and sets mode = Menu. Leaves row0/row1 alone: a
// windowed text is an OVERLAY and the screen underneath keeps whatever it had — that is
// the whole point of the 0x74/0x77 window vs fullscreen distinction (PROTOCOL-NOTES §2.2).
// Returns false if the payload is not a 0x74/0x77 text.
bool windowText(const uint8_t* payload, uint8_t len, ScreenModel& out);

// ---------------------------------------------------------------------------
// Carminat info-settings row — command 0x76. WIRE-SPEC §8.10
// ---------------------------------------------------------------------------
//   PAYLOAD [0]=10 [1]=0B [2]=76 [3]=prefix [4]=slot [5..12]=8 text bytes
// The three rows are THREE SEPARATE MESSAGES, one per row, so this is called three times
// and never sees all three at once. (The legacy delay(5) between them is gone, so nothing
// guarantees the rows stay adjacent on the wire — finding #10.)
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

// Sets mode = Info and writes one row of out.info[]. Returns false if not an info row.
// The OEM capture is SPACE-padded; the legacy builder's `char padded[8] = {' '}`
// initialises only element 0 and NUL-pads the rest (finding #9), so both forms are in
// circulation — asciiz() trims either to the same string, which is exactly why the oracle
// compares strings and not bytes.
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
// 0x76 versus 0x7F IS A DIFFERENTIAL ENCODING AND IT IS STATEFUL. 0x7F carries a
// three-byte icon header; 0x76 omits it and the PANEL KEEPS whatever the last 0x7F set.
// A decoder that ignores this reports "no icons" for a screen that has them. The latched
// state therefore belongs to the twin (per-instance, invalidated on resync) and NOT to
// this stateless function, which only reports what THIS message carried.
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
// leading and trailing spaces. Non-printable bytes are dropped, not substituted.
// `dstSize` includes room for the NUL. Exposed because the tests pin it directly.
//
// Dropping non-printables rather than substituting is deliberate: the panel's own filler
// (0xA3 on our bench unit, 0x84 on an OEM cluster, 0xFF on the OEM radio) lands in these
// fields on short strings, and a substitution would put a visible artefact into an oracle
// that is supposed to say what a human reads off the glass.
void asciiz(const uint8_t* payload, uint8_t len, uint8_t a, uint8_t b,
            char* dst, uint8_t dstSize);

}  // namespace screen
}  // namespace affa

#endif  // AFFA_ENABLE_VIRTUAL_PANEL || AFFA_ENABLE_ISOTP_RX
