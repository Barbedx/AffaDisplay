# 09_menu_widget — one menu, three displays

**The menu algorithm is display-agnostic: the sliding window, the in-place editing, the
field kinds and the scroll-arrow derivation in `src/widget/` do not change by one line
between a 2×26 panel window, a 3×8 info row and a 6×20 OLED.** All that changes is a
`MenuGeometry` and a renderer whose panel-specific part is under thirty lines of code —
counting the geometry and the three `IMenuRenderer` methods, and not the comments: 16 lines
for the info-row screen, 27 for the OLED, and 24 for the Carminat menu screen plus 9 more
for the highlight-only optimisation the seam cannot express (33 in total, and the only one
of the three that goes over).

```
pio run -e ex09_menu_widget -t upload -t monitor
```

Needs `-D AFFA_PANEL_CARMINAT=1 -D AFFA_ENABLE_MENU=1` (both set by the env).

## What it does

One `affa::widget::MenuModel` holding three items — an integer field (`Bright: 50%`), a list
field (`BT: AUTO`) and a three-field item (`Clock: 12 30 24h`) — is rendered onto each of
three displays in turn, a few seconds each, with the **same scripted key stream** applied to
every one of them. The panel's own wheel and Load button drive the same model at the same
time.

| Renderer | Geometry | `endFrame()` emits | Scroll mask | Selection |
| --- | --- | --- | --- | --- |
| `CarminatMenuRenderer` | 2 × 26 | `showMenu(header,row0,row1,mask)` + `highlightItem(row)` | **passes through** — the values *are* its wire bytes | `0x7E` / `0x7F` row tag |
| `InfoMenuRenderer` | 3 × 8 | `showInfoPopup(r0,r1,r2)` | **ignores** — no arrows on that screen | none — the screen has no highlight |
| `TextPanelRenderer` | 6 × 20 | a box on `Serial` | **translates** — `^` / `v` glyphs | `>` marker |

The third one includes `<Arduino.h>` and `widget/MenuModel.h` and nothing else. No
`CarminatDisplay`, no ISO-TP, no CAN, no wire constant — a display the library has never
heard of, running the identical state machine.

## What the model does not know, and who does

The row **tags** `0x7E`/`0x7F` live in `CarminatMenuRenderer::endFrame()`. The model speaks
row *index* 0..rows-1. Same for the highlight frame, the 96-byte screen, the character set,
and the fact that this panel has exactly two rows — the model will happily run a six-row
window, and it is the adapter that has to reject or clamp a geometry its glass cannot draw.

`IMenuRenderer` has no way to say *"only the lit row changed"*: a redraw is always
`beginFrame` → `row`×N → `endFrame`. On the Carminat that distinction is worth ~14× the bus
time (a highlight frame versus a 96-byte ISO-TP screen), so the adapter recovers it — it
caches the header, rows and mask it last sent, and an otherwise identical frame with a
different `selected` row emits the highlight alone. Construct it with
`CarminatMenuRenderer(display, /*coalesceHighlight=*/false)` to watch the bus cost double.

## Things that will bite

* **`MenuModel` has no key vocabulary.** It has six intents (`next`/`prev`/`select`/`back`/
  `increase`/`decrease`); mapping `(Key, KeyEdge)` onto them is now the application's, and so
  is the `default: break` in `onKey()`. The old `Menu::handleKey` returned `false` for
  `SrcNext`/`SrcPrev`/`VolUp`/`VolDown`/`Pause` so they reached the application even while the
  menu was open — route *every* key into the model and you swallow them.
* **The geometry is fixed at construction, on purpose.** A window cannot change height under
  a live selection without a rule for where the selection lands. Switching displays therefore
  destroys and re-constructs the model in a static buffer (placement `new` — no heap, the
  same constraint `src/widget/` holds itself to) and rebuilds the items. The selection resets
  to the top, which is why the key script restarts with it.
* **Item strings are caller-owned and pointed at, never copied.** `label`, `unit` and every
  entry of a `listField` must outlive the model: file-scope constants, not stack strings.
* **A closed model consumes nothing.** `back()` (hold-Load) closes the menu and every
  subsequent intent returns `false`; reopening is application policy, and this example does
  it in `runScript()`.
* **The library's own `Menu` is still compiled in and left empty.** `AFFA_ENABLE_MENU=1`
  turns on both widgets; `setup()` calls `clearMenuHotkey()` so the OEM gesture cannot reach
  the old one. `src/carminat/Menu/` is untouched, and migrating `CarminatDisplay` onto
  `MenuModel` is a separate change.
* **Truncation is the model's, and it is character-safe.** Every caller-owned string goes
  through `affa::toAscii` before it reaches the row buffer, so what gets cut at `rowChars` is
  always 7-bit and a multi-byte sequence is never split.
* **Eight characters is not enough for this menu, and the demo shows it.** On the info-row
  screen `Bright: 50%` renders as `Bright:` and `Clock: 12 30 24h` as `Clock: 1` — the labels
  survive and the values do not. The model is doing exactly what it was told; the geometry is
  the problem. A menu built for that screen wants three-character labels, or a layout that
  puts the value on its own row. Worth knowing *before* wiring a settings list to it.
