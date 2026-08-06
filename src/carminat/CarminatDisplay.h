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
  // CapturedB0x3 is the default and matches the supplied OEM-radio monitor traces.  The
  // legacy option is intentionally explicit: it preserves a proven MeganeCAN 70/B0/B0
  // opening for panels that require it without weakening the strict 61 11 00 gate.
  CarminatDisplay(ICanLink& link, IClock& clock,
                  carminat::CarminatHelloProfile hello =
                      carminat::CarminatHelloProfile::CapturedB0x3);

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

  // setText WITH THE HEADER EXPOSED. The plain override above hard-codes the four bytes
  // after the command; they are documented (MeganeCAN's CarminatDisplay.cpp, cross-checked
  // against the OEM capture in docs/PROTOCOL-NOTES.md §17) and an application that wants an
  // icon on the main line has no way to ask for one otherwise.
  //
  //   icon     carminat::kIconsNone 0x55 / kIconsAfRds 0x45. The capture uses 0x09, which is
  //            in neither documented list.
  //   srcIcon  kSrcIconNone 0xFF / 0xDF "MANU" / 0xFD "PRESET"
  //   fmt      0x19-0x3F radio style: 5 digits + '.' + 1 char. 0x59-0x7F plain ASCII.
  //            kFormatPlain 0x60 is what the plain override sends.
  //   iconBank2 payload byte 4. Was hard-coded 0x55 in the builder and unreachable from any
  //            caller; kIconBank2 keeps that captured value, so the default changes nothing.
  //            EXPOSED BECAUSE AN UNREACHABLE BYTE HIDES A SYMPTOM — a CD icon that stays
  //            lit on the glass whatever `icon` is set to has nowhere else to be coming
  //            from, and a caller could not sweep the byte to find out. What its bits mean
  //            is still unknown; this makes the question askable, it does not answer it.
  //
  // Format 0x31 is why the OEM's "   1056 " is 105.6 FM and not the number 1056 — the point
  // is drawn between the digits by the panel, not by the sender.
  [[nodiscard]] Result setTextStyled(const char* text, uint8_t icon, uint8_t srcIcon,
                                     uint8_t fmt,
                                     uint8_t iconBank2 = carminat::kIconBank2);
  [[nodiscard]] Result setTime(const char* hhmm) override;
  [[nodiscard]] Result setPower(bool on) override;

  [[nodiscard]] Result showMenu(const char* header, const char* row0, const char* row1,
                                uint8_t scrollIndicator = carminat::kScrollDown) override;
  [[nodiscard]] Result highlightItem(uint8_t row) override;

  [[nodiscard]] Result showPopupText(const char* text, uint8_t icon = carminat::kPopupIcon,
                                     uint8_t srcIcon = carminat::kSrcIconNone,
                                     uint8_t fmt = carminat::kFormatPlain) override;
  [[nodiscard]] Result hidePopup() override;

  // NO hideFullscreenText(). A fullscreen is not an overlay: the next render replaces it,
  // measured on a real Carminat. hidePopup() is the only close this panel needs, and it is
  // the same `02 54 03` the removed method sent. Removed 2026-08-06.
  [[nodiscard]] Result showFullscreenText(const char* l1, const char* l2,
                                          const char* l3) override;

  // The message box, mode 0x05, with its BUTTONS AS A REAL FIELD. `labels` is
  // `buttonCount` NUL-terminated strings, each truncated to six bytes — the size of the
  // field the panel reads them from. buttonCount is 0, 1 or 2: no capture has ever shown a
  // third, and the declared length is 105 + 6 * buttonCount, so a third would be a guess
  // about a length as well as about a layout.
  //
  // `selected` is which button is preselected, and it is the same index `selectBoxButton()`
  // moves. With no buttons it is forced to 0xFF, as the OEM sends.
  //
  // Everything about the layout is in CarminatConstants.h under "The message box". The
  // two-button form is 119 wire bytes and wraps the ISO-TP counter to 0x20 — which is what
  // the OEM's own `CONFIRM SCREEN NO.csv` does, and what the nav pane has done 24 912 times
  // on this bench.
  [[nodiscard]] Result showMessageBox(const char* row0, const char* row1,
                                      const char* const* labels = nullptr,
                                      uint8_t buttonCount = carminat::kButtonsNone,
                                      uint8_t selected = 0);

  // A ONE-BUTTON OK BOX, and `caption` IS THE BUTTON'S LABEL — six characters, the size of
  // the field it lands in. That is what it always was; the name simply never said so, and
  // a 7th character used to spill into the body's first byte.
  [[nodiscard]] Result showConfirmBox(const char* caption, const char* row0,
                                      const char* row1) override;

  // Move the selection between a message box's buttons: `03 29 05 <index>`. This is NOT
  // highlightItem(), which is the two-row list's `29 01 <rowtag>` form.
  [[nodiscard]] Result selectBoxButton(uint8_t index);

  // showInfoPopup IS showInfoMenu with the OEM's default offsets — the same three 0x76
  // messages, not a second screen. Kept only as the IDisplay spelling; new code should call
  // showInfoMenu(), which is what the screen actually is.
  //
  // NO hideInfoPopup(). It never was a close command: it sent setText("RENAULT"), a guess
  // at the OEM's idle banner dressed as protocol. No capture shows how these rows are
  // dismissed, and inventing one is how a guess becomes a fact. Removed 2026-08-06.
  [[nodiscard]] Result showInfoPopup(const char* l1, const char* l2,
                                     const char* l3) override;

#if AFFA_ENABLE_BIGMENU
  // Wire bytes an N-item list screen occupies, PCI included. 2 + 36 + 27*count.
  static constexpr uint16_t menuScreenBytes(uint8_t count) {
    return static_cast<uint16_t>(2 + carminat::kMenuFixedBytes +
                                 carminat::kMenuItemStride * count);
  }

  // The list screen with as many items as the panel will TRACK. It still DRAWS TWO ROWS —
  // the glass is a two-row viewport — but the panel holds the whole list and scrolls itself
  // as the selection moves, so the application stops maintaining a sliding window. `items` is `count` NUL-terminated strings, each
  // truncated to 26 characters.
  //
  // `scratch` is BUILT INTO AND THEN BORROWED: it must be at least menuScreenBytes(count)
  // and must stay valid and unchanged until the ticket completes — lastEnqueued() names
  // that ticket. It lives with the caller because a six-item screen is 200 bytes and the
  // library refuses to spend that on every consumer for a screen most will never draw.
  //
  // `firstVisible` scrolls a short window over a long list; `selected` is the row tag of the
  // highlighted item; `scrollMask` is 0x00 none / 0x07 up / 0x0B down / 0x03.
  [[nodiscard]] Result showMenuN(uint8_t* scratch, uint16_t cap, const char* title,
                                 const char* const* items, uint8_t count,
                                 uint8_t firstVisible = 0, uint8_t selected = 0,
                                 uint8_t scrollMask = 0);

  // Move the selection inside a list already on screen — eight bytes instead of the whole
  // screen. highlightItem() is the two-row form and refuses anything past row 1; this takes
  // an ITEM INDEX, and the panel scrolls its two-row viewport to wherever it lands.
  [[nodiscard]] Result selectMenuItem(uint8_t index);
#endif

#if AFFA_ENABLE_NAV
  // The 48x48 navigation pane on 0x1F1. `bitmap` is carminat::kNavBitmapBytes (288) of
  // row-major, MSB-first, 6-bytes-per-row monochrome — bit 7 of byte 0 is the top-left
  // pixel. tools/gen_navicons.js in the repo builds them.
  //
  // BORROWED, NOT COPIED: the bytes must stay valid and unchanged until the ticket
  // completes. A `const uint8_t[288]` in flash is the intended case and costs no RAM.
  //
  // The pane is a LAYER. It survives a menu, an info menu draws alongside it, and it can be
  // replaced underneath an open popup — so this is a widget you set once and update, not a
  // screen you switch to. [BENCH 2026-08-05]
  [[nodiscard]] Result showNavBitmap(const uint8_t* bitmap);

  // `25 00 00 00` / `25 00 03 00` — the OEM alternates these at 820 ms while the nav screen
  // is up, with nothing else on the bus. Almost certainly the blink of a flashing element.
  // Four bytes, verbatim from capture; what it actually does is NOT confirmed on glass.
  [[nodiscard]] Result navTick(bool phase);
#endif

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
  bool shouldAutoAck(const Frame& f) const override {
    return f.id == carminat::kIdKeyPressed;
  }

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
                              RenderSlot slot, bool coalesce = (AFFA_TX_COALESCE != 0),
                              Priority priority = Priority::Normal,
                              bool reassertAfterSession = false);

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
