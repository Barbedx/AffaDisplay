// The Carminat navigation stack: the active full-screen page, and key routing between
// page / menu / fall-through.
//
// It holds the menu MODEL by reference (CarminatDisplay owns it) and it does not own pages —
// the application creates them and pushes them in. No frame builder lives here: pages render
// through the IPanel they were given.
//
// WHY THIS DID NOT MOVE INTO widget::MenuModel. The model is a menu; this is NAVIGATION
// POLICY, and the two are separable. Which gesture opens a menu, whether a full-screen page
// can sit in front of it, who gets a key first and what happens to the glass when the page is
// popped are decisions an application with a different remote will make differently — the
// model deliberately has no key vocabulary at all (six intents, no Key enum) so that this
// layer can be replaced without touching it.
//
// THE KEY MAP IS HERE, and it is the one the deleted src/carminat/Menu/Menu.cpp had:
//
//     RollUp   click -> prev()        hold -> decrease()
//     RollDown click -> next()        hold -> increase()
//     Load     click -> select()      hold -> back()   (closes, even from inside edit mode)
//     anything else                   -> NOT consumed
//
// That last line is load-bearing. SrcNext, SrcPrev, VolUp, VolDown and Pause must reach the
// application even while the menu is open — the old Menu::handleKey returned false for them
// and AffaDisplayBase::routeKey falls through to the application's KeyCb on false. Routing
// every key you receive into the model would swallow them.
#pragma once
#include "../AffaConfig.h"

#if AFFA_PANEL_CARMINAT && AFFA_ENABLE_MENU

#include "../core/AffaTypes.h"
#include "../widget/MenuModel.h"
#include "IPage.h"

namespace affa {

class MenuController {
 public:
  explicit MenuController(widget::MenuModel& menu) : _menu(menu) {}

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
  //   no page             -> the menu handles it; consumed only if the menu was open AND the
  //                          key is one of the three the menu has an opinion about
  bool   routeKey(Key k, KeyEdge e);

 private:
  widget::MenuModel& _menu;
  IPage*             _currentPage = nullptr;
};

}  // namespace affa

#endif  // AFFA_PANEL_CARMINAT && AFFA_ENABLE_MENU
