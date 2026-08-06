// Carminat (AFFA3) family constants: identifiers, filler, sync profile, function table
// and the command / header bytes the frame builders emit.
//
// Every value carries its witness, in the vocabulary of core/AffaConstants.h:
//   [CAP] seen verbatim in MeganeCAN/logs/*.log, from OUR transmitter
//   [OEM] seen from a node that is not ours (the factory head unit)
//   [REF] notes/archive_mhroczny/affa3.{c,h}
//   [DERIVED] hand-executed from the builder, no capture
// The arithmetic and the quoted log lines are in docs/WIRE-SPEC.md §8; where this header
// and that document disagree, docs/API.md is the arbiter.
//
// NOTHING panel-agnostic belongs here — kReplyFlag, kAckDone, kRegisterByte and the
// ISO-TP counter live in core/AffaConstants.h and are shared with UpdateList.
#pragma once
#include "../AffaConfig.h"

#if AFFA_PANEL_CARMINAT

#include <cstdint>
#include "../core/AffaSyncProfile.h"

namespace affa {
namespace carminat {

// ---------------------------------------------------------------------------
// Identifiers
// ---------------------------------------------------------------------------

// We transmit the handshake here; the panel answers on kIdSyncReply. [CAP]
inline constexpr uint16_t kIdSync      = 0x3AF;
inline constexpr uint16_t kIdSyncReply = 0x3CF;   // shared with UpdateList

// The text/screen channel. setText, showMenu, highlightItem, the popups, the confirm box
// and the info rows ALL go out on this one id — which is why RenderSlot, and not the
// funcId, is what distinguishes them for coalescing. [CAP]
inline constexpr uint16_t kIdSetText     = 0x151;
inline constexpr uint16_t kIdDisplayCtrl = 0x151;   // same id, different command byte

// The second registered function. Nothing in this library transmits a payload on it; it
// exists because the panel expects BOTH functions registered, and the registration order
// {0x151, 0x1F1} is on the wire. [CAP] logs hold `0x1F1 { 70 00 00 00 00 00 00 00 }`.
inline constexpr uint16_t kIdNav = 0x1F1;

// Panel -> radio key frames arrive here, and pressKey(..., KeySource::Wire) transmits
// here. It is NOT a key-only channel: the corpus also holds `70 A3..`, `02 64 0F A3..`
// and `05 63 "0037"` on it, which is what makes the `03 89` guard load-bearing. [CAP]
inline constexpr uint16_t kIdKeyPressed = 0x1C1;

// Every frame WE build pads with this. It is not a protocol constant — it is per-node,
// and it means nothing on receive: the bench panel pads 0xA3, an OEM cluster 0x84, the
// OEM radio 0xFF. Never match on a received filler byte. [CAP]
inline constexpr uint8_t kFiller = 0x00;

// ---------------------------------------------------------------------------
// Sync profile
// ---------------------------------------------------------------------------

// Sent after the authorized panel `61 11 00` request on 0x3CF, in this order. `01` is
// only the bootstrap / START phase and receives the bounded B9 + BA control pair; it
// never starts this announce burst. The second and third
// frames are IDENTICAL — two sendCan calls in the legacy source, and present in the
// capture. It is not a typo and it is not deduplicated. [CAP]
inline constexpr uint8_t kHello[3][8] = {
    {0xB0, 0x14, 0x11, 0x00, 0x1F, 0x00, 0x00, 0x00},
    {0xB0, 0x14, 0x11, 0x00, 0x1F, 0x00, 0x00, 0x00},
    {0xB0, 0x14, 0x11, 0x00, 0x1F, 0x00, 0x00, 0x00},
};

// Historical MeganeCAN spelling. It remains named evidence rather than silently mixed
// into the captured profile above: `70/B0/B0` is a compatibility variant, not the AFFA3
// monitor sequence selected by kSync.
inline constexpr uint8_t kLegacyHello[3][8] = {
    {0x70, 0x1A, 0x11, 0x00, 0x00, 0x00, 0x00, 0x01},
    {0xB0, 0x14, 0x11, 0x00, 0x1F, 0x00, 0x00, 0x00},
    {0xB0, 0x14, 0x11, 0x00, 0x1F, 0x00, 0x00, 0x00},
};

// AFFA3 NAV repeats its full request at roughly 106 ms while waking. MeganeCAN's working
// handler answered that cadence, so this preserves the measured exchange without allowing
// a pathological line-rate retry storm to turn into an unbounded transmit flood.
inline constexpr uint32_t kHelloMinMs = 90;
inline constexpr uint32_t kHelloFirstDelayMs = 31;
inline constexpr uint32_t kHelloFrameGapMs = 31;
inline constexpr uint32_t kPayloadAfterRegistrationMs = 400;
inline constexpr uint32_t kSyncIntervalMs = 500;

// The monitor corpus and the historical MeganeCAN implementation are both real, but they
// disagree on the first three capability frames.  Make the distinction an explicit caller
// choice rather than guessing from timing or silently mixing bytes from two radios.
enum class CarminatHelloProfile : uint8_t {
  CapturedB0x3,          // current default: B0/B0/B0, paced at 31 ms on the wire
  MeganeCanLegacy70B0B0, // historical source: immediate 70/B0/B0 compatibility sequence
};

// requestArg is 0x00 AND MUST STAY SO. Carminat's `BA 00 00 …` is 0xBA followed by seven
// filler bytes that merely happen to be zero; UpdateList's `7A 01` carries a genuine
// argument. The two look symmetrical on the wire and are not. [CAP]
//
// replyToPing is FALSE for this family, and the four OEM captures are why. The real radio's
// B9 is a free-running 500.08 ms timer (sigma <= 0.5 ms) that never flinches as the display's
// 504/512 ms `69` drifts past it through a full phase cycle — including one 0.023 ms
// near-collision. It is categorically not a reply. With replyToPing on we emitted the paced
// B9 AND a pong ~4 ms later, twice the OEM rate. docs/PROTOCOL.md §3.
//
// SIX FIELDS SHORTER THAN IT WAS, and not one wire byte different. `authRequestByte2`,
// `helloAfterBootstrapRequest`, `helloOnNonAuthRequest`, `oneShotResyncOnStart`,
// `oneShotResyncOnPeerAlive` and `bootstrapAliveFrame` all held settled measurements and are
// now rules in AffaDisplayBase — see the header of AffaSyncProfile.h for which fact went
// where. What this profile still carries is identity, timing, and the four questions the two
// families genuinely answer differently.
//
//   syncId, syncReplyId, replyFlag, alive, request, requestArg, filler, hello, helloCount,
//   replyToPing, waitForPanel, sendSyncRequest, requireAuthRequest, helloMinMs,
//   helloFirstDelayMs, helloFrameGapMs, payloadAfterRegistrationMs, syncIntervalMs,
//   registerAfterHello, announceWhenSilentMs, helloRequiresAnnounce
inline constexpr SyncProfile kSync = {
    kIdSync, kIdSyncReply, 0x0400, 0xB9, 0xBA, 0x00, kFiller, kHello, 3,
    false, // B9 free-runs at 500 ms. The panel's 69 is an independent timer, not a request.
    true,  // Do not speak until the AFFA3 NAV panel speaks first.
    false, // Never spray BA probes as periodic recovery.
    true,  // A bare 69 is only a ping; only a full 61 11 xx starts auth/registration.
    kHelloMinMs,
    kHelloFirstDelayMs,
    kHelloFrameGapMs,
    kPayloadAfterRegistrationMs,
    kSyncIntervalMs,
    true,  // 151 70 / 1F1 70 follow the third B0 by 0.10-0.59 ms, with no application
           // involvement. Registration belongs to the opening on this family.
    30000, // Announce BA every 30 s into a SILENT bus. waitForPanel above is a deadlock
           // on its own: a display that has gone to sleep never speaks, so neither do we
           // (measured rx 0 / tx 0). The OEM radio initiates after a reattach.
    true,  // BA FIRST; the NEXT 61 11 draws the burst. 4/4 in the captures, and proven on
           // hardware 2026-08-04 — without it the panel never opens its own 1C1 channel.
};

// Historical MeganeCAN compatibility profile.  It deliberately keeps the user's strict
// `61 11 00` authorization gate and the bounded one-shot 01 -> B9/BA discovery path; only
// the wire spelling/timing of the three hello frames differs.  The zero gaps reproduce the
// proven old sender's three immediate `sendCan()` calls exactly.  Use it only for a panel
// that has demonstrated the legacy opening; `kSync` above is the capture-backed default.
inline constexpr SyncProfile kLegacyMeganeCanSync = {
    kIdSync, kIdSyncReply, 0x0400, 0xB9, 0xBA, 0x00, kFiller, kLegacyHello, 3,
    true,  // replyToPing — the ONE wire difference this profile still keeps
    true,  // waitForPanel
    false, // no periodic BA recovery
    true,  // same panel-led opening as kSync
    kHelloMinMs,
    0,     // old source emitted its first 70 in the receive pass
    0,     // old source emitted the two B0 frames immediately after it
    kPayloadAfterRegistrationMs,
    kSyncIntervalMs,
    true,  // same panel, same unconditional registration after the final hello frame
    30000, // same panel, same need to break a two-way silence
    true,  // same panel, same BA-before-hello rule
};

inline constexpr const SyncProfile& syncProfile(CarminatHelloProfile profile) {
  return profile == CarminatHelloProfile::MeganeCanLegacy70B0B0 ? kLegacyMeganeCanSync : kSync;
}

// ORDER IS ON THE WIRE: the first send after a resync walks this table in declaration
// order and puts a 1-byte 0x70 on each id before the payload. [CAP]
inline constexpr uint16_t kFuncIds[]  = {kIdSetText, kIdNav};
inline constexpr uint8_t  kFuncCount  = 2;

// ---------------------------------------------------------------------------
// Screen / text command bytes
// ---------------------------------------------------------------------------

// Byte 0 of a multi-frame payload. The transport does NOT add it — the builder does, and
// the transport then carries eight raw bytes in frame 0. Same numeric value as the text
// field separator inside a 0x76 payload, different layer; see PROTOCOL-NOTES §4.4.
inline constexpr uint8_t kPciFirstFrame = 0x10;

inline constexpr uint8_t kCmdText    = 0x77;  // setText, windowed        [CAP]
inline constexpr uint8_t kCmdPopup   = 0x74;  // showPopupText overlay    [CAP]
inline constexpr uint8_t kCmdScreen  = 0x21;  // showMenu / fullscreen / confirm box [CAP]
inline constexpr uint8_t kCmdInfoRow = 0x76;  // showInfoPopup row        [CAP]
inline constexpr uint8_t kCmdCtrl    = 0x52;  // setPower (legacy setState) [CAP]
inline constexpr uint8_t kCmdClock   = 0x56;  // 'V' — setTime            [CAP]
inline constexpr uint8_t kCmdHilite  = 0x29;  // highlightItem            [CAP]
inline constexpr uint8_t kCmdClose   = 0x54;  // hidePopup / hideFullscreenText [CAP][OEM]

// Screen modes, byte 3 of a 0x21 payload.
inline constexpr uint8_t kScreenWindowed   = 0x01;  // the two-row menu window [CAP]
inline constexpr uint8_t kScreenFullscreen = 0x05;  // whole glass             [OEM]
// The navigation pane, on kIdNav rather than kIdSetText — and the reason the 0x1F1 header
// opens `21 0B`: it is the SAME kCmdScreen with its own screen type. [OEM][BENCH]
inline constexpr uint8_t kScreenNav        = 0x0B;

// ---------------------------------------------------------------------------
// The navigation bitmap
// ---------------------------------------------------------------------------
// 48x48 monochrome, row-major, 6 bytes per row, MSB-first: bit 7 of byte 0 is the top-left
// pixel. Decoded from the OEM head unit's own 302-byte 0x1F1 transfer and CONFIRMED ON THE
// BENCH PANEL 2026-08-05 — it renders, and it renders the right way up.
//
// IT IS AN INDEPENDENT LAYER, not a screen mode. The info menu draws WITH it on one screen,
// and the bitmap can be replaced underneath an open popup and is seen to change. That is
// what makes it worth an API: it is a persistent widget under whatever else is drawn.
inline constexpr uint16_t kNavWidth       = 48;
inline constexpr uint16_t kNavHeight      = 48;
inline constexpr uint16_t kNavStride      = 6;                       // ceil(48/8)
inline constexpr uint16_t kNavBitmapBytes = kNavStride * kNavHeight; // 288

// The OEM's 14 header bytes, VERBATIM, because verbatim is what is proven to render.
//
//   21    kCmdScreen
//   0B    kScreenNav
//   00 25 unknown; possibly one 16-bit field
//   41..46 00  a seven-byte slot holding "ABCDEF\0" in the capture. CONFIRMED NOT TO BE
//              PLAIN TEXT: ASCII written here puts nothing on the glass. Left as captured
//              rather than zeroed, because what renders is what was captured and no
//              variation of it has been tested.
//   01    unknown; format or bit depth
//   30 30 48, 48 — the geometry, and the two bytes that agree with kNavBitmapBytes
inline constexpr uint8_t kNavHeader[14] = {
  kCmdScreen, kScreenNav, 0x00, 0x25,
  0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x00,
  0x01,
  static_cast<uint8_t>(kNavWidth), static_cast<uint8_t>(kNavHeight)
};

// setText / showPopupText header bytes.
//
// THE ICON BYTE IS A BITMASK AND ITS POLARITY IS INVERTED: A SET BIT TURNS AN ICON OFF.
// That is why "no icons" is 0x55 and not 0x00 — it is NoNews|NoTraffic|NoAfRds|NoMode.
// [REF] notes/archive_mhroczny/affa3.h:39-51 names every bit; affa3.c:320 builds exactly
// this 0x55 out of the four NO_ bits.
//
// CONFIRMED ON THE WIRE across 13 OEM `0x77` frames: the radio sends 0x05 and 0x09 on FM
// (bit 4 CLEAR — AF-RDS lit) and 0x15 / 0x55 on MW, LW and AUX (bit 4 SET — AF-RDS dark).
// The bit tracks the band, which is what an inverted mask predicts. [OEM]
inline constexpr uint8_t kIconNoNews      = 1u << 0;  // 0x01
inline constexpr uint8_t kIconNewsArrow   = 1u << 1;  // 0x02
inline constexpr uint8_t kIconNoTraffic   = 1u << 2;  // 0x04
inline constexpr uint8_t kIconTrafficArrow= 1u << 3;  // 0x08
inline constexpr uint8_t kIconNoAfRds     = 1u << 4;  // 0x10
inline constexpr uint8_t kIconAfRdsArrow  = 1u << 5;  // 0x20
inline constexpr uint8_t kIconNoMode      = 1u << 6;  // 0x40
// Bit 7 is UNNAMED IN THE ORIGIN AND CLEAR IN EVERY OBSERVED VALUE (0x05, 0x09, 0x15,
// 0x55). It is the only bit left for a glyph the origin never named, so it is the first
// thing to try against an icon that will not go out — but nothing states that, so it is
// spelled "unknown" rather than given a name it has not earned.
inline constexpr uint8_t kIconBit7Unknown = 1u << 7;  // 0x80

inline constexpr uint8_t kIconsNone   = 0x55;  // NoNews|NoTraffic|NoAfRds|NoMode [CAP][REF]
inline constexpr uint8_t kIconsAfRds  = 0x45;  // 0x55 minus NoAfRds — AF-RDS lit  [OEM]
// NOT A SECOND MASK, AND NOT THE PLACE TO HUNT A STUCK ICON: the OEM radio sends 0x55 here
// in 13 of 13 captured frames, across FM, MW, LW and AUX, and the origin hard-codes it in
// both of its senders. It is a genuine constant whose meaning is unknown. [OEM][REF]
inline constexpr uint8_t kIconBank2   = 0x55;
inline constexpr uint8_t kSrcIconNone = 0xFF;  // 0xDF "MANU", 0xFD "PRESET"      [CAP]
inline constexpr uint8_t kFormatPlain = 0x60;  // 0x19-0x3F radio style, 0x59-0x7F ASCII
inline constexpr uint8_t kControlByte = 0x01;  // always 0x01                     [CAP]
inline constexpr uint8_t kPopupIcon   = 0x09;  // the OEM volume popup's left icon [CAP]

// ---------------------------------------------------------------------------
// The message box (mode 0x05), fully resolved from five OEM captures
// ---------------------------------------------------------------------------
// Content offsets below are into the payload AFTER the two PCI bytes, i.e. content[0] is
// the 0x21 command. The layout is:
//
//   [0] 21  [1] 05  [2] selected button (0xFF when there are none)  [3] 00
//   [4] BUTTON COUNT  [5] 49  [6..31] zero
//   [32 ..] buttonCount x 6-byte NUL-padded labels, then the body, 0x0D-separated
//
// and the two lengths fall straight out of the count, three-for-three across captures with
// 0, 1 and 2 buttons:
//
//   declared length = 105 + 6 * buttonCount    -> 105 / 111 / 117
//   body offset     =  32 + 6 * buttonCount    ->  32 /  38 /  44
//
// [OEM] `navi start.csv` (0), `AVAILABLE SPACE BIG SCREEN ONE CONFIRM.csv` (1, labelled
// "OK"), `CONFIRM SCREEN NO.csv` (2, labelled "Yes"/"No"). The body region is always 73
// bytes, because both offsets move together.
//
// The builder placed both fields correctly all along — its offsets were relative to the
// end of the 6-byte header, so its 0x1A and 0x20 were payload 32 and 38. What it could not
// do was VARY payload[4]: the count was hard-coded to 1, so the zero-button screen and the
// two-button Yes/No box were unreachable however the caller spelled the call.
inline constexpr uint8_t kButtonsNone  = 0x00;
inline constexpr uint8_t kButtonsOk    = 0x01;
inline constexpr uint8_t kButtonsYesNo = 0x02;
inline constexpr uint8_t kButtonsMax   = 2;     // no capture has ever shown a third
inline constexpr uint8_t kButtonLabelLen  = 6;
inline constexpr uint8_t kBoxNoSelection  = 0xFF;  // content[2] when buttonCount is 0
inline constexpr uint8_t kBoxHeaderByte5  = 0x49;  // constant in all five captures
inline constexpr uint8_t kBoxLabelOffset  = 32;    // content offset of label 0
inline constexpr uint8_t kBoxBodyBytes    = 73;    // 105 - 32, whatever the button count
inline constexpr uint8_t kBoxBaseLength   = 105;   // declared length with no buttons
// Selection inside a message box moves with `03 29 05 <index>` — a THREE-BYTE SINGLE FRAME,
// mode 0x05, not the 0x01 the list screen uses and not the 7-byte `29 01 <rowtag> 80` form.
// [OEM] `CONFIRM SCREEN SWITCH TO YES.csv`, answered with a bare 0x74.
inline constexpr uint8_t kSelectModeBox  = 0x05;
inline constexpr uint8_t kSelectModeList = 0x01;

// Row tags. The SAME bytes appear in the highlight frame and at showMenu payload offsets
// 4, 38 and 65 — they are row identifiers, not magic numbers. [CAP]
inline constexpr uint8_t kRowTagTop    = 0x7E;
inline constexpr uint8_t kRowTagBottom = 0x7F;

// setPower states. Carminat enables with 0x09, UpdateList with 0x02 — do not unify. [CAP]
inline constexpr uint8_t kDisplayCtrlOff = 0x00;
inline constexpr uint8_t kDisplayCtrlOn  = 0x09;

// Row separator inside the fullscreen and confirm-box text blocks. [OEM]
inline constexpr uint8_t kLineSeparator = 0x0D;

// showInfoPopup row slots and format prefix, byte-for-byte from the OEM settings list
// (`76 60 41 .. AUX`, `76 60 44 .. AUTO`, `76 60 48 .. SPEED`). [OEM]
inline constexpr uint8_t kInfoOffset0 = 0x41;
inline constexpr uint8_t kInfoOffset1 = 0x44;
inline constexpr uint8_t kInfoOffset2 = 0x48;
inline constexpr uint8_t kInfoPrefix  = 0x60;

// ---------------------------------------------------------------------------
// Field capacities, as the panel renders them
// ---------------------------------------------------------------------------

inline constexpr uint8_t kTextCells      = 14;  // setText carries 14, panel shows ~8
inline constexpr uint8_t kPopupCellsMin  = 8;   // keeps the captured "VOL 28" identical
inline constexpr uint8_t kPopupCellsMax  = 16;
inline constexpr uint8_t kInfoCells      = 8;
inline constexpr uint8_t kMenuHeaderMax  = 26;

// The N-item list screen. Stride and cell width are MEASURED, not inferred: the six-item
// Navigation menu's tag bytes land at 36, 63, 90, 117, 144, 171 reading 00..05. [OEM]
inline constexpr uint8_t  kMenuItemStride = 27;   // 1 tag byte + 26 text bytes
inline constexpr uint8_t  kMenuItemTextMax = 26;
inline constexpr uint8_t  kMenuFixedBytes = 36;   // header block before item 0
// SIX IS THE MOST EVER CAPTURED, NOT A KNOWN CEILING. The builder allows more so the real
// limit can be found on glass; anything above six is unverified.
inline constexpr uint8_t  kMenuMaxItems   = 10;
inline constexpr uint8_t kConfirmCapMax  = 7;   // 6 is the safe maximum — see the note in
                                                // CarminatDisplay::showConfirmBox
inline constexpr uint8_t kClockDigits    = 4;   // "HHMM"

// Scroll-arrow indicator, byte 10 of the menu screen. Derived from the window's position
// in the list by widget::MenuModel::scrollMask(), whose four return values ARE these bytes
// (which is why CarminatMenuRenderer passes the mask straight through); exposed because
// showMenu() takes it.
//
// kScrollBoth WAS 0x0C UNTIL 2026-08-06, AND 0x0C IS NOT ON THE WIRE. It came from
// MeganeCAN's hand-written `SCROLL_BOTH = 0x0C` enum and appears in ZERO captures. What the
// OEM actually sends for a list with both arrows is 0x03, in three independent captures —
// the 6-item `mENU NAVIGATION` and the 4-item Settings list, twice. MeganeCAN's own
// hardware fixture carries 0x0B, never 0x0C.
//
// The bits then decompose cleanly, which is the reading that makes all seven observed
// frames consistent: 0x03 is the base "arrows" value and the two high bits SUPPRESS one
// each — 0x08 the up arrow (0x03|0x08 = 0x0B, captured at the TOP of a list, where up is
// impossible) and 0x04 the down arrow (0x03|0x04 = 0x07, captured at the BOTTOM). By that
// reading 0x0C = 0x08|0x04 suppresses both and is the exact OPPOSITE of what it was named.
// The decomposition is inference; the three 0x03 captures are not.
//
// NOTE FOR ANYONE TESTING THIS ON A TWO-ROW showMenu AND SEEING NOTHING: a 2-item window
// ([6] = 0x82) has no overflow, so the panel draws no arrows whatever this byte says. Every
// capture that shows an arrow is a 4- or 6-item list. Use showMenuN() to see it work.
enum ScrollIndicator : uint8_t {
  kScrollNone = 0x00,   // no arrows
  kScrollUp   = 0x07,   // top arrow only    [OEM] bottom of a 2-item window
  kScrollDown = 0x0B,   // bottom arrow only [OEM] top of a 2-item window
  kScrollBoth = 0x03,   // both              [OEM] 4-item and 6-item lists
};

}  // namespace carminat
}  // namespace affa

#endif  // AFFA_PANEL_CARMINAT
