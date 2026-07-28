// The dashboard-cluster variant — A THIRD SYNC PROFILE, AND NOBODY HAS RUN IT.
//
// EVERYTHING IN THIS FILE IS TRANSCRIBED FROM ONE CAPTURE of an OEM radio talking to an
// instrument cluster, supplied 2026-07-28 and analysed in docs/PROTOCOL-NOTES.md §9. Not one
// byte has been put on a bus by this library. It is here because it costs a header to try
// and because it is the evidence that SyncProfile was cut in the right place: a third family
// turned out to need new DATA and no new code.
//
// WHAT THE CAPTURE DOES NOT CONTAIN: any text frame. The radio never renders in the sample,
// so the cluster's setText encoding is unknown and ClusterDisplay deliberately does not
// implement one. What it can do is complete the handshake, latch registration and control
// the display — which is exactly as far as the evidence goes.
#pragma once
#include "../AffaConfig.h"

#if AFFA_PANEL_CLUSTER

#include "../core/AffaTypes.h"
#include "../core/AffaSyncProfile.h"

namespace affa {
namespace cluster {

// ---------------------------------------------------------------------------
// Ids — the same map as Carminat, with one addition
// ---------------------------------------------------------------------------
inline constexpr uint16_t kIdSync      = 0x3AF;  // WE transmit here — Carminat's id
inline constexpr uint16_t kIdSyncReply = 0x3CF;  // the cluster answers here
inline constexpr uint16_t kIdSetText   = 0x121;  // registered, encoding UNKNOWN
inline constexpr uint16_t kIdDisplayCtrl = 0x1B1;
inline constexpr uint16_t kIdKeyPressed  = 0x1C1;  // the CLUSTER transmits on this; we ACK
inline constexpr uint16_t kIdClock        = 0x3EF; // NOT an AFFA message — see below

// A fourth sync id appears in the capture as `3BF 2 49 00` and is not modelled. Nothing is
// known about it beyond that it exists and follows the same X9 shape.
inline constexpr uint16_t kIdSyncOther = 0x3BF;

// ---------------------------------------------------------------------------
// Sync bytes — a THIRD pair, on an id we already knew
// ---------------------------------------------------------------------------
// Carminat is B9/BA, UpdateList is 79/7A, this is 59/5A. The X9-alive / XA-request pattern
// holds across all three, which is the most useful thing this capture tells us.
inline constexpr uint8_t kAliveByte   = 0x59;   // `3AF 2 59 00`
inline constexpr uint8_t kRequestByte = 0x5A;   // `3AF 2 5A 01`
inline constexpr uint8_t kRequestArg  = 0x01;   // NOT 0x00 as Carminat's is

// The RADIO pads 0xFF in this capture; the cluster pads 0x84. Filler is a property of the
// SPEAKER, not of the bus, and this is ours because we are the radio.
inline constexpr uint8_t kFiller = 0xFF;

// `3AF 8 50 29 00 23 00 00 00 69`, seen three times ~30 ms apart. Whether that is three
// hellos or one hello retried twice is NOT established; three is the conservative reading,
// since sending a hello the peer ignores is harmless and missing one is not.
inline constexpr uint8_t kHello[][8] = {
    {0x50, 0x29, 0x00, 0x23, 0x00, 0x00, 0x00, 0x69},
    {0x50, 0x29, 0x00, 0x23, 0x00, 0x00, 0x00, 0x69},
    {0x50, 0x29, 0x00, 0x23, 0x00, 0x00, 0x00, 0x69},
};

inline constexpr SyncProfile kSync{
    kIdSync, kIdSyncReply, 0x0400, kAliveByte, kRequestByte, kRequestArg, kFiller, kHello, 3};

// The radio registers TWO functions in the capture — `121 70 FF..` answered on `521 74 ..`
// and `1B1 70 FF..` answered on `5B1 74 ..`. The cluster separately registers 0x1C1 to US,
// which the base already answers through its generic auto-ACK.
inline constexpr uint16_t kFuncIds[]  = {kIdSetText, kIdDisplayCtrl};
inline constexpr uint8_t  kFuncCount  = 2;

// ---------------------------------------------------------------------------
// Display control — a THIRD declared length for the same command
// ---------------------------------------------------------------------------
// `1B1 8 03 52 00 00 FF FF FF FF`. SF_DL 0x03 where UpdateList declares 0x04 for the same
// shape and Carminat declares 0x03 for its own. Only the OFF value is in the capture; ON is
// inferred from the other two families and is the least certain byte in this file.
inline constexpr uint8_t kPowerSfDl   = 0x03;
inline constexpr uint8_t kCmdSetState = 0x52;
inline constexpr uint8_t kPowerOff    = 0x00;   // [CAP]
inline constexpr uint8_t kPowerOn     = 0x02;   // [GUESS] — UpdateList's value, untested
inline constexpr uint8_t kPowerTail   = 0x00;
inline constexpr uint8_t kPowerLen    = 4;

// ---------------------------------------------------------------------------
// The clock — `3EF A6 <hours> <minutes>`, and it is NOT an AFFA message
// ---------------------------------------------------------------------------
// Three bytes at DLC 3 with NO PCI and NO SF_DL. It does not go through the transport at
// all; anything calling enqueue() with it will frame it and corrupt it. Send it with a raw
// ICanLink::send(), which is why that escape hatch exists.
//
// This is the only known way to set the clock on a panel whose head unit has no clock
// button — UpdateList has no setTime of its own at all.
inline constexpr uint8_t kClockCmd = 0xA6;

}  // namespace cluster
}  // namespace affa

#endif  // AFFA_PANEL_CLUSTER
