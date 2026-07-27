// A full-screen page pushed in front of the menu.
//
// The library owns the NAVIGATION STACK (one active page, keys routed to it first); the
// application owns every page. A page renders through the IPanel it was handed — it never
// builds a frame and never touches the link.
//
// This is the seam the extracted DiagPage/ELM screens hung on. Those pages themselves are
// application policy (one car, one OBD adapter) and are not part of this library.
#pragma once
#include "../AffaConfig.h"

#if AFFA_PANEL_CARMINAT && AFFA_ENABLE_MENU

#include "../core/AffaTypes.h"

namespace affa {

struct IPage {
  virtual ~IPage() = default;

  // Pushed / popped by MenuController. onExit() is followed by a menu re-render if the
  // menu was open underneath.
  virtual void onEnter() = 0;
  virtual void onExit()  = 0;

  // Called from AffaDisplayBase::poll() -> CarminatDisplay::onPoll(), ONCE PER POLL. It is
  // therefore called at whatever rate the application pumps the library, which is not a
  // rate anything may depend on: a page that needs a period must compare against its own
  // clock, exactly as the library does. Never sleep in here.
  virtual void onTick()  = 0;

  // The page owns every key while it is active — including the menu hotkey, which is why
  // CarminatDisplay::openMenu() refuses while a page is pushed.
  virtual void handleKey(Key k, KeyEdge e) = 0;
};

}  // namespace affa

#endif  // AFFA_PANEL_CARMINAT && AFFA_ENABLE_MENU
