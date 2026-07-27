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

// Sent in reply to the panel's `61 11 ..` on 0x3CF, in this order. The second and third
// frames are IDENTICAL — two sendCan calls in the legacy source, and present in the
// capture. It is not a typo and it is not deduplicated. [CAP]
inline constexpr uint8_t kHello[3][8] = {
    {0x70, 0x1A, 0x11, 0x00, 0x00, 0x00, 0x00, 0x01},
    {0xB0, 0x14, 0x11, 0x00, 0x1F, 0x00, 0x00, 0x00},
    {0xB0, 0x14, 0x11, 0x00, 0x1F, 0x00, 0x00, 0x00},
};

// requestArg is 0x00 AND MUST STAY SO. Carminat's `BA 00 00 …` is 0xBA followed by seven
// filler bytes that merely happen to be zero; UpdateList's `7A 01` carries a genuine
// argument. The two look symmetrical on the wire and are not. [CAP]
//
//   syncId, syncReplyId, replyFlag, alive, request, requestArg, filler, hello, helloCount
inline constexpr SyncProfile kSync = {
    kIdSync, kIdSyncReply, 0x0400, 0xB9, 0xBA, 0x00, kFiller, kHello, 3};

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

// setText / showPopupText header bytes.
inline constexpr uint8_t kIconsNone   = 0x55;  // NoNews|NoTraffic|NoAfRds|NoMode [CAP]
inline constexpr uint8_t kIconsAfRds  = 0x45;  // documented, never emitted by us
inline constexpr uint8_t kIconBank2   = 0x55;  // fixed, meaning unknown          [CAP]
inline constexpr uint8_t kSrcIconNone = 0xFF;  // 0xDF "MANU", 0xFD "PRESET"      [CAP]
inline constexpr uint8_t kFormatPlain = 0x60;  // 0x19-0x3F radio style, 0x59-0x7F ASCII
inline constexpr uint8_t kControlByte = 0x01;  // always 0x01                     [CAP]
inline constexpr uint8_t kPopupIcon   = 0x09;  // the OEM volume popup's left icon [CAP]

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
inline constexpr uint8_t kConfirmCapMax  = 7;   // 6 is the safe maximum — see the note in
                                                // CarminatDisplay::showConfirmBox
inline constexpr uint8_t kClockDigits    = 4;   // "HHMM"

// Scroll-arrow indicator, byte 10 of the menu screen. Derived from the selection's
// position in the list by Menu::scrollIndicator(); exposed because showMenu() takes it.
enum ScrollIndicator : uint8_t {
  kScrollNone = 0x00,   // no arrows
  kScrollUp   = 0x07,   // top arrow only
  kScrollDown = 0x0B,   // bottom arrow only
  kScrollBoth = 0x0C,   // both
};

}  // namespace carminat
}  // namespace affa

#endif  // AFFA_PANEL_CARMINAT
