// AFFA ISO-TP-ish framing: a function (transmit) and a class (receive).
//
// The layout, stated once:
//   frame 0 : 8 raw payload bytes, NO PCI prefix. The 0x10 at the head of a screen payload
//             is payload byte 0 built by the CALLER, not a transport byte (WIRE-SPEC §3.3).
//   frame n : [0x20 | (n & 0x0F)] then 7 payload bytes.
//   every frame is DLC 8, padded with the panel's filler (Carminat 0x00 / UL 0x81).
//
// The continuation counter WRAPS (isoTpCf in core/AffaConstants.h). Legacy wrote
// `0x20 + num`, producing 0x30 — the ISO-TP flow-control PCI — at num == 16. Nothing in
// our repertoire is that long, so the two are byte-identical here, but the wrapping form
// is what the OEM head unit uses on its 302-byte 0x1F1 message. [CAP]
//
// AffaDisplayBase::pumpTx() builds this layout inline rather than calling into proto/, so
// a consumer without AFFA_ENABLE_ISOTP_RX links none of this. The duplication is fenced by
// test_isotp/fragment_matches_fsm, which drives every length 1..AFFA_MAX_PAYLOAD through
// both paths and asserts byte-identical frame sequences.
#pragma once

#include "../AffaConfig.h"
#include "../core/AffaTypes.h"
#include "../core/AffaConstants.h"

namespace affa {
namespace isotp {

// Frames fragment() would produce for `len` bytes if nothing stopped the sender early. A
// REAL PANEL DOES stop it early when the declared FF_DL is shorter than what the builder
// holds: showMenu's 96 bytes are 14 frames here, 13 on hardware (last PCI 0x2C) and 14
// through the self-ACK emulator. A golden vector must be parameterised by ACK model, never
// a bare 14. docs/WIRE-SPEC.md §3.6, §8.5.
constexpr uint8_t frameCount(uint8_t len) { return isoTpFrameCount(len); }

// Split a payload into CAN frames. Returns frames written to `out`, which the caller sizes
// with frameCount(); writes nothing and returns 0 for a null payload or len == 0.
//
// Frames are NOT stamped fromSelf: that is a property of the link crossing, so it belongs
// to whoever hands them to ICanLink::send().
//
// Declared ungated, defined in a gated .cpp (docs/API.md §2.13): with both gates off
// IsoTp.cpp is empty and calling this is a link error, not a compile error. Nothing in the
// library does. AFFA_ENABLE_ISOTP_RX=1 buys fragment() alone.
uint8_t fragment(uint16_t id, const uint8_t* payload, uint8_t len, uint8_t filler,
                 Frame* out, uint8_t maxOut);

#if AFFA_ENABLE_ISOTP_RX

// The receive direction. Feed every frame in arrival order; read buffer()/len() after
// each. data[0] == 0x10 starts a fresh message, 0x2N appends, anything else is ignored.
//
// No continuation-sequence check and no gap detection, deliberately: this decodes traffic
// we are watching, not a transport we depend on. A dropped frame yields a short payload,
// which affa::screen rejects on length; a sequence check would turn a partially-decoded
// screen — which is useful — into nothing.
//
// Appends STOP at the AFFA_MAX_PAYLOAD ceiling rather than wrapping: a wrapped reassembler
// decodes a plausible-looking wrong screen, the one failure mode an oracle must not have.
class Reassembler {
 public:
  // True when the frame was consumed as an ISO-TP data frame (first or continuation).
  bool onFrame(const Frame& f);

  const uint8_t* buffer() const { return _buf; }
  uint8_t        len()    const { return _len; }
  bool           active() const { return _active; }
  void           reset()        { _len = 0; _active = false; }

 private:
  uint8_t _buf[AFFA_MAX_PAYLOAD] = {0};
  uint8_t _len    = 0;
  bool    _active = false;
};

#endif  // AFFA_ENABLE_ISOTP_RX

}  // namespace isotp
}  // namespace affa
