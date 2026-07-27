// Entire translation unit gated — see AffaConfig.h.
#include "UpdateListLcdVirtualPanel.h"

#if AFFA_ENABLE_VIRTUAL_PANEL && AFFA_PANEL_UPDATELIST_MENU

namespace affa {

void UpdateListLcdVirtualPanel::decode(const Frame& f) {
  if (f.id != updatelist_twin::kIdSetText) return;   // text channel only

  // Highlight, IF the mono UL panel uses the Carminat `07 29 01` convention. It has never
  // been observed doing so and our driver never sends one on 0x121, so this line is
  // hypothesis, not fact — kept because it costs nothing and screen::frame() carries the
  // full three-byte guard, so it cannot fire on anything else.
  if (screen::frame(f, _screen)) return;

  if (!_asm.onFrame(f)) return;

  const uint8_t* p = _asm.buffer();
  const uint8_t  n = _asm.len();
  if (n < 3) return;

  // [WIRE-SPEC §9.2] The form this panel actually receives: 10 1C 7F <icons> 55 <mode>
  // <chan> <loc> old(8) 10 new(12) 00. Five frames, last PCI 0x24.
  if (p[2] == screen::kSegIconCmd) {
    uint8_t icons = 0, mode = 0;
    if (screen::segTextIcons(p, n, _screen, &icons, &mode)) latchIcons(icons, mode);
    return;
  }

  // [WIRE-SPEC §9.1] The text-only form, in case the 8-segment driver is pointed at this
  // panel. Icons are LEFT ALONE, which is the whole meaning of the differential encoding.
  if (p[2] == screen::kSegCmd) { screen::segText(p, n, _screen); return; }

  // PROVISIONAL FALLBACK — the reference twin's hypothesis that the mono UL panel also
  // accepts the Carminat 96-byte windowed-menu payload on its text channel. Nothing in
  // the corpus shows such a payload on 0x121 and no driver of ours emits one, so this has
  // never been exercised on any bus, real or simulated. It is guarded on the command byte
  // as well as the length so that it cannot fire on a mis-sized 0x76/0x7F message.
  if (screen::isMenuPayload(p, n) && n >= screen::kMenuHwMinLen)
    screen::menu(p, n, _screen);
}

}  // namespace affa
#endif  // AFFA_ENABLE_VIRTUAL_PANEL && AFFA_PANEL_UPDATELIST_MENU
