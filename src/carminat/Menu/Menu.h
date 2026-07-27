// The two-row sliding-window menu the Carminat panel renders.
//
// WHY THIS IS IN THE LIBRARY AT ALL (docs/API.md §8.2): none of the mechanism is
// separable from the wire. A menu screen is one 96-byte ISO-TP payload with the header at
// a fixed offset, row 0 at offset 37 behind `00 7E` and row 1 at offset 64 behind
// `01 7F`; the highlight is a DIFFERENT single frame; and the scroll-arrow byte is a
// function of the selection's position in the list. Deciding "does this key redraw or
// just re-highlight" is reasoning about the wire, not about the application.
//
// The application owns the CONTENT: labels, fields, what a change means, and persistence.
//
// No Arduino String, no std::vector, no std::function, no allocation. `label`, `unit` and
// every string in `list` are CALLER-OWNED and must outlive the Menu — they are pointed
// at, never copied. String literals and static tables are the intended sources.
#pragma once
#include "../../AffaConfig.h"

#if AFFA_PANEL_CARMINAT && AFFA_ENABLE_MENU

#include "../../core/AffaTypes.h"
#include "../../core/IPanel.h"
#include <cstddef>
#include <cstdint>

namespace affa {

enum class FieldType : uint8_t { Integer, List };

// One editable value inside a menu item. Fixed layout; `value` is the integer for
// FieldType::Integer and the INDEX for FieldType::List.
struct Field {
  FieldType type           = FieldType::Integer;
  int32_t   value          = 0;
  int32_t   minValue       = 0;
  int32_t   maxValue       = 0;
  int32_t   step           = 1;
  int32_t   stepMultiplier = 1;   // multiplies `step` on a Hold edge (Increase/Decrease).
                                  // A held detent has NO wire representation, so this is
                                  // reachable only through nav()/pressKey() with a source
                                  // that includes Local — see docs/API.md §8.3.
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
  const char* label = nullptr;                  // caller-owned, must outlive the Menu
  Field    fields[AFFA_MENU_MAX_FIELDS];
  uint8_t  fieldCount = 0;                      // extras are dropped at addItem()
  char     separator  = ' ';                    // before each rendered field value
  bool     editable   = true;                   // false -> Select does nothing

  // Fired AFTER a field's value changed, from the task that called poll()/pressKey().
  // Never from an interrupt. Put your NVS write here — persistence is the application's.
  void (*onChange)(const MenuItem& item, uint8_t fieldIndex, void* ctx) = nullptr;

  // If set, a Select (click-Load) on this item calls this INSTEAD of entering edit mode.
  void (*onActivate)(void* ctx) = nullptr;

  void* ctx = nullptr;
};

class Menu {
 public:
  using CloseCb = void (*)(void* ctx);

  Menu(IPanel& panel, const char* header);

  Menu(const Menu&)            = delete;
  Menu& operator=(const Menu&) = delete;

  // ---- content, owned by the application ----------------------------------
  void      setHeader(const char* h);
  int       addItem(const MenuItem& item);   // index, or -1 if full
  MenuItem* item(uint8_t index);             // nullptr if out of range
  uint8_t   count() const;
  void      clear();                         // also closes the menu, silently: it does
                                             // NOT fire CloseCb, because tearing the
                                             // content down is not the user leaving

  // Change a value from outside (a sensor, a web request). Fires MenuItem::onChange and
  // re-renders ONLY if the item is in the visible two-row window.
  bool      setFieldValue(uint8_t itemIndex, uint8_t fieldIndex, int32_t value);

  void      onClose(CloseCb cb, void* ctx);   // CarminatDisplay installs
                                              // setText("RENAULT", 0), as today

  // ---- state --------------------------------------------------------------
  bool    isOpen()        const { return _open; }
  bool    isEditing()     const { return _editing; }
  uint8_t selectedIndex() const { return _selectedIndex; }
  uint8_t selectedRow()   const { return _selectedRow; }   // 0 = top, 1 = bottom

  // ---- driven by AffaDisplayBase, not by the application -------------------
  bool   handleKey(Key k, KeyEdge e);   // true = consumed; false falls through to KeyCb
  Result render();                      // re-emit the current window + its highlight
  void   open();                        // no-op on an EMPTY menu: an open menu that
                                        // renders nothing would eat every key
  void   close();                       // clears editing state, then fires CloseCb

 private:
  uint8_t scrollIndicator() const;      // 0x00 / 0x07 / 0x0B / 0x0C — docs/API.md §8.6
  void    rowText(uint8_t index, char* out, size_t outSize) const;
  Result  highlight();                  // the single-frame form
  void    selectNext();
  void    selectPrev();
  void    enterEdit();
  void    nextFieldOrExit();
  void    editField(int32_t delta, bool hold);

  IPanel&     _panel;
  const char* _header;
  MenuItem    _items[AFFA_MENU_MAX_ITEMS];
  uint8_t     _count         = 0;
  uint8_t     _selectedIndex = 0;
  uint8_t     _selectedRow   = 0;
  uint8_t     _editingField  = 0;
  bool        _open          = false;
  bool        _editing       = false;
  CloseCb     _closeCb       = nullptr;
  void*       _closeCtx      = nullptr;
};

}  // namespace affa

#endif  // AFFA_PANEL_CARMINAT && AFFA_ENABLE_MENU
