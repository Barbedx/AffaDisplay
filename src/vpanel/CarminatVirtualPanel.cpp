// Entire translation unit gated — see AffaConfig.h.
#include "CarminatVirtualPanel.h"

#if AFFA_ENABLE_VIRTUAL_PANEL && AFFA_PANEL_CARMINAT

namespace affa {

void CarminatVirtualPanel::decode(const Frame& f) {
  if (f.len == 0) return;

  // The highlight is a standalone single frame, not ISO-TP. screen::frame() carries the
  // full `07 29 01` guard, because 0x151 also carries `03 52 …`, `05 56 …` and `02 54 03`
  // and a looser test would manufacture a highlight out of one of them.
  if (screen::frame(f, _screen)) return;

  // EVERYTHING ELSE MULTI-FRAME GOES THROUGH THE REASSEMBLER, including first frames whose
  // command we do not decode.
  //
  // The reference returned early on an unrecognised first frame (`10` with data[1] != 0x5A)
  // WITHOUT touching the reassembler, so that message's continuation frames were still
  // appended — to the previous message's buffer, which was still active. The buffer then
  // grew past the end of the menu on every info popup until it hit its ceiling. Feeding
  // every frame in and dispatching on the decoded command is both simpler and correct.
  if (!_asm.onFrame(f)) return;      // single frame we do not model (setState, setTime, …)

  const uint8_t* p = _asm.buffer();
  const uint8_t  n = _asm.len();
  if (n < 4) return;                 // p[2] is the command, p[3] its first operand

  switch (p[2]) {
    case screen::kMenuCmd:           // 0x21 — windowed menu / now-playing / notification
      // Mode 0x05 is the FULLSCREEN variant (big text, confirm box); its payload has a
      // different layout entirely and is not modelled here. Decoding it with the menu
      // offsets would produce a confident wrong screen, which is the one thing a semantic
      // oracle must never do.
      if (p[3] == screen::kMenuModeWin) screen::menu(p, n, _screen);
      break;

    case screen::kWinTextCmdFull:    // 0x74 — full-window text / popup overlay
    case screen::kWinTextCmdWindow:  // 0x77 — windowed radio text (setText)
      screen::windowText(p, n, _screen);
      break;

    case screen::kInfoCmd:           // 0x76 — one row of the info-settings list
      screen::infoRow(p, n, _screen);
      break;

    default:
      break;                         // an unmodelled command: decoded as nothing, not as
                                     // a guess
  }
}

}  // namespace affa
#endif  // AFFA_ENABLE_VIRTUAL_PANEL && AFFA_PANEL_CARMINAT
