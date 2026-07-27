// The Carminat navigation stack: the active full-screen page, and key routing between
// page / menu / fall-through.
//
// It holds the Menu BY REFERENCE (CarminatDisplay owns it) and it does not own pages —
// the application creates them and pushes them in. No frame builder lives here: pages
// render through the IPanel they were given.
#pragma once
#include "../AffaConfig.h"

#if AFFA_PANEL_CARMINAT && AFFA_ENABLE_MENU

#include "../core/AffaTypes.h"
#include "IPage.h"
#include "Menu/Menu.h"

namespace affa {

class MenuController {
 public:
  explicit MenuController(Menu& menu) : _menu(menu) {}

  MenuController(const MenuController&)            = delete;
  MenuController& operator=(const MenuController&) = delete;

  void   pushPage(IPage* p);
  void   popPage();
  IPage* currentPage() const { return _currentPage; }

  // Called once per poll() from CarminatDisplay::onPoll(). See IPage::onTick().
  void   tickCurrentPage() { if (_currentPage) _currentPage->onTick(); }

  // Route a key: the active page first, else the menu. Returns true when the key was
  // consumed — i.e. the base must NOT fall through to the application's KeyCb.
  //
  //   active page present -> the page handles it, always consumed
  //   no page             -> the menu handles it; consumed only if the menu was open
  bool   routeKey(Key k, KeyEdge e);

 private:
  Menu&  _menu;
  IPage* _currentPage = nullptr;
};

}  // namespace affa

#endif  // AFFA_PANEL_CARMINAT && AFFA_ENABLE_MENU
