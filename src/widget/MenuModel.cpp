#include "MenuModel.h"

// The ENTIRE body is inside the gate, so a build without the menu compiles this
// translation unit to an empty object file. See AffaConfig.h.
#if AFFA_ENABLE_MENU

#include "../util/AffaLog.h"
#include "../util/AffaText.h"
#include <cstdio>

namespace affa {
namespace widget {
namespace {
constexpr const char* kTag = "MENUW";

// An item index that no menu can hold (AFFA_MENU_MAX_ITEMS is a uint8_t capacity well
// below this), used to mean "this row is past the end of the list".
constexpr uint8_t kNoItem = 0xFF;
}  // namespace

// ---------------------------------------------------------------------------
// Field builders — identical to the deleted Carminat widget's, which is why an item table
// written against that one still compiles: carminat/CarminatDisplay.h re-exports these three
// and MenuItem/Field/FieldType into namespace affa under their old names.
// ---------------------------------------------------------------------------

Field integerField(int32_t value, int32_t min, int32_t max, int32_t step,
                   int32_t stepMultiplier, const char* unit) {
  Field f;
  f.type           = FieldType::Integer;
  f.value          = value;
  f.minValue       = min;
  f.maxValue       = max;
  f.step           = step;
  f.stepMultiplier = stepMultiplier;
  f.unit           = unit;
  f.readOnly       = false;
  return f;
}

Field readOnlyField(int32_t value, const char* unit) {
  Field f;
  f.type     = FieldType::Integer;
  f.value    = value;
  f.minValue = value;
  f.maxValue = value;
  f.step     = 0;
  f.stepMultiplier = 0;
  f.unit     = unit;
  f.readOnly = true;
  return f;
}

Field listField(const char* const* values, uint8_t count, uint8_t index) {
  Field f;
  f.type      = FieldType::List;
  f.list      = values;
  f.listCount = count;
  f.value     = (count == 0) ? 0 : (index < count ? index : static_cast<int32_t>(count - 1));
  f.minValue  = 0;
  f.maxValue  = (count == 0) ? 0 : static_cast<int32_t>(count - 1);
  f.step      = 1;
  f.stepMultiplier = 1;
  return f;
}

// ---------------------------------------------------------------------------
// Construction and content
// ---------------------------------------------------------------------------

// The geometry is sanitised HERE and nowhere else, so every later use can trust it: rows is
// a loop bound and a divisor of the window arithmetic, and rowChars sizes a write into a
// fixed array. A caller that asks for more than the library can hold gets the library's
// limit and a warning, not a buffer overrun.
MenuModel::MenuModel(IMenuRenderer& renderer, const MenuGeometry& geom, const char* header)
    : _renderer(renderer), _geom(geom), _header(header) {
  if (_geom.rows == 0) {
    AFFA_LOGW(kTag, "geometry.rows = 0, using 1");
    _geom.rows = 1;
  }
  constexpr uint8_t kMaxChars = static_cast<uint8_t>(AFFA_MENU_ROW_MAX - 1);
  if (_geom.rowChars == 0) {
    AFFA_LOGW(kTag, "geometry.rowChars = 0, using 1");
    _geom.rowChars = 1;
  } else if (_geom.rowChars > kMaxChars) {
    AFFA_LOGW(kTag, "geometry.rowChars %u > AFFA_MENU_ROW_MAX-1 (%u), clamped",
              static_cast<unsigned>(_geom.rowChars), static_cast<unsigned>(kMaxChars));
    _geom.rowChars = kMaxChars;
  }
}

void MenuModel::setHeader(const char* h) { _header = h; }

int MenuModel::addItem(const MenuItem& it) {
  if (_count >= AFFA_MENU_MAX_ITEMS) {
    AFFA_LOGW(kTag, "menu full at %d items, '%s' dropped", AFFA_MENU_MAX_ITEMS,
              it.label ? it.label : "?");
    return -1;
  }
  MenuItem& dst = _items[_count];
  dst = it;
  if (dst.fieldCount > AFFA_MENU_MAX_FIELDS) {
    AFFA_LOGW(kTag, "'%s': %u fields, %d kept", dst.label ? dst.label : "?",
              static_cast<unsigned>(dst.fieldCount), AFFA_MENU_MAX_FIELDS);
    dst.fieldCount = AFFA_MENU_MAX_FIELDS;
  }
  return static_cast<int>(_count++);
}

MenuItem* MenuModel::item(uint8_t index) { return (index < _count) ? &_items[index] : nullptr; }

void MenuModel::clear() {
  _count         = 0;
  _selectedIndex = 0;
  _selectedRow   = 0;
  _editingField  = 0;
  _editing       = false;
  _open          = false;      // silently: CloseCb means "the user left", not "the
                               // application replaced the content"
}

void MenuModel::onClose(CloseCb cb, void* ctx) { _closeCb = cb; _closeCtx = ctx; }

// ---------------------------------------------------------------------------
// Window and scroll arrows
// ---------------------------------------------------------------------------

// The window is derived, not stored: the item on row 0 is the selection minus the row it
// sits on. Two pieces of state (index + row) instead of three, which is why no navigation
// path can leave the window and the selection disagreeing.
uint8_t MenuModel::topIndex() const {
  if (_selectedRow > _selectedIndex) return 0;    // cannot happen; cheaper than an assert
  return static_cast<uint8_t>(_selectedIndex - _selectedRow);
}

// The Carminat rule (docs/API.md §8.6) with the hard-coded 2 replaced by geometry.rows, and
// the selection/row test rewritten as the window test it always was:
//
//     count <= 2                                        -> 0x00     count <= rows
//     sel == 0 || (sel == 1 && row == 1)                -> 0x0B     top == 0
//     sel == count-1 || (sel == count-2 && row == 0)    -> 0x07     top + rows >= count
//     otherwise                                         -> 0x0C
//
// Both forms agree for every reachable state at rows = 2 — `top` IS `sel - row`, so
// "sel 0, or sel 1 on the bottom row" is exactly "the window starts at item 0".
//
// The `count <= rows` case is the one the extracted code did not have: it indexed
// items[top+1] with no bounds check and read past the end of a one-item menu. Showing no
// arrows when the whole list fits is not a cosmetic choice — an arrow that points at
// nothing is what sent someone hunting for items that were not there.
uint8_t MenuModel::scrollMask() const {
  if (_count <= _geom.rows) return 0x00;
  const unsigned top = topIndex();
  if (top == 0) return 0x0B;
  if (top + _geom.rows >= _count) return 0x07;
  return 0x0C;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

// `Label` with no fields; `*Label:<sep>v1<sep>v2` with them; the item being edited carries
// the `*`, and the field under edit is wrapped in `<>`. Reproduced from the Carminat
// widget's rowText() exactly — this is what the glass has been showing for months.
//
// TRUNCATION AND UTF-8. Every caller-owned string goes through toAscii() on its own before
// it reaches `out`, so the bytes that get truncated at the row width are always 7-bit and
// a multi-byte sequence can never be cut in half — the panel-side guarantee, moved here
// because a generic renderer has no frame builder to do it at. (A label longer than
// AFFA_MENU_ROW_MAX is clipped by the scratch buffer first, which is harmless: the row is
// narrower than the scratch by construction.)
void MenuModel::rowText(uint8_t index, char* out, size_t outSize) const {
  if (!out || outSize == 0) return;
  out[0] = '\0';
  if (index >= _count) return;

  const MenuItem& it = _items[index];
  size_t n = 0;
  char   scratch[AFFA_MENU_ROW_MAX];

  const auto putc_  = [&](char c) { if (n + 1 < outSize) out[n++] = c; };
  const auto putRaw = [&](const char* s) { while (s && *s && n + 1 < outSize) out[n++] = *s++; };
  const auto put    = [&](const char* s) {
    if (!s) return;
    toAscii(s, scratch, sizeof(scratch));
    putRaw(scratch);
  };

  const bool onThisItem = (_editing && index == _selectedIndex);
  if (onThisItem) putc_('*');
  put(it.label);
  if (it.fieldCount > 0) putc_(':');

  for (uint8_t i = 0; i < it.fieldCount; ++i) {
    const Field& f    = it.fields[i];
    const bool   edit = onThisItem && _editingField == i;
    putc_(it.separator);
    if (edit) putc_('<');
    if (f.type == FieldType::Integer) {
      char num[16];
      std::snprintf(num, sizeof(num), "%ld", static_cast<long>(f.value));
      putRaw(num);                                  // already ASCII
      put(f.unit);
    } else if (f.list && f.value >= 0 && f.value < static_cast<int32_t>(f.listCount)) {
      put(f.list[f.value]);
    }
    if (edit) putc_('>');
  }
  out[n] = '\0';
}

// One redraw is one whole window: begin, every row top to bottom, end. The cheap "only the
// highlight moved" case is NOT this function — it is renderSelectionMove(), below.
void MenuModel::render() {
  if (_count == 0) {
    AFFA_LOGW(kTag, "render on an empty menu");
    return;
  }
  const unsigned top      = topIndex();
  const size_t   rowBytes = static_cast<size_t>(_geom.rowChars) + 1u;
  char           buf[AFFA_MENU_ROW_MAX];

  _renderer.beginFrame(_header ? _header : "", scrollMask());
  for (uint8_t r = 0; r < _geom.rows; ++r) {
    const unsigned idx = top + r;
    // Rows past the end of the list are still emitted, empty: the window has a fixed height
    // and the panel expects that many rows whatever the list does.
    rowText(idx < _count ? static_cast<uint8_t>(idx) : kNoItem, buf, rowBytes);
    _renderer.row(r, buf, r == _selectedRow);
  }
  _renderer.endFrame();
}

// THE CONDITION, stated once so the two callers do not have to restate it: `top` is
// _selectedIndex - _selectedRow, so a move that changes BOTH by one leaves the window exactly
// where it was. Same window + same items + not editing (selectNext/selectPrev are unreachable
// while editing, and the '*' / '<>' markers are the only way the selection touches the text)
// means every row's text is what the last frame put there, and scrollMask() — a function of
// `top` and _count alone — is unchanged too. Nothing but the lit row differs, which is
// precisely what highlightOnly() is allowed to assume.
//
// A renderer that cannot do it returns false (the default) and gets the frame it would have
// got before this call existed. On the Carminat the true branch is one 07 29 01 frame against
// a 96-byte ISO-TP screen — the whole reason the old Menu::selectNext had this branch.
void MenuModel::renderSelectionMove() {
  if (_renderer.highlightOnly(_selectedRow)) return;
  render();
}

// ---------------------------------------------------------------------------
// Open / close
// ---------------------------------------------------------------------------

void MenuModel::open() {
  if (_open) return;
  if (_count == 0) {
    // Refusing is the whole point: an open menu with nothing in it renders nothing and then
    // swallows the wheel and the select key, which reads as a dead panel.
    AFFA_LOGW(kTag, "open() on an empty menu — refused");
    return;
  }
  _open = true;
  AFFA_LOGI(kTag, "menu opened at item %u", static_cast<unsigned>(_selectedIndex));
  render();
}

// Clearing `editing` here is a DELIBERATE behaviour change, carried over from the Carminat
// widget. The code it was extracted from closed on hold-Load without it, so reopening
// resumed in edit mode on whatever field was live when you left — with no visual difference
// from a fresh open.
void MenuModel::close() {
  if (!_open) return;
  _open         = false;
  _editing      = false;
  _editingField = 0;
  AFFA_LOGI(kTag, "menu closed");
  if (_closeCb) _closeCb(_closeCtx);
}

// ---------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------

void MenuModel::selectNext() {
  if (static_cast<uint8_t>(_selectedIndex + 1) >= _count) {
    if (!_geom.wrap) return;                          // the Carminat behaviour: no wrap
    _selectedIndex = 0;
    _selectedRow   = 0;
    // A wrap is a whole frame, not a highlight: it takes the window from one end of the list
    // to the other. (On a geometry where the entire list fits the window it lands back on the
    // same items — but that is a property of the content, not of the move, and the model does
    // not claim the cheap path on an accident.)
    render();
    return;
  }
  ++_selectedIndex;
  if (_selectedRow + 1 < _geom.rows) {
    // The selection moved INSIDE the visible window: the text is unchanged and only the lit
    // row differs, so the renderer gets offered the one-frame path.
    ++_selectedRow;
    renderSelectionMove();
    return;
  }
  // The selection was already on the last row, so the window scrolls under it and
  // _selectedRow stays put: every row's text changes and it is a whole frame.
  render();
}

void MenuModel::selectPrev() {
  if (_selectedIndex == 0) {
    if (!_geom.wrap) return;                          // no wrap
    _selectedIndex = static_cast<uint8_t>(_count - 1);
    // Land on the LAST row of the window when the list is longer than the window, so the
    // window shows the tail of the list rather than one item and a blank.
    _selectedRow   = (_count < _geom.rows) ? static_cast<uint8_t>(_count - 1)
                                           : static_cast<uint8_t>(_geom.rows - 1);
    render();
    return;
  }
  --_selectedIndex;
  if (_selectedRow > 0) {                             // inside the window
    --_selectedRow;
    renderSelectionMove();
    return;
  }
  // The window scrolls up and _selectedRow stays 0.
  render();
}

void MenuModel::enterEdit() {
  if (_selectedIndex >= _count) return;
  if (!_items[_selectedIndex].editable) return;
  if (_items[_selectedIndex].fieldCount == 0) return;
  _editingField = 0;
  _editing      = true;
  render();
}

// field 0 -> 1 -> 2 -> leave. The last field exits edit mode; it does not wrap.
void MenuModel::nextFieldOrExit() {
  const MenuItem& it = _items[_selectedIndex];
  if (static_cast<uint8_t>(_editingField + 1) < it.fieldCount) {
    ++_editingField;
    render();
    return;
  }
  _editing      = false;
  _editingField = 0;
  render();
}

void MenuModel::editField(int32_t delta, bool hold) {
  if (_selectedIndex >= _count) return;
  MenuItem& it = _items[_selectedIndex];
  if (_editingField >= it.fieldCount) return;
  Field& f = it.fields[_editingField];
  if (f.readOnly) return;

  // The multiplier applies to the DELTA, before the step — legacy order, kept so a
  // stepMultiplier of 2 on a step of 5 still moves 10.
  if (hold && f.stepMultiplier > 0) delta *= f.stepMultiplier;

  int32_t nv;
  if (f.type == FieldType::Integer) {
    nv = f.value + delta * f.step;
    if (nv < f.minValue) nv = f.minValue;
    if (nv > f.maxValue) nv = f.maxValue;
  } else {
    // THE CLAMP FIX. The extracted code wrote `if (newIndex >= size-1) newIndex = size-1`,
    // which snaps index size-2 straight to size-1 going up and makes the LAST entry
    // unreachable coming back down. The bound is `> count-1`, not `>= count-1`.
    if (f.listCount == 0) return;
    nv = f.value + delta;
    if (nv < 0) nv = 0;
    if (nv > static_cast<int32_t>(f.listCount) - 1) nv = static_cast<int32_t>(f.listCount) - 1;
  }

  if (nv == f.value) return;    // no change, no frames
  f.value = nv;

  if (it.onChange) it.onChange(it, _editingField, it.ctx);
  render();
}

bool MenuModel::setFieldValue(uint8_t itemIndex, uint8_t fieldIndex, int32_t value) {
  if (itemIndex >= _count) return false;
  MenuItem& it = _items[itemIndex];
  if (fieldIndex >= it.fieldCount) return false;

  it.fields[fieldIndex].value = value;
  if (it.onChange) it.onChange(it, fieldIndex, it.ctx);

  if (_open) {
    const unsigned top = topIndex();
    if (itemIndex >= top && itemIndex < top + _geom.rows) render();
  }
  return true;
}

// ---------------------------------------------------------------------------
// The navigation surface
// ---------------------------------------------------------------------------
//
// OPENING is not handled here: the gesture that opens a menu is policy and belongs to
// whatever routes keys. A CLOSED MODEL CONSUMES NOTHING — that false return is what lets an
// application implement its own open gesture and keep every other key.
//
// The click/hold split is the Carminat widget's, unchanged: while editing, hold is the
// coarse step; while not editing, hold moves the selection exactly like a click, because a
// held wheel on a list is still just a wheel.

bool MenuModel::next() {
  if (!_open) return false;
  if (_editing) editField(+1, false); else selectNext();
  return true;
}

bool MenuModel::prev() {
  if (!_open) return false;
  if (_editing) editField(-1, false); else selectPrev();
  return true;
}

bool MenuModel::increase() {
  if (!_open) return false;
  if (_editing) editField(+1, true); else selectNext();
  return true;
}

bool MenuModel::decrease() {
  if (!_open) return false;
  if (_editing) editField(-1, true); else selectPrev();
  return true;
}

bool MenuModel::select() {
  if (!_open) return false;
  if (_editing) { nextFieldOrExit(); return true; }
  if (_selectedIndex < _count) {
    MenuItem& it = _items[_selectedIndex];
    if (it.onActivate)    it.onActivate(it.ctx);
    else if (it.editable) enterEdit();
  }
  return true;
}

// Back closes the menu even from inside edit mode — one gesture, one meaning, which is what
// the Carminat widget does on hold-Load (it tests the Hold edge before it tests _editing).
// NavCommand::Back is documented as "leave edit / close the menu"; the behaviour that
// shipped is "close", and the port keeps the behaviour rather than the doc line.
bool MenuModel::back() {
  if (!_open) return false;
  close();
  return true;
}

}  // namespace widget
}  // namespace affa

#endif  // AFFA_ENABLE_MENU
