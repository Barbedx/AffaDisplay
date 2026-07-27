// The sliding-window menu, with the panel taken out of it.
//
// This is the former affa::Menu (src/carminat/Menu/, since DELETED — this file replaced it
// rather than joining it) with exactly one change of substance: the two rows of twenty-six
// characters became a MenuGeometry, and the IPanel became an IMenuRenderer. On the Carminat
// the adapter is src/carminat/CarminatMenuRenderer, and CarminatDisplay::getMenu() hands out
// one of these. The state machine is otherwise carried across behaviour for behaviour —
// the window/selectedRow arithmetic, the scroll-arrow derivation and its deliberate
// "no arrows when the list fits", the coarse step on a Hold edge, the clamps, the list-field
// bound that keeps BOTH the first and the last entry reachable, and Select walking
// field 0 -> 1 -> 2 -> out. Where the original had a comment explaining a decision, the
// comment came with it.
//
// WHAT MOVED OUT. The model speaks row INDEX (0..rows-1); the Carminat row tags 0x7E/0x7F,
// the highlight frame, the 96-byte screen, the charset of the glass and the cost of a
// message are the adapter's, behind IMenuRenderer. What did NOT move out is the KNOWLEDGE of
// which redraws are cheap: the model still tells the renderer when only the lit row changed
// (IMenuRenderer::highlightOnly), because that is a fact about the state machine and every
// adapter would otherwise have to rediscover it. The model no longer returns Result:
// whether a frame reached the panel is not something a UI state machine can act on, and the
// adapter is the only layer that can — on this panel that is
// CarminatDisplay::menuRenderer().lastResult().
//
// CONSTRAINTS THIS FILE HOLDS ITSELF TO: no Arduino, no panel header, no CAN, no heap after
// construction, no std::function. Capacities come from AffaConfig (AFFA_MENU_MAX_ITEMS,
// AFFA_MENU_MAX_FIELDS, AFFA_MENU_ROW_MAX). It compiles and is meant to be tested on the
// host.
//
// `label`, `unit` and every string in `list` are CALLER-OWNED and must outlive the model —
// they are pointed at, never copied. String literals and static tables are the intended
// sources.
#pragma once
#include "../AffaConfig.h"

#if AFFA_ENABLE_MENU

#include "IMenuRenderer.h"
#include "MenuGeometry.h"
#include <cstddef>
#include <cstdint>

namespace affa {
namespace widget {

enum class FieldType : uint8_t { Integer, List };

// One editable value inside a menu item. Fixed layout; `value` is the integer for
// FieldType::Integer and the INDEX for FieldType::List.
struct Field {
  FieldType type           = FieldType::Integer;
  int32_t   value          = 0;
  int32_t   minValue       = 0;
  int32_t   maxValue       = 0;
  int32_t   step           = 1;
  int32_t   stepMultiplier = 1;   // multiplies `step` on a Hold edge (increase()/decrease()).
                                  // A held detent has no wire representation on the Carminat
                                  // remote, so on that panel this is reachable only through
                                  // a synthesised key — see docs/API.md §8.3.
  const char*        unit  = nullptr;   // Integer only, may be null
  const char* const* list  = nullptr;   // List only
  uint8_t   listCount      = 0;
  bool      readOnly       = false;     // a live reading: rendered, never edited
};

// The whole item-construction API an application needs.
Field integerField(int32_t value, int32_t min, int32_t max,
                   int32_t step = 1, int32_t stepMultiplier = 10,
                   const char* unit = nullptr);
Field readOnlyField(int32_t value, const char* unit = nullptr);
Field listField(const char* const* values, uint8_t count, uint8_t index = 0);

struct MenuItem {
  const char* label = nullptr;                  // caller-owned, must outlive the model
  Field    fields[AFFA_MENU_MAX_FIELDS];
  uint8_t  fieldCount = 0;                      // extras are dropped at addItem()
  char     separator  = ' ';                    // before each rendered field value
  bool     editable   = true;                   // false -> select() does nothing

  // Fired AFTER a field's value changed, from whatever task called the navigation method.
  // Never from an interrupt. It is BOTH the per-item and the per-field hook: the item comes
  // by reference and the field that moved comes as an index, so one function pointer covers
  // "this item changed" and "field 1 of this item changed". Put your NVS write here —
  // persistence is the application's.
  void (*onChange)(const MenuItem& item, uint8_t fieldIndex, void* ctx) = nullptr;

  // If set, select() on this item calls this INSTEAD of entering edit mode.
  void (*onActivate)(void* ctx) = nullptr;

  void* ctx = nullptr;
};

class MenuModel {
 public:
  using CloseCb = void (*)(void* ctx);

  // `geom` is copied and sanitised: rows below 1 becomes 1, rowChars is clamped to
  // AFFA_MENU_ROW_MAX - 1 because the row buffer is a fixed array.
  MenuModel(IMenuRenderer& renderer, const MenuGeometry& geom, const char* header);

  MenuModel(const MenuModel&)            = delete;
  MenuModel& operator=(const MenuModel&) = delete;

  // ---- content, owned by the application ----------------------------------
  void      setHeader(const char* h);
  int       addItem(const MenuItem& item);   // index, or -1 if full
  MenuItem* item(uint8_t index);             // nullptr if out of range
  uint8_t   count() const { return _count; }
  void      clear();                         // also closes the menu, silently: it does NOT
                                             // fire CloseCb, because tearing the content
                                             // down is not the user leaving

  // Change a value from outside (a sensor, a web request). Fires MenuItem::onChange and
  // redraws ONLY if the item is inside the visible window.
  bool      setFieldValue(uint8_t itemIndex, uint8_t fieldIndex, int32_t value);

  void      onClose(CloseCb cb, void* ctx);

  // ---- state --------------------------------------------------------------
  bool    isOpen()      const { return _open; }
  bool    isEditing()   const { return _editing; }
  uint8_t selectedIndex() const { return _selectedIndex; }
  uint8_t selectedRow() const { return _selectedRow; }    // 0 = top row of the window
  uint8_t editingField() const { return _editingField; }
  uint8_t topIndex()    const;                            // item shown on row 0
  const MenuGeometry& geometry() const { return _geom; }

  // One of widget::Scroll, derived from the window position — docs/API.md §8.6.
  uint8_t scrollMask() const;

  // ---- navigation ----------------------------------------------------------
  // The surface the Carminat widget had, one method per intent instead of one switch over
  // (Key, KeyEdge): an adapter maps its own keys onto these. Each returns true when the
  // model consumed the intent — i.e. false means "the menu was closed, this key is the
  // application's", which is the contract Menu::handleKey had.
  //
  // next/prev are the CLICK edge and increase/decrease are the HOLD edge of the same wheel:
  // while editing, hold applies Field::stepMultiplier; while not editing, both simply move
  // the selection, exactly as Menu::handleKey did.
  bool next();       // wheel down:  edit ? +1 step     : selection down
  bool prev();       // wheel up:    edit ? -1 step     : selection up
  bool increase();   // wheel down held: edit ? +coarse : selection down
  bool decrease();   // wheel up   held: edit ? -coarse : selection up
  bool select();     // activate item / enter edit / advance to the next field
  bool back();       // leave the menu

  void open();       // no-op on an EMPTY menu: an open menu that renders nothing would eat
                     // every key
  void close();      // clears editing state, then fires CloseCb

  // ---- rendering -----------------------------------------------------------
  // Re-emit the whole window through the renderer. The navigation methods call it for you;
  // call it yourself after rebuilding content while open, or from an adapter that lost the
  // glass to a popup.
  void render();

  // The text of ONE ITEM (not one row), truncated to outSize - 1 characters and always
  // NUL-terminated. An out-of-range index yields an EMPTY string rather than reading past
  // the array: the bottom row of a one-item menu is a legitimate call. Public because an
  // adapter that measures or re-lays-out text needs it, and because it is the cheapest
  // thing to unit-test.
  void rowText(uint8_t itemIndex, char* out, size_t outSize) const;

 private:
  // The selection moved INSIDE the window: same items, same arrows, a different lit row.
  // Offers the renderer the cheap path (IMenuRenderer::highlightOnly) and falls back to a
  // whole frame when it declines. Every OTHER redraw goes straight to render() — this is
  // reached only from the two branches that provably left the window where it was.
  void renderSelectionMove();
  void selectNext();
  void selectPrev();
  void enterEdit();
  void nextFieldOrExit();
  void editField(int32_t delta, bool hold);

  IMenuRenderer& _renderer;
  MenuGeometry   _geom;
  const char*    _header;
  MenuItem       _items[AFFA_MENU_MAX_ITEMS];
  uint8_t        _count         = 0;
  uint8_t        _selectedIndex = 0;
  uint8_t        _selectedRow   = 0;
  uint8_t        _editingField  = 0;
  bool           _open          = false;
  bool           _editing       = false;
  CloseCb        _closeCb       = nullptr;
  void*          _closeCtx      = nullptr;
};

}  // namespace widget
}  // namespace affa

#endif  // AFFA_ENABLE_MENU
