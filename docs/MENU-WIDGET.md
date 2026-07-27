# MENU-WIDGET.md — the sliding-window menu, with the panel taken out of it

**This widget is OPTIONAL, and it is not the protocol.** What the Carminat panel defines is
exactly two calls, and they are always available whatever this document says:

```cpp
showMenu(header, row0, row1, scrollByte);   // the 96-byte 0x21/0x01 screen
highlightItem(rowTag);                      // 07 29 01 <7E|7F> 80
```

Header, two rows, which one is lit, which arrows — that is the whole wire contract, and it
lives in `CarminatDisplay` unconditionally. Everything *above* it — which items exist, which
one is selected, how a window slides over N of them, what a field is, when Select advances to
the next field and when it exits — is a UI state machine the panel knows nothing about. It is
one opinion about how a menu should behave, and an application with a different remote or a
different idea of editing is expected to write its own against those two primitives. That is
why `AFFA_ENABLE_MENU` defaults to **0** (`src/AffaConfig.h` argues the boundary at length).

`src/widget/` is that opinion, extracted so it can be *reused* rather than *assumed*. The
algorithm is the one that has been on the glass for months — the same window arithmetic, the
same scroll-arrow derivation, the same coarse step, the same clamps, the same
`field 0 → 1 → 2 → out`. The only thing generalised is the **geometry**.

> `src/carminat/Menu/` (`Menu`, `MenuController`, `IPage`) is untouched and still ships.
> The two widgets live side by side; migrating `CarminatDisplay` onto `MenuModel` is a
> separate change and has not been made.

---

## 1. The three files, and the line between them

```
src/widget/MenuGeometry.h    rows, rowChars, wrap — the display's shape, injected
src/widget/MenuModel.{h,cpp} items, fields, selection, window, editing. NO panel.
src/widget/IMenuRenderer.h   the seam an adapter implements
```

| The **model** owns | An **adapter** (renderer) owns |
| --- | --- |
| The item list (`AFFA_MENU_MAX_ITEMS`) and each item's fields (`AFFA_MENU_MAX_FIELDS`) | Nothing about content. It is handed strings. |
| Which item is selected, which **row index** it sits on, which item is on row 0 | Which *physical* row that index is: the Carminat tags `0x7E`/`0x7F`, an OLED's y-offset |
| Edit state: which item, which field, entering and leaving it | Nothing — it sees the result in the row text |
| Row **text**, truncated to `rowChars` and transliterated to 7-bit | The charset of the glass beyond that, fonts, proportional layout |
| The `*Label: <value>` edit markers | Any additional marker the panel affords (a `>` column, an inverse video row) |
| The scroll mask, derived from the window position | What a mask *means* on this display: a pass-through byte, two glyphs, or nothing |
| Clamping, wrapping, step and coarse step | Nothing |
| Firing `onChange` / `onActivate` / `onClose` | Nothing |
| **Never**: frames, ISO-TP, CAN, `Result`, what a redraw costs | **Exactly that**: how to draw, and how expensive it is |

`src/widget/` compiles on the native host with nothing but the C++17 standard library: no
Arduino, no `esp32_can`, no panel header. It allocates nothing after construction, uses no
`std::function`, and every `.cpp` body is gated on `#if AFFA_ENABLE_MENU` like the rest of the
library. Strings (`label`, `unit`, list entries) are **caller-owned and pointed at, never
copied** — string literals and static tables are the intended sources.

---

## 2. `MenuGeometry` — the only thing that was generalised

```cpp
struct MenuGeometry {
  uint8_t rows     = 2;      // visible rows of the sliding window
  uint8_t rowChars = 26;     // usable characters per row, NOT counting the NUL
  bool    wrap     = false;  // does the selection wrap past the ends
};
```

| Field | Meaning | Sanitising (constructor, once) |
| --- | --- | --- |
| `rows` | How many items the window shows at a time. `1` is legal — a one-line display steps item by item. | `0 → 1`. The model divides nothing by it but derives the window from it. |
| `rowChars` | Characters the panel can show on one row, **not** counting the NUL. The model writes at most `rowChars + 1` bytes into the buffer it hands you. | `0 → 1`; anything above `AFFA_MENU_ROW_MAX - 1` (31 by default) is clamped to it, because the row buffer is a fixed array. Raise `AFFA_MENU_ROW_MAX` for a wider panel. |
| `wrap` | `false` stops at both ends and **emits no frame** for the key that moved nothing. | none |

The three shapes this was built to survive:

| Display | `rows` | `rowChars` | `wrap` |
| --- | :---: | :---: | :---: |
| Carminat menu screen (`showMenu`) | 2 | 26 | false — a wheel with detents and no end stop |
| Carminat info-row screen (`showInfoPopup`) | 3 | 8 | false |
| A 4" OLED / character LCD | 6+ | 20+ | your call |

The geometry is **copied and immutable**. To drive the same content onto a different display,
build a second `MenuModel` (or reconstruct one in place) — there is no `setGeometry()`.

---

## 3. `IMenuRenderer` — the contract

```cpp
struct IMenuRenderer {
  virtual ~IMenuRenderer() = default;
  virtual void beginFrame(const char* header, uint8_t scrollMask) = 0;
  virtual void row(uint8_t index, const char* text, bool selected) = 0;
  virtual void endFrame() = 0;
};
```

What the model guarantees, per redraw:

* **One** `beginFrame`, then **exactly `geometry().rows`** calls to `row()` with `index`
  ascending `0 … rows-1`, then **one** `endFrame`. Never nested, never partial, never a
  subset of the window.
* `header` is never null (`""` substitutes for a null header).
* `text` is never null, is NUL-terminated, is at most `rowChars` characters, and is already
  transliterated to 7-bit ASCII — so truncation can never cut a UTF-8 sequence in half.
  Rows past the end of the list arrive as **empty strings**: the bottom row of a one-item
  menu on a two-row panel is a legitimate call, and it blanks the row.
* `text` points at a **borrowed scratch buffer that is reused for the next row**. Copy it.
* `index` is a **row index counted from the top of the window** — not an item index, not a
  panel row tag.
* Exactly **one** row carries `selected == true` while the menu is open.

### `scrollMask`

Derived by the model from the *window position*, not the selection:

| Value | Meaning | Rule |
| --- | --- | --- |
| `0x00` | no arrows | `count <= rows` — the whole list fits |
| `0x0B` | bottom arrow only | `top == 0` |
| `0x07` | top arrow only | `top + rows >= count` |
| `0x0C` | both | otherwise |

These are the Carminat wire values verbatim (`docs/API.md` §8.6), so that panel's adapter is a
pass-through. An adapter with no arrows **ignores the argument**; one with different glyphs
switches on it. The `0x00` case is deliberate and is the bug fix that came with the port: an
arrow pointing at items that are not there is what sent someone hunting for them.

> The mask is a function of the window, so it is **direction-dependent**. Walking down, item 1
> of 3 at `rows = 2` shows `0x0B`; walking back up, the *same item* shows `0x07`, because the
> window is now at the bottom. Any documentation that describes the arrows in terms of "which
> item you are on" is wrong for half the reachable states.

### What the seam deliberately does NOT carry

There is no "only the highlight moved" call. On the Carminat a full redraw is a 96-byte
ISO-TP screen and a highlight change is one small frame — ~14× the bus time — but a three-call
frame protocol cannot express the difference without inventing a second entry point, and the
model does not know what a message costs. **So the model always emits a whole frame, and an
adapter that cares recovers the distinction**: remember the header, rows and mask you last
*sent*; identical text + identical mask + a different `selected` row is precisely the
highlight-only case. `examples/09_menu_widget/CarminatMenuRenderer.h` does exactly this; an
adapter that skips it doubles the bus cost of turning the wheel and nothing will fail.

---

## 4. The model's surface

```cpp
MenuModel menu(renderer, geometry, "SETTINGS");   // geometry copied and sanitised

menu.addItem(item);           // → index, or -1 when full
menu.item(i);                 // MenuItem*, nullptr out of range
menu.setFieldValue(i, f, v);  // from a sensor/web: fires onChange, redraws only if visible
menu.clear();                 // also closes, silently — no onClose

menu.open();                  // refuses an EMPTY menu: it would render nothing and eat keys
menu.close();                 // clears edit state, then fires onClose
menu.render();                // re-emit the whole window
```

Navigation — one method per **intent**, each returning `true` when the model consumed it
(`false` means "the menu is closed, this key is yours", which is what `Menu::handleKey`
returned):

| Method | Not editing | Editing |
| --- | --- | --- |
| `next()` — wheel down, click | selection down | `+1 × step` |
| `prev()` — wheel up, click | selection up | `−1 × step` |
| `increase()` — wheel down, **hold** | selection down | `+1 × step × stepMultiplier` |
| `decrease()` — wheel up, **hold** | selection up | `−1 × step × stepMultiplier` |
| `select()` | `onActivate` if set, else enter edit on field 0 | field `0 → 1 → 2 → out`, no wrap |
| `back()` | close | close (one gesture, one meaning) |

`open()` is deliberately **not** a key: the gesture that opens a menu is policy and belongs to
whatever routes keys.

Field kinds — `integerField(value, min, max, step, stepMultiplier, unit)`,
`listField(values, count, index)`, `readOnlyField(value, unit)`. A read-only field is walked
onto by Select and rendered with the edit markers; all four wheel intents leave it alone.
A list field reaches **both** entry 0 and entry `count-1` in both directions — the bound is
`> count-1`, and getting that wrong is what made the last entry unreachable in the code this
was extracted from.

A change that clamps to a no-op fires **neither** `onChange` nor a redraw. `onChange(item,
fieldIndex, ctx)` fires **after** the value changed and **before** the redraw; it is both the
per-item and the per-field hook. Put your NVS write there — persistence is the application's.

---

## 5. Worked example — a display the library has never seen

A 4-row × 16-character character LCD, wrapping, with the scroll mask translated into two
glyphs it happens to have. This is the whole adapter; nothing else is needed to run the menu
on it.

```cpp
#include <widget/MenuModel.h>
#include <LiquidCrystal_I2C.h>

class LcdMenuRenderer final : public affa::widget::IMenuRenderer {
 public:
  static constexpr uint8_t kRows = 4, kChars = 16;
  static constexpr affa::widget::MenuGeometry geometry() { return {kRows, kChars, true}; }

  explicit LcdMenuRenderer(LiquidCrystal_I2C& lcd) : _lcd(lcd) {}

  void beginFrame(const char* header, uint8_t scrollMask) override {
    _lcd.clear();
    _lcd.setCursor(0, 0);
    _lcd.print(header);                                   // row 0 is the header on this glass
    _lcd.setCursor(kChars - 2, 0);
    _lcd.write((scrollMask == 0x07 || scrollMask == 0x0C) ? '^' : ' ');
    _lcd.write((scrollMask == 0x0B || scrollMask == 0x0C) ? 'v' : ' ');
  }

  void row(uint8_t index, const char* text, bool selected) override {
    if (index + 1 >= kRows) return;                       // header ate one line
    _lcd.setCursor(0, index + 1);
    _lcd.write(selected ? '>' : ' ');                     // this panel's highlight
    _lcd.print(text);                                     // already ≤ kChars and 7-bit
  }

  void endFrame() override {}                             // nothing buffered

 private:
  LiquidCrystal_I2C& _lcd;
};
```

Driving it:

```cpp
LcdMenuRenderer renderer(lcd);
affa::widget::MenuModel menu(renderer, LcdMenuRenderer::geometry(), "SETTINGS");
// ... addItem() ..., then map your buttons onto next()/prev()/select()/back().
```

Declare `kRows = 3` if you want three menu rows plus the header; the geometry you hand the
model is the number of rows **it** may draw, and stealing one for a header is your business.
`examples/09_menu_widget/` runs one identical menu through three adapters — the Carminat menu
screen (2 × 26, mask passed through), the Carminat info-row screen (3 × 8, mask and highlight
ignored because that screen has neither), and a 6 × 20 serial stand-in for an OLED (mask
translated). Between them they cover every legal thing an adapter can do with the mask.

---

## 6. Where it is tested

`test/test_menu_widget/` — 38 cases. Twelve behavioural cases written once and instantiated at
**`rows` = 2, 3 and 6** over the same three-item model (so the identical list *fits* the taller
windows and must lose its arrows), plus two non-parameterised cases. A recorder polices every
redraw: begin/rows-in-order/end never nested, exactly `rows` rows per frame, no row longer than
`rowChars`, exactly one selected row. Expectations are hand-written per geometry, not computed
with the model's own expression.

The suite includes **only** `<unity.h>`, the C headers and `widget/MenuModel.h`. That include
list is itself the claim that the model is display-agnostic, and it stops compiling the day
that stops being true.
