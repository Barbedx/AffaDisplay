// The Carminat / AFFA3 panel: the colour two-row window with a menu, popups, a fullscreen
// screen and a confirm box. Sync on 0x3AF/0x3CF, data on 0x151 and 0x1F1, keys in on
// 0x1C1.
//
// It supplies four things to AffaDisplayBase and nothing more:
//   * its SyncProfile and its function table (ORDER IS ON THE WIRE),
//   * its packet filler (0x00) and its key transmit id (0x1C1),
//   * the frame BUILDERS for every operation the family supports,
//   * the menu seams (menuOpen / openMenu / routeKeyToMenu) and the page stack.
//
// The duplicated tick() sync machine is GONE — the base owns it, once, for both families.
// Every render goes through enqueue() with a RenderSlot so latest-value-wins coalescing
// works; nothing here calls _link.send() for a render.
//
// The application couplings the extracted class carried — NVS/Preferences, `extern bool
// _autoTime`, MediaInfo, the now-playing screen, MediaRouter, ANCS, ELM/DiagController,
// the analogRead voltage helper, sendPasswordSequence() and emulateKey() — are all gone.
// They are application policy: one car, one radio, one phone. The password sequence is
// rebuilt as examples/08_radio_mitm against subscribe() + pressKey(..., KeySource::Wire),
// which is public API and produces the identical bytes.
#pragma once
#include "../AffaConfig.h"

#if AFFA_PANEL_CARMINAT

#include "../core/AffaDisplayBase.h"
#include "CarminatConstants.h"

#if AFFA_ENABLE_MENU
#  include "../widget/MenuModel.h"
#  include "CarminatMenuRenderer.h"
#  include "MenuController.h"
#  include "IPage.h"
#endif

namespace affa {

#if AFFA_ENABLE_MENU
// THE TYPE BEHIND getMenu() CHANGED, AND THESE NAMES DID NOT.
//
// `affa::Menu` used to be src/carminat/Menu/Menu.{h,cpp} — a two-row sliding-window menu with
// the panel welded into it (row0/row1, a fixed pair of char[AFFA_MENU_ROW_MAX], the count<=2
// scroll rule, IPanel calls in the middle of the state machine). That file is GONE. The same
// state machine now lives once, in src/widget/MenuModel, with the panel behind IMenuRenderer;
// the Carminat side of the seam is CarminatMenuRenderer, above.
//
// The names stay because they are the application-facing API: docs/API.md §2.12 and every
// example spell `affa::Menu`, `affa::MenuItem` and the three field builders. They are ALIASES,
// not a compatibility layer — there is exactly one implementation and these are its other
// spelling. Two differences a caller can observe:
//
//   * MenuModel::render() returns void. Whether a frame reached the panel is not something a
//     UI state machine can act on; the adapter is the layer that can, so the verdict is
//     menuRenderer().lastResult().
//   * the geometry is injected (CarminatMenuRenderer::geometry(), 2 x 26), so rows are
//     truncated at 26 characters rather than at AFFA_MENU_ROW_MAX - 1.
//
// New code should prefer the widget:: spelling; it is the one that works on a panel that is
// not this one.
using Menu = widget::MenuModel;
using widget::Field;
using widget::FieldType;
using widget::MenuItem;
using widget::integerField;
using widget::readOnlyField;
using widget::listField;
#endif

class CarminatDisplay final : public AffaDisplayBase {
 public:
  CarminatDisplay(ICanLink& link, IClock& clock);

  // NAME HIDING, not decoration. The protected `bool onFrame(const Frame&)` hook below
  // hides EVERY base member called onFrame, including the public Layer-0 tap
  // `void onFrame(FrameTap, void*)` — so without this line `display.onFrame(&tap, ctx)`
  // fails to compile through a CarminatDisplay& and only works through an
  // AffaDisplayBase&. The derived override still hides the base's same-signature member,
  // so this changes nothing else. Found by examples/06_counter_preempt.
  using AffaDisplayBase::onFrame;

  bool supports(Feature f) const override;

  // ---- IDisplay / IPanel rendering ----------------------------------------
  // Every one of these ENQUEUES and returns immediately. The Result is an acceptance
  // verdict — "was it queued?" — never a delivery verdict; that arrives through
  // onComplete(). `digit` is ignored on this panel; it exists for the UpdateList
  // signature.
  [[nodiscard]] Result setText(const char* text, uint8_t digit = 255) override;
  [[nodiscard]] Result setTime(const char* hhmm) override;
  [[nodiscard]] Result setPower(bool on) override;

  [[nodiscard]] Result showMenu(const char* header, const char* row0, const char* row1,
                                uint8_t scrollIndicator = carminat::kScrollDown) override;
  [[nodiscard]] Result highlightItem(uint8_t row) override;

  [[nodiscard]] Result showPopupText(const char* text, uint8_t icon = carminat::kPopupIcon,
                                     uint8_t srcIcon = carminat::kSrcIconNone,
                                     uint8_t fmt = carminat::kFormatPlain) override;
  [[nodiscard]] Result hidePopup() override;

  [[nodiscard]] Result showFullscreenText(const char* l1, const char* l2,
                                          const char* l3) override;
  [[nodiscard]] Result hideFullscreenText() override;

  [[nodiscard]] Result showConfirmBox(const char* caption, const char* row0,
                                      const char* row1) override;

  [[nodiscard]] Result showInfoPopup(const char* l1, const char* l2,
                                     const char* l3) override;
  [[nodiscard]] Result hideInfoPopup() override;

  // The offset-taking form of showInfoPopup, exposed because the three row slots and the
  // format prefix are the only part of this screen still being reverse-engineered. The
  // defaults reproduce the OEM settings list byte for byte. Sends ONE MESSAGE PER ROW —
  // three queue slots, and they deliberately do not coalesce against each other.
  [[nodiscard]] Result showInfoMenu(const char* row0, const char* row1, const char* row2,
                                    uint8_t offset0 = carminat::kInfoOffset0,
                                    uint8_t offset1 = carminat::kInfoOffset1,
                                    uint8_t offset2 = carminat::kInfoOffset2,
                                    uint8_t infoPrefix = carminat::kInfoPrefix);

#if AFFA_ENABLE_MENU
  // The library hands out an EMPTY menu with a header; the application fills it. See
  // docs/API.md §8.7 for the complete item-building example — nothing else is needed and
  // no library internal is touched.
  //
  // THE NAME IS THE SAME AND THE TYPE IS NOT: this used to return `affa::Menu&`, the Carminat
  // widget; it now returns the display-agnostic model that replaced it. See the alias block
  // above for what a caller can observe.
  widget::MenuModel& getMenu() { return _menu; }

  // The adapter between that model and this panel's glass. Exposed for the one thing the
  // model deliberately cannot answer — lastResult(), the panel's verdict on the last redraw —
  // and for tracing (lastWasHighlightOnly(), the row text as it went out).
  CarminatMenuRenderer&       menuRenderer()       { return _menuRenderer; }
  const CarminatMenuRenderer& menuRenderer() const { return _menuRenderer; }

  // The page stack. The application owns every page; pushing one gives it every key until
  // it is popped, including the menu hotkey.
  void  pushPage(IPage* p) { _menuCtrl.pushPage(p); }
  void  popPage()          { _menuCtrl.popPage(); }
  IPage* currentPage() const { return _menuCtrl.currentPage(); }
#endif

 protected:
  uint8_t  packetFiller() const override { return carminat::kFiller; }
  uint16_t keyTxId()      const override { return carminat::kIdKeyPressed; }

  bool onFrame(const Frame& f) override;
  void onPoll() override;

#if AFFA_ENABLE_ISOTP_RX
  // 0x151 is the id WE render on, so inbound text there is another head unit's — the only
  // way text arrives at a node in the radio role. Feeds onText().
  uint16_t textRxId() const override { return carminat::kIdSetText; }
  bool decodeText(const uint8_t* payload, uint8_t len, char* out,
                  uint8_t outSize) const override;
#endif

#if AFFA_ENABLE_MENU
  bool menuOpen() const override;
  bool openMenu() override;
  bool routeKeyToMenu(Key k, KeyEdge e) override;
#endif

 private:
  // enqueue() + "translate kNoTicket into the reason". Every builder ends in this.
  [[nodiscard]] Result submit(uint16_t funcId, const uint8_t* data, uint8_t len,
                              RenderSlot slot, bool coalesce = (AFFA_TX_COALESCE != 0));

#if AFFA_ENABLE_MENU
  void initializeMenu();                    // creates an EMPTY menu; the app fills it
  static void onMenuClosed(void* ctx);      // -> setText("RENAULT", 0), the OEM default

  // DECLARATION ORDER IS CONSTRUCTION ORDER, and the model binds a reference to the renderer:
  // the renderer must be declared first, and the controller last because it binds to the
  // model.
  CarminatMenuRenderer _menuRenderer;
  widget::MenuModel    _menu;
  MenuController       _menuCtrl;
#endif
};

}  // namespace affa

#endif  // AFFA_PANEL_CARMINAT
