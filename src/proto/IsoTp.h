// AFFA ISO-TP-ish framing, expressed as a function (transmit) and a class (receive).
//
// The TRANSMIT layout is specified in docs/API.md §2.9.2 and built inline by
// AffaDisplayBase::pumpTx() — the core deliberately does NOT call into proto/, so a
// consumer who never enables AFFA_ENABLE_ISOTP_RX links none of this. fragment() is the
// same layout expressed as a function, for the twins and the tests. That duplication is
// fenced by test_isotp/fragment_matches_fsm, which drives every payload length from 1 to
// AFFA_MAX_PAYLOAD through both paths and asserts the frame sequences are byte-identical.
//
// The layout, once, so it is not restated in three places:
//   frame 0 : 8 raw payload bytes, NO PCI prefix. The 0x10 at the head of a screen
//             payload is payload byte 0 built by the CALLER, not a transport byte
//             (docs/WIRE-SPEC.md §3.3).
//   frame n : [0x20 | (n & 0x0F)] then 7 payload bytes.
//   every frame is DLC 8 and padded with the panel's filler (Carminat 0x00 / UL 0x81).
//
// The continuation counter WRAPS (isoTpCf in core/AffaConstants.h). Legacy wrote
// `0x20 + num`, which produces 0x30 — the ISO-TP flow-control PCI — at num == 16. No
// message in our repertoire is that long, so the two are byte-identical here, but the
// wrapping form is the one the OEM head unit uses on its 302-byte 0x1F1 message. [CAP]
#pragma once

#include "../AffaConfig.h"
#include "../core/AffaTypes.h"
#include "../core/AffaConstants.h"

namespace affa {
namespace isotp {

// Number of frames fragment() would produce for `len` payload bytes, if nothing stopped
// the sender early. A REAL PANEL DOES stop it early when the declared FF_DL is shorter
// than what the builder holds: showMenu is 96 bytes = 14 frames here, but 13 on hardware
// (last PCI 0x2C) and 14 through the self-ACK emulator. Any golden vector must be
// parameterised by ACK model, never a bare 14. docs/WIRE-SPEC.md §3.6, §8.5.
//
// Forwards to core/AffaConstants.h rather than restating the arithmetic: one formula.
constexpr uint8_t frameCount(uint8_t len) { return isoTpFrameCount(len); }

// Split a payload into CAN frames (the transmit direction). Returns the number of frames
// written to `out`; the caller sizes `out` with frameCount(). Writes nothing and returns
// 0 for a null payload or len == 0.
//
// Frames are NOT stamped fromSelf: that flag belongs to whoever hands them to
// ICanLink::send(), because it is a property of the link crossing, not of the layout.
//
// DECLARED HERE UNGATED, DEFINED IN A GATED .cpp — exactly as docs/API.md §2.13 has it.
// With both gates off the whole of IsoTp.cpp is empty, so calling this would be a link
// error rather than a compile error. Nothing in the library does; AffaDisplay.h does not
// even include this header unless AFFA_ENABLE_ISOTP_RX is set. If you want fragment()
// alone, AFFA_ENABLE_ISOTP_RX=1 is the flag that buys it.
uint8_t fragment(uint16_t id, const uint8_t* payload, uint8_t len, uint8_t filler,
                 Frame* out, uint8_t maxOut);

#if (AFFA_ENABLE_VIRTUAL_PANEL || AFFA_ENABLE_ISOTP_RX)

// The receive direction. Feed every frame in arrival order; read buffer()/len() after
// each. A frame whose data[0] is 0x10 starts a fresh message, 0x2N appends, anything
// else is ignored and leaves the buffer untouched.
//
// There is no continuation-sequence check and no gap detection, DELIBERATELY: this is a
// decoder for traffic we are watching, not a transport we depend on. A dropped frame
// yields a short or scrambled payload, which affa::screen rejects on length. Adding a
// sequence check would turn a partially-decoded screen — which is useful — into nothing.
//
// The buffer is AFFA_MAX_PAYLOAD bytes and appends STOP at that ceiling rather than
// wrapping: a wrapped reassembler decodes a plausible-looking wrong screen, which is the
// one failure mode a semantic oracle must never have.
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

#endif  // AFFA_ENABLE_VIRTUAL_PANEL || AFFA_ENABLE_ISOTP_RX

}  // namespace isotp
}  // namespace affa
