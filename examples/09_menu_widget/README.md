# 09_menu_widget — one menu, three displays

**The menu algorithm is display-agnostic: the sliding window, the in-place editing, the
field kinds and the scroll-arrow derivation in `src/widget/` do not change by one line
between a 2×26 panel window, a 3×8 info row and a 6×20 OLED.** All that changes is a
`MenuGeometry` and a renderer whose panel-specific part is under thirty lines of code —
counting the geometry and the three `IMenuRenderer` methods, and not the comments: 16 lines
for the info-row screen, 27 for the OLED, and 24 for the Carminat menu screen plus 9 more
for the highlight-only optimisation the seam cannot express (33 in total, and the only one
of the three that goes over).

Only **two** of the three renderers are in this folder. The Carminat one ships in the library
as `src/carminat/CarminatMenuRenderer.{h,cpp}`, because `CarminatDisplay`'s own menu is built
on it — copying it here so the example could own a private one would be the second
implementation this library just finished deleting.

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

| Renderer | Geometry | `endFrame()` emits | Scroll mask | Selection | `highlightOnly()` |
| --- | --- | --- | --- | --- | --- |
| `affa::CarminatMenuRenderer` *(in the library)* | 2 × 26 | `showMenu(header,row0,row1,mask)` + `highlightItem(row)` | **passes through** — the values *are* its wire bytes | `0x7E` / `0x7F` row tag | **overrides** — one frame instead of a screen |
| `InfoMenuRenderer` | 3 × 8 | `showInfoPopup(r0,r1,r2)` | **ignores** — no arrows on that screen | none — the screen has no highlight | default — nothing to move |
| `TextPanelRenderer` | 6 × 20 | a box on `Serial` | **translates** — `^` / `v` glyphs | `>` marker | default — a printed line cannot be edited |

The third one includes `<Arduino.h>` and `widget/MenuModel.h` and nothing else. No
`CarminatDisplay`, no ISO-TP, no CAN, no wire constant — a display the library has never
heard of, running the identical state machine.

## What the model does not know, and who does

The row **tags** `0x7E`/`0x7F` live in `CarminatMenuRenderer::sendHighlight()`. The model speaks
row *index* 0..rows-1. Same for the highlight frame, the 96-byte screen, the character set,
and the fact that this panel has exactly two rows — the model will happily run a six-row
window, and it is the adapter that has to reject or clamp a geometry its glass cannot draw.

*"Only the lit row changed"* is `IMenuRenderer::highlightOnly(index)`, the seam's fourth call.
On the Carminat that distinction is worth ~14× the bus time (one `07 29 01` frame versus a
96-byte ISO-TP screen), and the **model** raises it — exactly when the window did not move —
so no adapter has to detect the case. `CarminatMenuRenderer` overrides it in four lines and
returns `true`; the other two do not override it at all and get a full frame, which is the
correct answer for a screen with no highlight (`InfoMenuRenderer`) and for one that can only
reprint the whole box (`TextPanelRenderer`). Construct it with
`CarminatMenuRenderer(display, /*coalesceHighlight=*/false)` to decline the call and watch the
bus cost double.

## Things that will bite

* **`MenuModel` has no key vocabulary.** It has six intents (`next`/`prev`/`select`/`back`/
  `increase`/`decrease`); mapping `(Key, KeyEdge)` onto them is the caller's, and so is the
  `default: break` in `onKey()`. The deleted `Menu::handleKey` returned `false` for
  `SrcNext`/`SrcPrev`/`VolUp`/`VolDown`/`Pause` so they reached the application even while the
  menu was open — route *every* key into the model and you swallow them. (For
  `CarminatDisplay`'s own menu that map lives in `MenuController::routeKey`, with the same
  `default:`; `onKey()` below is the hand-written equivalent for a model you own.)
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
* **`CarminatDisplay`'s own menu is the same model, and this example leaves it empty.** Since
  the migration `getMenu()` hands out a `widget::MenuModel` driven through the very
  `CarminatMenuRenderer` used for target 1 — there is no second implementation anywhere. What
  this example adds is a *second, application-owned* model on the same panel, so `setup()`
  calls `clearMenuHotkey()` and the library's empty one stays out of the way.
* **Truncation is the model's, and it is character-safe.** Every caller-owned string goes
  through `affa::toAscii` before it reaches the row buffer, so what gets cut at `rowChars` is
  always 7-bit and a multi-byte sequence is never split.
* **Eight characters is not enough for this menu, and the demo shows it.** On the info-row
  screen `Bright: 50%` renders as `Bright:` and `Clock: 12 30 24h` as `Clock: 1` — the labels
  survive and the values do not. The model is doing exactly what it was told; the geometry is
  the problem. A menu built for that screen wants three-character labels, or a layout that
  puts the value on its own row. Worth knowing *before* wiring a settings list to it.
