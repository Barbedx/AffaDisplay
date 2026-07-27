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

> **There is exactly one menu implementation.** `src/carminat/Menu/` — the two-row widget
> with the panel welded into it — has been **deleted**, and `CarminatDisplay` now holds a
> `widget::MenuModel` plus an `affa::CarminatMenuRenderer`. Shipping the extraction *beside*
> the original was a safety measure for one step, and keeping it would have been a second
> copy of a state machine on purpose — the exact failure this project already paid for once,
> when the sync FSM was duplicated in `CarminatDisplay::tick()` and `UpdateListBase::tick()`
> and both copies carried the same two defects verbatim.
>
> The application-facing names survive as **aliases** in `carminat/CarminatDisplay.h`:
> `affa::Menu` is `affa::widget::MenuModel`, and `affa::MenuItem`, `affa::Field`,
> `affa::FieldType`, `integerField`, `readOnlyField`, `listField` name the `widget::` ones.
> `getMenu()` keeps its name and returns `widget::MenuModel&`. Two observable differences:
> `render()` returns **void** (the verdict is `menuRenderer().lastResult()` — only the layer
> holding the `IPanel` can answer it), and rows truncate at the injected `rowChars` (26)
> rather than at `AFFA_MENU_ROW_MAX - 1`.

---

## 1. The files, and the line between them

```
src/widget/MenuGeometry.h    rows, rowChars, wrap — the display's shape, injected
src/widget/MenuModel.{h,cpp} items, fields, selection, window, editing. NO panel.
src/widget/IMenuRenderer.h   the seam an adapter implements

src/carminat/CarminatMenuRenderer.{h,cpp}   the adapter for THIS panel: 2 x 26, showMenu
                                            + highlightItem, the highlight-only fast path
src/carminat/MenuController.{h,cpp}         navigation policy: the page stack, and the
                                            (Key, KeyEdge) -> intent map
src/carminat/IPage.h                        a full-screen page pushed in front of the menu
```

The controller and `IPage` did **not** fold into the model, and that is deliberate: which
gesture opens a menu, who gets a key first, and what happens to the glass when a page is
popped are navigation *policy*. The model has no `Key` vocabulary at all — six intents, no
enum — precisely so that layer can be replaced without touching it.

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
| Which redraws are *whole* and which are only the lit row moving (`highlightOnly`) — a fact about the state machine, not about the glass | What that is worth: one frame, one cell, or nothing it can do |
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

  // Only the lit row changed. Override to move the highlight without redrawing; the
  // default declines and the model emits a full frame instead.
  virtual bool highlightOnly(uint8_t index) { (void)index; return false; }
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

### `highlightOnly` — the one call that is not a whole frame

On the Carminat a full redraw is a 96-byte ISO-TP screen and a highlight change is one
`07 29 01` frame — **~14× less bus time**, which is why `RenderSlot::Menu` and
`RenderSlot::Highlight` are different slots. Moving the selection *inside* the visible window
changes nothing but which row is lit, and `beginFrame`/`row`/`endFrame` cannot say that. So
there is a fourth call that can.

```cpp
bool highlightOnly(uint8_t index);   // index: the ROW index, 0..rows-1, that is now lit
```

The model raises it **exactly when the window did not move and only the selected row
changed** — `top` is `selectedIndex - selectedRow`, so a move that changes both by one leaves
the window, the text and the mask untouched. This is the same condition the pre-extraction
`Menu::selectNext` used to call `highlightItem()` alone.

Everything else is a whole frame: the window scrolling, entering or leaving edit, a value
changing, `open()`, `render()`, `setFieldValue()`, and a wrap from one end of the list to the
other.

**The default is correct, not fast**, and that is why the method is not pure virtual. A
renderer that cannot move a highlight — anything that redraws the whole glass, a screen with
no selection tag at all — overrides nothing, returns `false` by inheritance, and gets the
frame it would have got anyway. Returning `true` is a promise that the highlight *did* move.

> This deliberately does **not** live in the adapters. The earlier design had every adapter
> cache its last frame and infer the case by comparing text + mask + `selected`; that is an
> obligation on every adapter, silently paid in bus time by the ones that skip it — nothing
> fails, the wheel just costs twice what it should. A cost only the seam can make free for
> everybody belongs in the seam.

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
(`false` means "the menu is closed, this key is yours", which is what the deleted
`Menu::handleKey` returned; `MenuController::routeKey` is where that map now lives on this
panel, and its `default:` is what keeps the transport keys reaching the application):

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

`highlightOnly()` is not overridden here, so this adapter redraws on every wheel detent — the
correct, boring answer. A panel that can address one cell cheaply overrides it: blank the `>`
at the row it last lit, write it at `index`, `return true`. Note that only the *new* row is
passed — an adapter that has to erase the old marker keeps it from the `selected` flags of the
last frame, which it already saw.

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

`test/test_menu_widget/` — 51 cases. Sixteen behavioural cases written once and instantiated at
**`rows` = 2, 3 and 6** over the same three-item model (so the identical list *fits* the taller
windows and must lose its arrows), plus three non-parameterised cases. A recorder polices every
redraw: begin/rows-in-order/end never nested, exactly `rows` rows per frame, no row longer than
`rowChars`, exactly one selected row. Expectations are hand-written per geometry, not computed
with the model's own expression.

Section 10 of the suite pins `highlightOnly()` at every geometry: a move inside the window
calls it and emits **no** frame; a move across the window boundary emits a frame and does
**not** call it; a renderer that answers `false` gets a full frame every time; nothing that
changes row *text* (entering edit, a value moving, leaving edit) ever takes the cheap path.
The recorder used by the other thirty-eight cases does **not** override `highlightOnly()`, so
those cases are simultaneously the assertion that the default still emits every frame it did
before the call existed.

The suite includes **only** `<unity.h>`, the C headers and `widget/MenuModel.h`. That include
list is itself the claim that the model is display-agnostic, and it stops compiling the day
that stops being true.

### 6.1 The evidence that the migration did not change behaviour

`test_menu_widget` proves the model is *correct*. It cannot prove that `CarminatDisplay` still
behaves the way it did, because it never mentions `CarminatDisplay`. That job belongs to the
suites that were written against the **deleted** widget, through the display's public API,
before `MenuModel` existed:

| Suite | What it holds the migration to |
| --- | --- |
| `test_nav` | key routing, the page stack, and **frame counts on the wire** for wheel moves, edit entry and exit |
| `test_bench_surface` | the web console's acceptance list, driven through the same public API |
| `test_twin` | the virtual panel decoding real menu screens off the bus — **suite deleted since**, with `src/vpanel/`; `test_bench_surface` now carries a local decoder over the transmitted frames instead |
| `test_seam` | that hold-`Load` is the OEM open gesture and `getMenu()` is reachable through the seam |
| `test_core`, `test_updatelist_wire` | that a panel with no menu still answers `supports(Feature::Menu)` with false |

**These suites are the acceptance criterion, and they pass unchanged.** No assertion, rig,
recorder or `RUN_TEST` line was weakened, and the frame-count assertions in particular are
what confirm the bus cost did not move. Two mechanical edits were unavoidable and neither
touches an expectation: `test_nav` and `test_twin` both lost a now-dangling
`#include "carminat/Menu/Menu.h"` (the names come from the alias block in
`CarminatDisplay.h`), and one `ASSERT_RESULT(Ok, …getMenu().render())` became
`…getMenu().render(); ASSERT_RESULT(Ok, …menuRenderer().lastResult())`, because `render()`
now returns `void` and the verdict moved to the adapter. `test_nav` also gained one *new*
case — the transport keys (`SrcNext`, `SrcPrev`, `VolUp`, `VolDown`, `Pause`) must still
reach the application while the menu is open — because the `(Key, KeyEdge)` map was
hand-rewritten into `MenuController` and that was the most plausible way to break it.

## 7. What collapsing the two implementations cost

Measured, not asserted: a clean `pio run` of every environment on the tree that still had
`src/carminat/Menu/`, and another on this one, same day and same toolchain.

| Env | Flash before | Flash after | Δ | RAM before | RAM after | Δ |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `ex03_carminat_menu` | 274 672 B | 275 700 B | **+1 028 B** | 17 900 B | 18 028 B | **+128 B** |
| `ex08_radio_mitm` | 274 700 B | 275 740 B | +1 040 B | 17 756 B | 17 884 B | +128 B |
| `ex90_bench_ota` | 899 032 B | 900 040 B | **+1 008 B** | 71 124 B | 71 236 B | **+112 B** |
| `ex09_menu_widget` | 278 294 B | 276 904 B | **−1 390 B** | 19 876 B | 19 908 B | +32 B |
| every build without a menu | — | — | **0** | — | — | **0** |

Three things to read off it.

**It cost flash; it did not save flash.** A Carminat build that uses a menu grew by about a
kilobyte. That is the price of generality: the deleted `Menu` had `rows = 2` and
`rowChars = 26` as compile-time constants and called `IPanel` directly, while `MenuModel`
multiplies by a `rowChars` it is handed and reaches the panel through a virtual
`IMenuRenderer`, and `CarminatDisplay` now holds an adapter object next to the model. Anyone
who expected deleting a file to shrink the binary should look at this row and adjust.

**Every build that does not use a menu is byte-identical.** `size_min`, `size_all`,
`ex02_carminat_text`, both UpdateList examples — all zero. The whole-body `#if`
discipline held through the migration; you do not pay for the widget unless you name it.

**The one row that shrank is the argument in miniature.** `ex09_menu_widget` lost 1 390 B
because it stopped carrying its own copy of the Carminat adapter and now uses the library's.
One copy is smaller than two, at every scale — the example just happened to be small enough
for the effect to outrun the cost of the abstraction. That is the trade in a sentence: a
kilobyte, in exchange for a fix landing once. The project has already paid the other side of
this bill, when the sync state machine was duplicated across `CarminatDisplay::tick()` and
`UpdateListBase::tick()` and **both copies carried the same two defects verbatim**.
