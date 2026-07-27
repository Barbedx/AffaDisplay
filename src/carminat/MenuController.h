// The Carminat navigation stack: the active full-screen page, and key routing between
// page / menu / fall-through. Holds the menu MODEL by reference (CarminatDisplay owns it),
// owns no pages, and builds no frames — pages render through their own IPanel.
//
// Separate from widget::MenuModel because this is NAVIGATION POLICY, which an application
// with a different remote replaces; the model has no key vocabulary at all (six intents,
// no Key enum) so that this layer can go without touching it.
//
// THE KEY MAP:
//     RollUp   click -> prev()        hold -> decrease()
//     RollDown click -> next()        hold -> increase()
//     Load     click -> select()      hold -> back()   (closes, even from inside edit mode)
//     anything else                   -> NOT consumed
//
// The last line is load-bearing: SrcNext, SrcPrev, VolUp, VolDown and Pause must reach the
// application even while the menu is open, and routeKey's false is what falls through to
// KeyCb. Routing every key into the model would swallow them.
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
