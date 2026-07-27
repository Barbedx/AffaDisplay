#include "MenuController.h"

#if AFFA_PANEL_CARMINAT && AFFA_ENABLE_MENU

namespace affa {

void MenuController::pushPage(IPage* p) {
  if (!p) return;
  _currentPage = p;
  p->onEnter();
}

void MenuController::popPage() {
  if (!_currentPage) return;
  _currentPage->onExit();
  _currentPage = nullptr;
  // The menu was underneath the page and the page overwrote the glass, so it has to be
  // redrawn — the panel keeps no stack of its own.
  //
  // The redraw's verdict is deliberately not consulted: popPage() has no caller to report it
  // to, and a redraw that could not be queued (no sync, queue full) is cosmetic — the next key
  // renders again. It is readable through CarminatDisplay::menuRenderer().lastResult() by an
  // application that cares.
  if (_menu.isOpen()) _menu.render();
}

// The (Key, KeyEdge) -> intent map, carried over verbatim from the deleted Menu::handleKey.
// A CLOSED MENU CONSUMES NOTHING: MenuModel's navigation methods all return false when the
// menu is not open, which is what lets an application implement its own open gesture in its
// KeyCb and keep every other key.
bool MenuController::routeKey(Key k, KeyEdge e) {
  if (_currentPage) {
    _currentPage->handleKey(k, e);
    return true;
  }

  const bool hold = (e == KeyEdge::Hold);
  switch (k) {
    // While editing, hold is the coarse step; while not editing, both simply move the
    // selection — a held wheel on a list is still just a wheel. That split lives in the
    // model (next/prev vs increase/decrease); the edge -> intent choice lives here.
    case Key::RollUp:   return hold ? _menu.decrease() : _menu.prev();
    case Key::RollDown: return hold ? _menu.increase() : _menu.next();

    // Hold-Load closes the menu even from inside edit mode — one gesture, one meaning. The
    // Hold edge is tested BEFORE the editing state, exactly as the old widget did.
    case Key::Load:     return hold ? _menu.back() : _menu.select();

    default:
      // SrcNext, SrcPrev, VolUp, VolDown, Pause: the menu has no opinion, so they reach the
      // application even while it is open. Returning false here is what makes
      // AffaDisplayBase::routeKey fall through to the KeyCb.
      return false;
  }
}

}  // namespace affa

#endif  // AFFA_PANEL_CARMINAT && AFFA_ENABLE_MENU
