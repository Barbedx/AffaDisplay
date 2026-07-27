// Entire translation unit gated — see AffaConfig.h.
#include "UpdateListSegVirtualPanel.h"

#if AFFA_ENABLE_VIRTUAL_PANEL && AFFA_PANEL_UPDATELIST

namespace affa {

void UpdateListSegVirtualPanel::decode(const Frame& f) {
  // Only the text channel carries a screen payload. Display-control (0x1B1) is
  // enable/disable and is acknowledged by the base without a decode.
  if (f.id != updatelist_twin::kIdSetText) return;
  if (!_asm.onFrame(f)) return;

  const uint8_t* p = _asm.buffer();
  const uint8_t  n = _asm.len();
  if (n < 3) return;

  // 0x7F first: it is the form that CARRIES the icon header, and the latch has to be
  // updated from it before anything reads it. A 0x76 leaves the latch exactly where the
  // last 0x7F put it — that is the differential encoding, and modelling it here is what
  // lets a test prove the driver forces a 0x7F after a resync instead of assuming it.
  if (p[2] == screen::kSegIconCmd) {
    uint8_t icons = 0, mode = 0;
    if (screen::segTextIcons(p, n, _screen, &icons, &mode)) latchIcons(icons, mode);
    return;
  }

  if (p[2] == screen::kSegCmd) screen::segText(p, n, _screen);
}

}  // namespace affa
#endif  // AFFA_ENABLE_VIRTUAL_PANEL && AFFA_PANEL_UPDATELIST
