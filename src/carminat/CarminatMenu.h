// The Carminat menu, as a WIDGET THE APPLICATION OWNS.
//
//   affa::CarminatDisplay display(link, clock);
//   affa::CarminatMenu    menu(display);          // <- the app owns it, not the display
//
//   void onKey(affa::Key k, affa::KeyEdge e, void*) {
//     if (menu.routeKey(k, e)) return;            // the menu had an opinion
//     …your own handling
//   }
//
//   display.onKey(&onKey, nullptr);
//   display.begin();
//
// WHY IT MOVED OUT OF THE PANEL. Until 2.0 `CarminatDisplay` held the renderer, the model
// and the page controller as members, and `AffaDisplayBase` carried three virtual seams
// (`menuOpen`, `openMenu`, `routeKeyToMenu`) plus a hotkey triple so it could drive them.
// That made a CAN driver the owner of a UI state machine, and it was the ONLY mutable state
// `CarminatDisplay` had — every one of its twenty-two renders is a pure byte-builder.
// Removing it leaves the panel stateless, which is what lets a render called from any task
// be safe with nothing to check.
//
// It also put the cost in the wrong place: every panel paid for three virtuals and three
// members whether or not it had a menu, and `AFFA_ENABLE_MENU` defaulted to 0 — so the
// common build carried the seams for a feature it had switched off.
//
// This is the same arrangement `Marquee` and `RowScreen` already had, stated in
// AffaDisplay.h: a widget transmits nothing and holds no display; the application samples
// it and the render primitive does the rest. The menu was the one widget that got embedded.
//
// WHAT THIS CLASS IS: the three objects, wired, plus the two pieces of POLICY the base used
// to own — the gesture that opens the menu, and the page stack's first claim on a key. It
// builds no frames itself; every byte still leaves through IPanel.
#pragma once
#include "../AffaConfig.h"

#if AFFA_PANEL_CARMINAT && AFFA_ENABLE_MENU

#include "../core/AffaTypes.h"
#include "../core/IPanel.h"
#include "../widget/MenuModel.h"
#include "CarminatMenuRenderer.h"
#include "MenuController.h"
#include "IPage.h"

namespace affa {

class CarminatMenu {
 public:
  // `panel` is normally the CarminatDisplay, taken as IPanel because four rendering calls
  // are all this needs. It must outlive the menu.
  //
  // The menu starts EMPTY, on purpose. What the items are called, what a field means and
  // whether a change is written to NVS is the application's business: fill it through
  // model(). See docs/MENU-WIDGET.md.
  explicit CarminatMenu(IPanel& panel, const char* header = "Main Menu",
                        bool coalesceHighlight = true)
      : _renderer(panel, coalesceHighlight),
        _model(_renderer, CarminatMenuRenderer::geometry(), header),
        _ctrl(_model),
        _panel(panel) {
    // The OEM convention when the menu closes: the panel goes back to the source banner.
    // A default because it is the convention, replaceable because it is policy —
    // model().onClose() overwrites it.
    _model.onClose(&CarminatMenu::onClosed, this);
  }

  CarminatMenu(const CarminatMenu&)            = delete;
  CarminatMenu& operator=(const CarminatMenu&) = delete;

  widget::MenuModel&          model()          { return _model; }
  const widget::MenuModel&    model() const    { return _model; }
  // Exposed for the one thing the model deliberately cannot answer — lastResult(), the
  // panel's verdict on the last redraw — and for tracing.
  CarminatMenuRenderer&       renderer()       { return _renderer; }
  const CarminatMenuRenderer& renderer() const { return _renderer; }

  // The page stack. The application owns every page; pushing one gives it every key until
  // it is popped, INCLUDING the hotkey.
  void   pushPage(IPage* p)  { _ctrl.pushPage(p); }
  void   popPage()           { _ctrl.popPage(); }
  IPage* currentPage() const { return _ctrl.currentPage(); }

  // The active page's own tick. Call it from your loop. It is NOT a period — a page that
  // needs one compares against its own clock — and nothing here counts calls.
  //
  // It used to be called from CarminatDisplay::onPoll(), i.e. from the poll task. Being on
  // the application's task instead is the point: a page that does slow work can no longer
  // stall the protocol with it.
  void tick() { _ctrl.tickCurrentPage(); }

  bool isOpen() const { return _model.isOpen(); }

  // Open the menu regardless of the hotkey — the INTENT, not the gesture. False when an
  // active page owns the keys, or when the menu is empty (an open menu that renders nothing
  // would eat every key and show the user nothing).
  bool open() {
    if (_ctrl.currentPage() != nullptr) return false;
    _model.open();
    return _model.isOpen();
  }

  // THE ONE CALL AN APPLICATION NEEDS, from its KeyCb. True means the menu consumed the key
  // and the application should do nothing with it.
  //
  // The order is the base's old routeKey(), moved here verbatim: the hotkey opens the menu
  // if nothing is open, then the page stack and the model get their turn. Keys neither has
  // an opinion about — SrcNext, VolUp, Pause — return false and are yours, even while the
  // menu is open. That last part is load-bearing; routing every key into the model would
  // swallow them.
  bool routeKey(Key k, KeyEdge e) {
    bool consumed = false;
    if (_hotkeyOn && k == _hotkey && e == _hotkeyEdge && !isOpen()) consumed = open();
    if (!consumed) consumed = _ctrl.routeKey(k, e);
    return consumed;
  }

  // Drive the menu by INTENT rather than by gesture — for a console, a test, or a remote
  // whose buttons are not this panel's. True when the intent was consumed.
  //
  // It maps to a key and goes through routeKey() rather than poking the model, and that is
  // deliberate: an active page must get its turn, exactly as it does for a real press.
  // `Open` is the exception — it is the intent, so it ignores the hotkey setting, which is
  // what makes clearHotkey() leave this as the only way in.
  //
  // This was AffaDisplayBase::nav(NavCommand, KeySource) until 2.0. The KeySource half is
  // not reproduced: putting a phantom key on the BUS is the display's job and is still
  // display.pressKey(k, e, KeySource::Wire). Mixing "drive my menu" and "impersonate the
  // panel at another radio" into one call meant Increase and Decrease had to return
  // NotSupported for a wire source, because a held detent has no wire representation.
  bool nav(NavCommand c) {
    switch (c) {
      case NavCommand::Open:     return open();
      case NavCommand::Back:     return routeKey(Key::Load,     KeyEdge::Hold);
      case NavCommand::Select:   return routeKey(Key::Load,     KeyEdge::Click);
      case NavCommand::Next:     return routeKey(Key::RollDown, KeyEdge::Click);
      case NavCommand::Prev:     return routeKey(Key::RollUp,   KeyEdge::Click);
      case NavCommand::Increase: return routeKey(Key::RollDown, KeyEdge::Hold);
      case NavCommand::Decrease: return routeKey(Key::RollUp,   KeyEdge::Hold);
    }
    return false;
  }

  // The gesture that opens the menu — UI policy, not wire format. Affects OPENING ONLY;
  // once open, key routing is not configurable. open() works regardless, so clearing the
  // hotkey makes it the only way in.
  void setHotkey(Key k, KeyEdge e) { _hotkey = k; _hotkeyEdge = e; _hotkeyOn = true; }
  void clearHotkey()               { _hotkeyOn = false; }
  bool hotkey(Key& k, KeyEdge& e) const {
    if (!_hotkeyOn) return false;
    k = _hotkey; e = _hotkeyEdge;
    return true;
  }

 private:
  static void onClosed(void* ctx) {
    // Deliberately dropped: this is a CloseCb with nowhere to report to, and the banner is
    // cosmetic. An application that needs the verdict installs its own onClose().
    (void)static_cast<CarminatMenu*>(ctx)->_panel.setText("RENAULT", 0);
  }

  // DECLARATION ORDER IS CONSTRUCTION ORDER AND IT IS LOAD-BEARING: the model binds the
  // renderer and the controller binds the model, so neither may be declared before what it
  // references.
  CarminatMenuRenderer _renderer;
  widget::MenuModel    _model;
  MenuController       _ctrl;
  IPanel&              _panel;

  Key     _hotkey     = Key::Load;      // the OEM default, replaceable
  KeyEdge _hotkeyEdge = KeyEdge::Hold;
  bool    _hotkeyOn   = true;
};

}  // namespace affa

#endif  // AFFA_PANEL_CARMINAT && AFFA_ENABLE_MENU
