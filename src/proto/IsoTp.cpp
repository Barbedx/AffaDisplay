// Entire translation unit gated: with the twins and the RX decoder both off this file
// compiles to an empty object and the linker's --gc-sections has nothing left to do.
// See AffaConfig.h for why the gate has to be here and not in build_src_filter.
#include "IsoTp.h"

#if AFFA_ENABLE_ISOTP_RX

namespace affa {
namespace isotp {

uint8_t fragment(uint16_t id, const uint8_t* payload, uint8_t len, uint8_t filler,
                 Frame* out, uint8_t maxOut) {
  if (!payload || len == 0 || !out || maxOut == 0) return 0;

  uint8_t count = 0;
  uint8_t pos   = 0;
  uint8_t num   = 0;

  while (pos < len && count < maxOut) {
    Frame f;
    f.id  = id;
    f.len = kPacketLength;
    f.ext = false;

    uint8_t i = 0;
    // Frame 0 carries EIGHT raw payload bytes with NO PCI. This is the single most
    // misread thing about the AFFA transport and it is why the frame counts do not match
    // textbook ISO-TP. docs/WIRE-SPEC.md §3.3.
    if (num > 0) f.data[i++] = isoTpCf(num);
    while (i < kPacketLength && pos < len) f.data[i++] = payload[pos++];
    while (i < kPacketLength) f.data[i++] = filler;

    out[count++] = f;
    ++num;
  }
  return count;
}

bool Reassembler::onFrame(const Frame& f) {
  if (f.len == 0) return false;

  // First frame: eight bytes INCLUDING the PCI byte, because that byte is payload byte 0
  // and every offset in affa::screen is measured from it.
  //
  // The nibble, not the whole byte. ISO 15765-2 puts a 12-bit length in a first frame:
  // the high nibble is 1 and the low nibble is the top four bits of FF_DL, so a message
  // longer than 255 bytes arrives as 0x11..0x1F. Everything OUR two drivers build is
  // short enough to use 0x10, which is why matching the whole byte survived this long —
  // but AFFA_ENABLE_ISOTP_RX exists to sniff somebody else's head unit, and the OEM's
  // 302-byte 0x1F1 screen opens with `11 2E 21 0B 00 25 41 42`. Under the old test that
  // first frame was refused, _active stayed false, and all 43 continuation frames were
  // refused with it — the message decoded to nothing at all.
  //
  // Safe because no single-frame PCI in either family's repertoire can be mistaken for
  // one: a single frame's PCI is its length, and every single-frame payload here is
  // <= 7 bytes, so payload byte 0 is never in 0x11..0x1F.
  if ((f.data[0] & 0xF0) == 0x10) {
    _len    = 0;
    _active = true;
    for (uint8_t i = 0; i < kFrame0Bytes && i < f.len && _len < AFFA_MAX_PAYLOAD; ++i)
      _buf[_len++] = f.data[i];
    return true;
  }

  // Continuation: append data[1..7]. The counter in data[0] is NOT checked — see the
  // header. `_active` is: a continuation with no first frame in front of it is noise
  // from some other transfer and must not be appended to the previous message.
  if ((f.data[0] & 0xF0) == kIsoTpCfBase && _active) {
    for (uint8_t i = 1; i < kPacketLength && i < f.len && _len < AFFA_MAX_PAYLOAD; ++i)
      _buf[_len++] = f.data[i];
    return true;
  }

  return false;
}

}  // namespace isotp
}  // namespace affa

#endif  // AFFA_ENABLE_ISOTP_RX
