#include "Menu.h"

// The ENTIRE body is inside the gate, so a build without the menu compiles this
// translation unit to an empty object file. See AffaConfig.h.
#if AFFA_PANEL_CARMINAT && AFFA_ENABLE_MENU

#include "../../util/AffaLog.h"
#include <cstdio>

namespace affa {
namespace {
constexpr const char* kTag = "MENU";
}

// ---------------------------------------------------------------------------
// Field builders
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

Menu::Menu(IPanel& panel, const char* header) : _panel(panel), _header(header) {}

void Menu::setHeader(const char* h) { _header = h; }

int Menu::addItem(const MenuItem& it) {
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

MenuItem* Menu::item(uint8_t index) { return (index < _count) ? &_items[index] : nullptr; }

uint8_t Menu::count() const { return _count; }

void Menu::clear() {
  _count         = 0;
  _selectedIndex = 0;
  _selectedRow   = 0;
  _editingField  = 0;
  _editing       = false;
  _open          = false;      // silently: CloseCb means "the user left", not "the
                               // application replaced the content"
}

void Menu::onClose(CloseCb cb, void* ctx) { _closeCb = cb; _closeCtx = ctx; }

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

// count <= 2 -> no arrows. THIS CASE IS NEW: the extracted code indexed items[top+1]
// with no bounds check and read past the end of a one-item menu.
uint8_t Menu::scrollIndicator() const {
  if (_count <= 2) return 0x00;
  if (_selectedIndex == 0 || (_selectedIndex == 1 && _selectedRow == 1)) return 0x0B;
  if (_selectedIndex + 1 == _count ||
      (_selectedIndex + 2 == _count && _selectedRow == 0)) return 0x07;
  return 0x0C;
}

// `Label` with no fields; `*Label:<sep>v1<sep>v2` with them; the item being edited carries
// the `*`, and the field under edit is wrapped in `<>`. Reproduced from the extracted
// getItemString() exactly — this is what the glass has been showing for months.
//
// An out-of-range index yields an EMPTY row rather than reading past the array: the bottom
// row of a one-item menu is a legitimate call.
void Menu::rowText(uint8_t index, char* out, size_t outSize) const {
  if (!out || outSize == 0) return;
  out[0] = '\0';
  if (index >= _count) return;

  const MenuItem& it = _items[index];
  size_t n = 0;
  const auto putc_ = [&](char c) { if (n + 1 < outSize) out[n++] = c; };
  const auto put   = [&](const char* s) { while (s && *s && n + 1 < outSize) out[n++] = *s++; };

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
      put(num);
      put(f.unit);
    } else if (f.list && f.value >= 0 && f.value < static_cast<int32_t>(f.listCount)) {
      put(f.list[f.value]);
    }
    if (edit) putc_('>');
  }
  out[n] = '\0';
}

Result Menu::highlight() { return _panel.highlightItem(_selectedRow); }

// A full redraw is one 96-byte showMenu payload followed by one highlight frame — ~14x
// the bus time of a highlight alone, which is why they occupy different RenderSlots and
// why the menu, not the application, decides between them.
Result Menu::render() {
  if (_count == 0) {
    AFFA_LOGW(kTag, "render on an empty menu");
    return Result::BadArgument;
  }
  const uint8_t top = (_selectedRow == 0) ? _selectedIndex
                                          : static_cast<uint8_t>(_selectedIndex - 1);

  char row0[AFFA_MENU_ROW_MAX];
  char row1[AFFA_MENU_ROW_MAX];
  rowText(top, row0, sizeof(row0));
  rowText(static_cast<uint8_t>(top + 1), row1, sizeof(row1));

  const Result r = _panel.showMenu(_header ? _header : "", row0, row1, scrollIndicator());
  const Result h = highlight();
  return (r != Result::Ok) ? r : h;
}

// ---------------------------------------------------------------------------
// Open / close
// ---------------------------------------------------------------------------

void Menu::open() {
  if (_open) return;
  if (_count == 0) {
    // Refusing is the whole point: an open menu with nothing in it renders nothing and
    // then swallows the wheel and Load, which reads as a dead panel.
    AFFA_LOGW(kTag, "open() on an empty menu — refused");
    return;
  }
  _open = true;
  AFFA_LOGI(kTag, "menu opened at item %u", static_cast<unsigned>(_selectedIndex));
  render();
}

// Clearing `editing` here is a DELIBERATE behaviour change. The extracted code closed on
// hold-Load without it, so reopening resumed in edit mode on whatever field was live when
// you left — with no visual difference from a fresh open.
void Menu::close() {
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

void Menu::selectNext() {
  if (static_cast<uint8_t>(_selectedIndex + 1) >= _count) return;   // no wrap
  ++_selectedIndex;
  if (_selectedRow == 0) {
    // The selection moved INSIDE the visible window: one frame, not a screen.
    _selectedRow = 1;
    highlight();
  } else {
    render();                                                       // the window scrolls
  }
}

void Menu::selectPrev() {
  if (_selectedIndex == 0) return;                                  // no wrap
  --_selectedIndex;
  if (_selectedRow == 1) {
    _selectedRow = 0;
    highlight();
  } else {
    render();
  }
}

void Menu::enterEdit() {
  if (_selectedIndex >= _count) return;
  if (!_items[_selectedIndex].editable) return;
  if (_items[_selectedIndex].fieldCount == 0) return;
  _editingField = 0;
  _editing      = true;
  render();
}

// field 0 -> 1 -> 2 -> leave. The last field exits edit mode; it does not wrap.
void Menu::nextFieldOrExit() {
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

void Menu::editField(int32_t delta, bool hold) {
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

bool Menu::setFieldValue(uint8_t itemIndex, uint8_t fieldIndex, int32_t value) {
  if (itemIndex >= _count) return false;
  MenuItem& it = _items[itemIndex];
  if (fieldIndex >= it.fieldCount) return false;

  it.fields[fieldIndex].value = value;
  if (it.onChange) it.onChange(it, fieldIndex, it.ctx);

  if (_open) {
    const uint8_t top = (_selectedRow == 0) ? _selectedIndex
                                            : static_cast<uint8_t>(_selectedIndex - 1);
    if (itemIndex == top || itemIndex == static_cast<uint8_t>(top + 1)) render();
  }
  return true;
}

// ---------------------------------------------------------------------------
// Key routing
// ---------------------------------------------------------------------------
//
// OPENING is not handled here: the hotkey is policy and lives in AffaDisplayBase
// (setMenuHotkey / clearMenuHotkey). A CLOSED MENU CONSUMES NOTHING, which is what lets
// an application implement its own open gesture in its KeyCb.
bool Menu::handleKey(Key k, KeyEdge e) {
  if (!_open) return false;

  switch (k) {
    case Key::RollUp:
      if (_editing) editField(-1, e == KeyEdge::Hold); else selectPrev();
      return true;

    case Key::RollDown:
      if (_editing) editField(+1, e == KeyEdge::Hold); else selectNext();
      return true;

    case Key::Load:
      if (e == KeyEdge::Hold) { close(); return true; }
      if (_editing) { nextFieldOrExit(); return true; }
      if (_selectedIndex < _count) {
        MenuItem& it = _items[_selectedIndex];
        if (it.onActivate)      it.onActivate(it.ctx);
        else if (it.editable)   enterEdit();
      }
      return true;

    default:
      // SrcNext, SrcPrev, VolUp, VolDown, Pause: the menu has no opinion, so they reach
      // the application even while it is open.
      return false;
  }
}

}  // namespace affa

#endif  // AFFA_PANEL_CARMINAT && AFFA_ENABLE_MENU
