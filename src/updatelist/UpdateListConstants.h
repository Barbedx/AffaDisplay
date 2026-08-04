// UpdateList (AFFA2) family constants: identifiers, filler, sync profile, and every
// literal byte of the three payloads this family builds.
//
// Every value carries its witness, in the notation of docs/WIRE-SPEC.md:
//   [CAP]  seen verbatim in MeganeCAN/logs/*.log
//   [REF]  notes/archive_mhroczny/affa3.{c,h}
//   [CODE] present in the extracted source only
//
// Nothing here is panel-agnostic; the shared bytes live in core/AffaConstants.h.
#pragma once
#include "../AffaConfig.h"
#if AFFA_PANEL_UPDATELIST

#include "../core/AffaConstants.h"
#include "../core/AffaSyncProfile.h"
#include <cstdint>

namespace affa {
namespace updatelist {

// ---------------------------------------------------------------------------
// Identifiers  [CAP] docs/WIRE-SPEC.md §2.1, third-party corroboration W10 §8.6
// ---------------------------------------------------------------------------
inline constexpr uint16_t kIdSync        = 0x3DF;  // we transmit the handshake here
inline constexpr uint16_t kIdSyncReply   = 0x3CF;  // the panel answers here
inline constexpr uint16_t kIdSetText     = 0x121;  // text function
inline constexpr uint16_t kIdDisplayCtrl = 0x1B1;  // display-control function
inline constexpr uint16_t kIdKeyPressed  = 0x0A9;  // the panel's key channel

// Spelled out only so a static_assert, not a comment, pins the arithmetic: 0x0A9 | 0x400
// is 0x4A9 and NOT 0x5A9, because bit 8 is already clear in 0x0A9 — uniquely in either
// family's table. Nothing reads this; sendGenericAck() ORs the flag itself.
// [CAP] docs/WIRE-SPEC.md §2.2
inline constexpr uint16_t kAckIdKeyPressed = kIdKeyPressed | kReplyFlag;
static_assert(kAckIdKeyPressed == 0x4A9, "0x0A9 | 0x400 is 0x4A9, not 0x5A9");

// ---------------------------------------------------------------------------
// Sync profile  [CAP] docs/WIRE-SPEC.md §5
// ---------------------------------------------------------------------------
// Pads every frame WE build. Per-node and meaningless on receive: our bench panel pads
// 0xA3, one OEM cluster 0x84, the OEM radio 0xFF. Never match on a received filler.
inline constexpr uint8_t kFiller      = 0x81;
inline constexpr uint8_t kAliveByte   = 0x79;  // 1 Hz heartbeat, data[0]
inline constexpr uint8_t kRequestByte = 0x7A;  // sync request, data[0]

// data[1] of the sync request, and a GENUINE ARGUMENT — unlike Carminat's 0x00, which is
// merely the first of seven filler bytes that happen to be zero. The two families look
// symmetrical on the wire and are not; do not "harmonise" them.
inline constexpr uint8_t kRequestArg = 0x01;

// ONE hello frame, sent in reply to `61 11`. Carminat sends three (the second and third
// identical); this family sends exactly one. [CAP]
inline constexpr uint8_t kHello[1][8] = {
    {0x70, 0x1A, 0x11, 0x00, 0x00, 0x00, 0x00, 0x01},
};

// THE SAME MACHINE AS CARMINAT, WITH ONE DIFFERENCE. Owner's decision, 2026-08-04, recorded
// in docs/REFACTOR-PLAN.md: the two originals were nearly the same code — affa3.c is the
// ancestor of both — the Carminat side has been reworked against OEM captures and proven on
// glass for 96 minutes, and the UpdateList reference *worked* rather than being *right*. A
// driver whose only heartbeat was a pong for most of its life, that registers lazily and
// serially, and that cannot notice a panel deauthorizing it, is coincidence-shaped.
//
// So four of the five structural differences are deliberately removed:
//
//   registration is part of the OPENING, pipelined — not lazy on the first render
//   the panel's own `0A9 70` GATES ours, instead of merely being acknowledged
//   any complete `61 11 xx` while registered VOIDS the session
//   the heartbeat starts after registration, and silence is broken by a slow announce
//     rather than the reference's BA-every-second
//
// THE ONE EXCEPTION IS `helloRequiresAnnounce`. Carminat's burst answers the panel's SECOND
// request, because 4/4 OEM captures put the radio's `BA` in between. There is no such
// capture for this family and its reference answers the FIRST request straight out of
// recv(), so it keeps that. Our announce should still provoke a request; it is simply not a
// precondition here.
//
// NOT ONE BYTE CHANGES. Every literal above is unmoved and pinned by test_updatelist_wire;
// what changes is WHEN frames go out. If a real UpdateList panel ever reaches a bench and
// stalls, the first knob to turn is `replyToPing` (its reference pongs every `0x69`, and
// that pong was its only heartbeat until March 2026) and the second is reverting
// `registerAfterHello` to lazy.
//
//   syncId, syncReplyId, replyFlag, alive, request, requestArg, filler, hello, helloCount,
//   replyToPing, waitForPanel, sendSyncRequest, requireAuthRequest, helloMinMs,
//   helloFirstDelayMs, helloFrameGapMs, payloadAfterRegistrationMs, syncIntervalMs,
//   registerAfterHello, announceWhenSilentMs, helloRequiresAnnounce
inline constexpr SyncProfile kSync = {
    kIdSync, kIdSyncReply, kReplyFlag, kAliveByte, kRequestByte, kRequestArg, kFiller,
    kHello, 1,
    false, // replyToPing — removed on four CARMINAT captures and no UpdateList evidence
           // either way. The first knob to turn if a panel ever stalls. [GUESS]
    true,  // waitForPanel — do not speak until the panel does...
    false, // ...and never as a periodic BA probe. The reference's every-second request is
           // the storm this pair exists to prevent.
    true,  // a bare 69 is only a ping; a complete 61 11 xx is the request
    0,     // helloMinMs — the generic AFFA_HELLO_MIN_MS floor; nothing measured here
    0,     // helloFirstDelayMs — the reference answers from inside recv(), immediately, and
    0,     // helloFrameGapMs — with ONE hello frame there is no gap to space anyway
    0,     // payloadAfterRegistrationMs — Carminat's measured 400 ms is a CARMINAT number;
           // there is no capture of this family's radio waiting, so it does not wait
    0,     // syncIntervalMs — AFFA_SYNC_INTERVAL_MS, i.e. the reference's 1 Hz alive
    true,  // registration belongs to the opening, gated on the panel's own 0A9
    30000, // announce into a SILENT bus. waitForPanel alone deadlocks against a display
           // that has gone quiet. The interval is Carminat's [GUESS] — nothing measures it
           // here, and it is slow enough that being wrong is cheap.
    false, // THE ONE DIFFERENCE: the burst answers the FIRST request, not the second.
};

// ORDER IS ON THE WIRE: the order the 0x70 registration probes go out in — now during the
// opening, right after the panel opens its own channel, rather than on the first render.
inline constexpr uint16_t kFuncIds[]  = {kIdSetText, kIdDisplayCtrl};
inline constexpr uint8_t  kFuncCount  = 2;

// ---------------------------------------------------------------------------
// setPower / display control — 0x1B1  [CAP] docs/WIRE-SPEC.md §9.3
// ---------------------------------------------------------------------------
//   04 52 <state> FF FF   + filler to 8
// The 0x04 is a single-frame PCI declaring FOUR content bytes, and here it is actually
// correct — this family's length bytes are the self-consistent ones. Carminat's spelling
// of the same command is `03 52 <state> FF FF`. DO NOT UNIFY THEM: both have been
// rendering correctly for months and a length byte is glass, not style.
inline constexpr uint8_t kPowerSfDl   = 0x04;
inline constexpr uint8_t kCmdSetState = 0x52;
inline constexpr uint8_t kPowerOn     = 0x02;  // Carminat's enable is 0x09, not this
inline constexpr uint8_t kPowerOff    = 0x00;
inline constexpr uint8_t kPowerTail   = 0xFF;  // bytes 3..4, fixed
inline constexpr uint8_t kPowerLen    = 5;

// ---------------------------------------------------------------------------
// setText — 0x121, both encodings  [CAP] docs/WIRE-SPEC.md §9.1, §9.2
// ---------------------------------------------------------------------------
inline constexpr uint8_t kCmdSetText   = 0x10;  // ISO-TP first frame / "set text"
inline constexpr uint8_t kTextSep      = 0x10;  // separator between the two text fields
inline constexpr uint8_t kOldCells     = 8;     // "old text" field width
inline constexpr uint8_t kNewCells     = 12;    // "new text" field width — what is shown
inline constexpr uint8_t kTextTerm     = 0x00;  // terminator

// -- 8-segment (UpdateListDisplay) -------------------------------------------
//   10 19 76 <chan> 01 old(8) 10 new(12) 00 81 81                      29 bytes
// 0x19 = 25 declared content bytes, covering data[2..26]: CORRECT, and the trailing two
// 0x81 sit outside it. Frames: 1 + ceil(21/7) = 4, last PCI 0x23.
inline constexpr uint8_t kSegFfDl      = 0x19;
inline constexpr uint8_t kSegTextType  = 0x76;  // text-only variant; 0x7F is text+icons
                                                // and this driver never emits it, so
                                                // icon control is NOT a capability here
inline constexpr uint8_t kSegLocation  = 0x01;  // LOCATION(0,0) | SELECTED
inline constexpr uint8_t kSegPayload   = 29;

// chan = 0x70 + digit for digit 0..9, else 0x7A ("no channel").
inline constexpr uint8_t kChanBase     = 0x70;
inline constexpr uint8_t kChanNone     = 0x7A;
inline constexpr uint8_t kChanMaxDigit = 9;

// -- LCD (UpdateListMenuDisplay) ---------------------------------------------
//   10 1C 7F 55 55 FF 60 03 old(8) 10 new(12) 00                       30 bytes
// 0x1C = 28 declared content bytes, covering data[2..29]: also correct. Frames:
// 1 + ceil(22/7) = 5, last PCI 0x24 carrying one byte and six filler.
inline constexpr uint8_t kLcdFfDl     = 0x1C;
inline constexpr uint8_t kLcdFixed    = 0x7F;
inline constexpr uint8_t kLcdIcons    = 0x55;  // NO_TRAFFIC|NO_NEWS|NO_AFRDS|NO_MODE [REF]
inline constexpr uint8_t kLcdIconSep  = 0x55;  // literal separator, not a second icon set
inline constexpr uint8_t kLcdIconMode = 0xFF;  // AFFA3_ICON_MODE_NONE [REF]
inline constexpr uint8_t kLcdChannel  = 0x60;  // LCD channel encoding is 0x60 | chan
inline constexpr uint8_t kLcdLocation = 0x03;  // LOCATION(0,0) | SELECTED | FULLSCREEN
inline constexpr uint8_t kLcdPayload  = 30;

// ---------------------------------------------------------------------------
// Inbound radio text — 0x121  [CAP] docs/WIRE-SPEC.md §9.6
// ---------------------------------------------------------------------------
// When the RADIO transmits the segment encoding on 0x121, data[5..7] are the first three
// cells of its "old text" field. `AUX` there is the whole of the extracted heuristic.
inline constexpr uint8_t kAuxProbeOffset = 5;

// ---------------------------------------------------------------------------
// AMS key-forwarding feedback  [CODE] UpdateListBase::ProcessKey
// ---------------------------------------------------------------------------
// The banner is exactly kOldCells wide so it fills the segment display and neither field
// is truncated. Both strings are 8 characters; do not shorten one of them.
inline constexpr char kAmsOnText[]  = "AMS  ON ";
inline constexpr char kAmsOffText[] = "AMS OFF ";
static_assert(sizeof(kAmsOnText)  == kOldCells + 1, "AMS banner must be 8 characters");
static_assert(sizeof(kAmsOffText) == kOldCells + 1, "AMS banner must be 8 characters");

// The legacy code sent the banner three times with delay(100) between them, so it would
// survive the next scroll step. The repeat count and the spacing are preserved; the
// blocking is not — poll() advances the schedule against IClock.
inline constexpr uint8_t  kAmsRepeats    = 3;
inline constexpr uint32_t kAmsRepeatMs   = 100;

// ---------------------------------------------------------------------------
// Title scroll  [CODE] UpdateListDisplay::tickMedia
// ---------------------------------------------------------------------------
inline constexpr uint8_t  kScrollWidth   = 8;    // the 8-segment display width
inline constexpr uint32_t kScrollStepMs  = 400;  // ms per one-character step
inline constexpr uint8_t  kScrollGap     = 8;    // blank cells appended before wrap-around

}  // namespace updatelist
}  // namespace affa

#endif  // AFFA_PANEL_UPDATELIST
