// Protocol constants shared by BOTH panel families. Nothing panel-specific lives here —
// identifiers and fillers belong to carminat/ and updatelist/.
//
// Every value carries its witness. [CAP] = seen verbatim in MeganeCAN/logs/*.log,
// [OEM] = seen from a node that is not ours, [REF] = notes/archive_mhroczny/affa3.c,
// [DERIVED] = hand-executed from the builder with no capture. Full arithmetic and the
// quoted log lines are in docs/WIRE-SPEC.md.
#pragma once
#include <cstdint>

namespace affa {

// Every AFFA frame is DLC 8. Short DLCs are real on the RECEIVE side — the OEM corpus
// contains 0x3CF with DLC 1 and DLC 2 — so decoders must check len before indexing.
inline constexpr uint8_t kPacketLength = 8;

// Hold is 0x80|0x40 set in the LOW byte of the key code. 0x40 is simultaneously
// RollDown's direction bit, which is why the two wheel codes are exempt (see
// kKeyRollUp/kKeyRollDown below). [REF] affa3.c:128-137
inline constexpr uint8_t kKeyHoldMask = 0x80 | 0x40;

// Clears the two hold bits of the low byte only. Written explicitly rather than as
// `raw & ~kKeyHoldMask`, which works solely by accident of integer promotion
// (~(uint8_t)0xC0 is 0xFFFFFF3F, so the high byte survives).
inline constexpr uint16_t kKeyCodeMask = 0xFF3F;

// The two encoder detent codes. Exempt from hold masking: masking 0x0141 would rewrite
// it to 0x0101, i.e. every wheel-down detent would be reported as a wheel-up.
inline constexpr uint16_t kKeyRollUp   = 0x0101;
inline constexpr uint16_t kKeyRollDown = 0x0141;

// The two guard bytes of a key frame. 0x1C1 is NOT a key-only channel — the corpus holds
// `70 A3..`, `02 64 0F A3..` and `05 63 30 30 33 37 A3 A3` on it from the panel — and a
// decoder without this guard invents keys 0x640F and 0x3030 out of that traffic. [CAP]
inline constexpr uint8_t kKeyFrameByte0 = 0x03;
inline constexpr uint8_t kKeyFrameByte1 = 0x89;

// An ACK arrives on funcId | replyFlag. COMPUTE IT, never hard-code it:
// 0x0A9 | 0x400 is 0x4A9 and not 0x5A9, because bit 8 is already clear in 0x0A9 — the
// one identifier in the table where that is true.
inline constexpr uint16_t kReplyFlag = 0x0400;

// ACK repertoire on funcId|replyFlag. Only data[0] (DONE) and data[0..2] (PARTIAL) carry
// meaning; the remaining bytes are the sender's filler and MUST NOT be matched on. The
// filler is per-node and means nothing: our bench panel pads 0xA3, one OEM cluster pads
// 0x84, the OEM radio pads 0xFF, we pad 0x00 (Carminat) or 0x81 (UpdateList). [CAP]
inline constexpr uint8_t kAckDone       = 0x74;  // end of data — message accepted
inline constexpr uint8_t kAckPartial0   = 0x30;  // partial accepted, send the next frame
inline constexpr uint8_t kAckPartial1   = 0x01;
inline constexpr uint8_t kAckPartial2   = 0x00;

// Function registration: a 1-byte payload sent to every id in the panel's function table
// before the first payload after a resync. [CAP]
inline constexpr uint8_t kRegisterByte = 0x70;

// Frame 0 carries EIGHT raw payload bytes with no transport PCI — the 0x10 at the head of
// a screen payload is payload byte 0, built by the caller. Continuations carry seven,
// prefixed with the counter below.
inline constexpr uint8_t kFrame0Bytes = 8;
inline constexpr uint8_t kFrameNBytes = 7;

// Continuation PCI; the counter WRAPS. Legacy `0x20 + num` produces 0x30 at num == 16 —
// the ISO-TP flow-control PCI, not a consecutive frame. The OEM head unit wraps modulo 16
// on its 302-byte 0x1F1 message (distinct PCIs there: 11 20 21..2A 2C 2D 2E 2F, 0x20
// present, nothing above 0x2F). [CAP] docs/WIRE-SPEC.md §3.3
inline constexpr uint8_t kIsoTpCfBase = 0x20;
inline constexpr uint8_t kIsoTpCfMask = 0x0F;
inline constexpr uint8_t isoTpCf(uint8_t num) {
  return static_cast<uint8_t>(kIsoTpCfBase | (num & kIsoTpCfMask));
}

// Frames a `len`-byte payload occupies if nothing stops the sender early. The panel DOES
// stop early when the declared FF_DL is shorter than the builder holds — showMenu is the
// one over-run in the repertoire, ending at PCI 0x2C — so this is a ceiling, not always
// the count. docs/WIRE-SPEC.md §3.6
constexpr uint8_t isoTpFrameCount(uint8_t len) {
  return (len <= kFrame0Bytes)
             ? 1
             : static_cast<uint8_t>(1 + (len - kFrame0Bytes + (kFrameNBytes - 1)) / kFrameNBytes);
}

// Sync handshake bytes shared by both families. The per-family bytes (aliveByte,
// requestByte, requestArg, filler, ids) are in SyncProfile.
inline constexpr uint8_t kSyncRequestByte0 = 0x61;  // panel asks us to announce
inline constexpr uint8_t kSyncRequestByte1 = 0x11;  // ... our panel's token. One OEM
                                                    // cluster says 0x23 and we would
                                                    // never answer it (Appendix C).
// data[2] of the sync request. OBSERVED ON THE BENCH PANEL 2026-07-29, which corrects what
// this comment used to claim — "never observed, data[2] is the panel's filler". It is not
// the filler: the filler is 0xA3 and it starts at data[3]. data[2] is a real field, and both
// values have now been captured on the same panel:
//
//     3CF  61 11 00 A3 A3 A3 A3 A3    2026-07-26, after a completed handshake
//     3CF  61 11 01 A3 A3 A3 A3 A3    2026-07-29, from power-on, never acknowledged
//
// Its MEANING is still unknown. All the library does with it is raise SyncState::Start,
// which only makes the sync request go out again on the next poll — there is no
// authentication step in this handshake as far as any capture shows. Do not infer one from
// the byte alone; if it turns out to matter, it will be because a capture showed the panel
// behaving differently, not because 0x01 looks significant.
inline constexpr uint8_t kSyncStartFlag    = 0x01;
inline constexpr uint8_t kSyncPeerAlive    = 0x69;  // ~1 Hz ping. data[0] is the ENTIRE
                                                    // test; nothing else may be read.

} // namespace affa
