// The Carminat / AFFA3 panel: the colour two-row window with a menu, popups, a fullscreen
// screen and a confirm box. Sync on 0x3AF/0x3CF, data on 0x151 and 0x1F1, keys in on
// 0x1C1.
//
// It supplies three things to AffaDisplayBase and nothing more:
//   * its SyncProfile and its function table (ORDER IS ON THE WIRE),
//   * its packet filler (0x00) and its key transmit id (0x1C1),
//   * the frame BUILDERS for every operation the family supports.
//
// AND IT HOLDS NO STATE AT ALL. Every builder below is a pure function of its arguments into
// a stack buffer, which is what makes a render safe to call from any task with nothing to
// check (docs/API.md §7). It used to own the menu renderer, model and page stack as
// well — a fourth bullet, and the only members it had; those are affa::CarminatMenu now, and
// the application owns one.
//
// The duplicated tick() sync machine is GONE — the base owns it, once, for both families.
// Every render goes through enqueue() with a RenderSlot so latest-value-wins coalescing
// works; nothing here calls _link.send() for a render.
//
// The application couplings the extracted class carried — NVS/Preferences, `extern bool
// _autoTime`, MediaInfo, the now-playing screen, MediaRouter, ANCS, ELM/DiagController,
// the analogRead voltage helper, sendPasswordSequence() and emulateKey() — are all gone.
// They are application policy: one car, one radio, one phone. A password sequence is
// application code built on the frame tap plus pressKey(..., KeySource::Wire), which is
// public API and produces the identical bytes.
#pragma once
#include "../AffaConfig.h"

#if AFFA_PANEL_CARMINAT

#include "../core/AffaDisplayBase.h"
#include "CarminatConstants.h"

// THE MENU IS NOT PART OF THIS LIBRARY ANY MORE. The state machine, its adapter and the
// affa::Menu / MenuItem / field-builder aliases are all gone: they were UI policy, and this
// is a transport. What a menu IS on this panel is four render calls below.
namespace affa {

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
  // so this changes nothing else. Found by a preemption bench, now pinned by test_latency.
  using AffaDisplayBase::onFrame;

  bool supports(Feature f) const override;

  // MEASURED AT THE GLASS, not taken from the builders. setText carries fourteen cells and
  // the panel shows about eight; `listItemChars` is 26 because docs/WIRE.md pins
  // item2 at 26 usable even though the builder accepts 30. A fitter that trusted the builder
  // would produce text that is correct in every test and cut off on hardware.
  PanelGeometry panelGeometry() const override {
    PanelGeometry g;
    g.mainChars     = 8;                          // 14 carried, ~8 visible
    g.menuRows      = 2;
    g.menuRowChars  = 26;
    g.infoRows      = 3;
    g.infoRowChars  = carminat::kInfoCells;       // 8
    g.listMaxItems  = carminat::kMenuMaxItems;    // 10; the OEM corpus shows 2, 4 and 6
    g.listItemChars = 26;
    g.fullRows      = 3;
    g.fullRowChars  = 26;
    g.imageWidth    = static_cast<uint8_t>(carminat::kNavWidth);
    g.imageHeight   = static_cast<uint8_t>(carminat::kNavHeight);
    return g;
  }

  // ---- IDisplay / IPanel rendering ----------------------------------------
  // Every one of these ENQUEUES and returns immediately. The Result is an acceptance
  // verdict — "was it queued?" — never a delivery verdict; that arrives through
  // onComplete(). `digit` is ignored on this panel; it exists for the UpdateList
  // signature.
  Submitted setText(const char* text, uint8_t digit = 255) override;

  // setText WITH THE HEADER EXPOSED. The plain override above hard-codes the four bytes
  // after the command; they are documented (MeganeCAN's CarminatDisplay.cpp, cross-checked
  // against the OEM capture in docs/NOTES.md) and an application that wants an
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
  Submitted setTextStyled(const char* text, uint8_t icon, uint8_t srcIcon,
                                     uint8_t fmt,
                                     uint8_t iconBank2 = carminat::kIconBank2);
  Submitted setTime(const char* hhmm) override;
  Submitted setPower(bool on) override;

  Submitted showMenu(const char* header, const char* row0, const char* row1,
                                uint8_t scrollIndicator = carminat::kScrollDown) override;

  // THE TWO-ROW MENU WITH A GUTTER GLYPH AND A SCROLLBAR. TWO SEPARATE FIELDS:
  //
  //   icon   payload [3] — an INDEX into a glyph table the panel owns. 0x00..0x10 blank,
  //          0x11 book-with-magnifier, 0x19 open book, 0x26 bluetooth, 0x30 GPS, 0x34
  //          aircraft, and populated uncatalogued runs between. Bit 7 set draws nothing.
  //   thumb  payload [4] — the SCROLLBAR THUMB POSITION. 0x00 no scrollbar, 0x10 top of
  //          travel, 0x58 bottom. Independent of `scrollIndicator` ([8], the arrows) and of
  //          showMenuN's `firstVisible` ([35]) — the panel derives none of the three from
  //          the others, so a scrolling application moves this itself.
  //
  // Both swept on the bench 2026-08-07; carminat::kMenuIcon* / kMenuThumb* is that table.
  //
  // The plain showMenu() override delegates here with kMenuIconOemBlank / kMenuThumbNone —
  // the bytes it always sent, so nothing on the wire moved. Note what that default IS:
  // `0x80 0x00` is glyph 0 with the suppress bit set and no scrollbar, doubly blank and
  // unreachable from any caller, which is the entire reason this panel looked for months as
  // though it had no icon-bearing list. The same trap setTextStyled() was built to open.
  Submitted showMenuIcon(const char* header, const char* row0, const char* row1,
                                    uint8_t scrollIndicator = carminat::kScrollDown,
                                    uint8_t icon = carminat::kMenuIconOemBlank,
                                    uint8_t thumb = carminat::kMenuThumbNone);

  Submitted highlightItem(uint8_t row) override;

  Submitted showPopupText(const char* text, uint8_t icon = carminat::kPopupIcon,
                                     uint8_t srcIcon = carminat::kSrcIconNone,
                                     uint8_t fmt = carminat::kFormatPlain) override;
  Submitted hidePopup() override;

  // NO hideFullscreenText(). A fullscreen is not an overlay: the next render replaces it,
  // measured on a real Carminat. hidePopup() is the only close this panel needs, and it is
  // the same `02 54 03` the removed method sent. Removed 2026-08-06.
  Submitted showFullscreenText(const char* l1, const char* l2,
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
  Submitted showMessageBox(const char* row0, const char* row1,
                                      const char* const* labels = nullptr,
                                      uint8_t buttonCount = carminat::kButtonsNone,
                                      uint8_t selected = 0);

  // A ONE-BUTTON OK BOX, and `caption` IS THE BUTTON'S LABEL — six characters, the size of
  // the field it lands in. That is what it always was; the name simply never said so, and
  // a 7th character used to spill into the body's first byte.
  Submitted showConfirmBox(const char* caption, const char* row0,
                                      const char* row1) override;

  // Move the selection between a message box's buttons: `03 29 05 <index>`. This is NOT
  // highlightItem(), which is the two-row list's `29 01 <rowtag>` form.
  Submitted selectBoxButton(uint8_t index);

  // showInfoPopup IS showInfoMenu with the OEM's default offsets — the same three 0x76
  // messages, not a second screen. Kept only as the IDisplay spelling; new code should call
  // showInfoMenu(), which is what the screen actually is.
  //
  // NO hideInfoPopup(). It never was a close command: it sent setText("RENAULT"), a guess
  // at the OEM's idle banner dressed as protocol. No capture shows how these rows are
  // dismissed, and inventing one is how a guess becomes a fact. Removed 2026-08-06.
  Submitted showInfoPopup(const char* l1, const char* l2,
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
  //
  // `icon` is the gutter glyph and `thumb` the scrollbar position — payload [3] and [4],
  // two unrelated fields; see showMenuIcon() above and the swept table in
  // CarminatConstants.h. THE THUMB MATTERS MOST HERE: this is the builder that scrolls a
  // long list, and `firstVisible` moves the viewport without moving the scrollbar.
  Submitted showMenuN(uint8_t* scratch, uint16_t cap, const char* title,
                                 const char* const* items, uint8_t count,
                                 uint8_t firstVisible = 0, uint8_t selected = 0,
                                 uint8_t scrollMask = 0,
                                 uint8_t icon = carminat::kMenuIconOemBlank,
                                 uint8_t thumb = carminat::kMenuThumbNone);

  // Move the selection inside a list already on screen — eight bytes instead of the whole
  // screen. highlightItem() is the two-row form and refuses anything past row 1; this takes
  // an ITEM INDEX, and the panel scrolls its two-row viewport to wherever it lands.
  Submitted selectMenuItem(uint8_t index);
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
  Submitted showNavBitmap(const uint8_t* bitmap);

  // THE SAME TRANSFER WITH THE HEADER EXPOSED — 14 bytes the caller supplies, in place of
  // carminat::kNavHeader. `bitmap` is still the 288-byte image and is still BORROWED.
  //
  // WHY RAW BYTES AND NOT NAMED PARAMETERS. Ten of those fourteen are UNMEASURED:
  //
  //   [2] [3]    `00 25` — unknown, possibly one 16-bit field
  //   [4..10]    `41 42 43 44 45 46 00` — a seven-byte slot holding "ABCDEF\0" in the
  //              capture, and CONFIRMED NOT TO BE TEXT: ASCII written here draws nothing
  //   [11]       `01` — unknown, format or bit depth
  //
  // showMenuIcon() got named parameters because a bench sweep had already established what
  // its two bytes DO. Naming these would be inventing vocabulary for bytes nobody has
  // measured, which is the mistake docs/NOTES.md keeps warning about. When a sweep
  // settles one, it earns a name and a constant, exactly as kMenuIcon* did.
  //
  // WHAT PROMPTED IT (owner, 2026-08-08): a stripe appears on the nav pane after a display
  // power-cycle followed by setText and then a bitmap, and no capture holds a second mode.
  // A byte in this header is the first suspect and there was no way to ask.
  //
  // [12] [13] are the geometry and MUST stay 48/48: the declared ISO-TP length is computed
  // from kNavBitmapBytes, so a header claiming a different size describes a payload that is
  // not there. Sweep [2], [3], [4..10] and [11]; leave the last two alone.
  Submitted showNavBitmapWithHeader(const uint8_t* header, const uint8_t* bitmap);

  // `25 00 00 00` / `25 00 03 00` — the OEM alternates these at 820 ms while the nav screen
  // is up, with nothing else on the bus. Almost certainly the blink of a flashing element.
  // Four bytes, verbatim from capture; what it actually does is NOT confirmed on glass.
  Submitted navTick(bool phase);
#endif

  // The offset-taking form of showInfoPopup, exposed because the three row slots and the
  // format prefix are the only part of this screen still being reverse-engineered. The
  // defaults reproduce the OEM settings list byte for byte. Sends ONE MESSAGE PER ROW —
  // three queue slots, and they deliberately do not coalesce against each other.
  Submitted showInfoMenu(const char* row0, const char* row1, const char* row2,
                                    uint8_t offset0 = carminat::kInfoOffset0,
                                    uint8_t offset1 = carminat::kInfoOffset1,
                                    uint8_t offset2 = carminat::kInfoOffset2,
                                    uint8_t infoPrefix = carminat::kInfoPrefix);

 protected:
  uint8_t  packetFiller() const override { return carminat::kFiller; }
  uint16_t keyTxId()      const override { return carminat::kIdKeyPressed; }
  bool shouldAutoAck(const Frame& f) const override {
    return f.id == carminat::kIdKeyPressed;
  }

  bool onFrame(const Frame& f) override;


 private:
  // enqueue() + "translate kNoTicket into the reason". Every builder ends in this.
  Submitted submit(uint16_t funcId, const uint8_t* data, uint8_t len,
                              RenderSlot slot, bool coalesce = (AFFA_TX_COALESCE != 0),
                              Priority priority = Priority::Normal,
                              bool reassertAfterSession = false);

  // NO MEMBERS. Every render above is a pure function of its arguments into a stack buffer,
  // which is what makes a call from any task safe with nothing to check — see
  // docs/API.md §7 The three that used to be here were the menu widget's, and it
  // is affa::CarminatMenu now.
};

}  // namespace affa

#endif  // AFFA_PANEL_CARMINAT
