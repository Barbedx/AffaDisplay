#pragma once
#include <cstdint>

namespace affa {

// One sync FSM, two panel families: everything family-specific is data here, the code is
// in AffaDisplayBase::pumpSync(). Duplicating the FSM is what let the same two defects
// live twice — a watchdog counting tick() CALLS instead of milliseconds, and a delay(100)
// in the sync-request branch.
struct SyncProfile {
  uint16_t syncId;        // Carminat 0x3AF   UpdateList 0x3DF   (we transmit here)
  uint16_t syncReplyId;   // both 0x3CF                          (panel transmits here)
  uint16_t replyFlag;     // both 0x400
  uint8_t  aliveByte;     // 0xB9 / 0x79   heartbeat, data[0]
  uint8_t  requestByte;   // 0xBA / 0x7A   sync request, data[0]
  uint8_t  requestArg;    // Carminat 0x00, UpdateList 0x01 — data[1] of the request.
                          // DO NOT "harmonise" these: Carminat's `BA 00 00 …` is 0xBA
                          // followed by seven filler bytes that merely happen to be
                          // 0x00, whereas UpdateList's 0x01 is a genuine argument. The
                          // wire looks symmetrical and is not.
  uint8_t  filler;        // 0x00 / 0x81   pads every frame we build. data[1] of the
                          // HEARTBEAT is a literal 0x00 in both families and is NOT the
                          // filler: UpdateList sends `79 00 81 81 …`, not `79 81 81 …`.
  const uint8_t (*hello)[8];  // frames sent in reply to `61 11`, in order
  uint8_t  helloCount;    // Carminat 3 (the second and third are IDENTICAL — two sendCan
                          // calls in the legacy source, and present in the capture),
                          // UpdateList 1
};

} // namespace affa
