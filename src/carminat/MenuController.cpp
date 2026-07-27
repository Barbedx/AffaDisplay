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
  if (_menu.isOpen()) _menu.render();
}

bool MenuController::routeKey(Key k, KeyEdge e) {
  if (_currentPage) {
    _currentPage->handleKey(k, e);
    return true;
  }
  return _menu.handleKey(k, e);
}

}  // namespace affa

#endif  // AFFA_PANEL_CARMINAT && AFFA_ENABLE_MENU
