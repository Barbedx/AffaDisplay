# 03_carminat_menu — the menu, the highlight, and the keys

```
pio run -e ex03_carminat_menu -t upload -t monitor
```

Needs `-D AFFA_PANEL_CARMINAT=1 -D AFFA_ENABLE_MENU=1` (both are set by the env).

## On the panel

| Gesture | Effect |
| --- | --- |
| hold Load | opens the library menu (the OEM default hotkey) |
| wheel up / down | moves the selection; in edit mode, changes the value |
| Load | activates the item: `onActivate`, or enter edit mode |
| Load, while editing | next field, or leave edit after the last one |
| hold Load, while open | closes the menu → `setText("RENAULT")` |
| VolUp | `showMenu()` + `highlightItem()` **directly** — no Menu behind them |
| Pause / VolDown | `showPopupText("VOL 28")` / `hidePopup()`, behind `supports()` |
| SrcNext / SrcPrev | `showFullscreenText()` / `hideFullscreenText()`, behind `supports()` |

## Two things that look alike and are not

`Menu` is the state machine: a window over a list, a selection, an edit mode, and the
knowledge that moving the selection *inside* the visible two rows is a **highlight** while
moving it outside is a **redraw**. It calls `showMenu()` and `highlightItem()` for you.

`showMenu()` / `highlightItem()` are the two wire operations. VolUp calls them directly to
draw a screen with no state machine attached — the same bytes, no library state changed.
That is why `RenderSlot::Highlight` is deliberately *not* `RenderSlot::Menu`: a queued
highlight must not replace a queued full redraw, or vice versa.

## Why the menu is in the library at all

None of the mechanism is separable from the wire. A menu screen is one 96-byte ISO-TP
payload with the header at a fixed offset, row 0 behind `00 7E` and row 1 behind `01 7F`;
the highlight is a *different* single frame; and the scroll-arrow byte is a function of the
selection's position in the list. Deciding "does this key redraw or just re-highlight" is
reasoning about the wire, not about the application.

What the application owns is all of the content: labels, fields, what a change means, and
persistence. `onChange` is where an NVS write goes — the library never touches
`Preferences`.

## Things that will bite

* **Item strings are caller-owned and pointed at, never copied.** `label`, `unit` and every
  string in a `listField` must outlive the `Menu`. File-scope constants and string
  literals are the intended sources; a `char[]` on the stack of `setup()` is not.
* **An empty menu never opens.** `openMenu()` returns false, because an open menu that
  renders nothing would swallow the wheel and Load with no way out. Build items first.
* **`setFieldValue()` re-renders only if the item is in the visible window.** That is what
  makes it safe to push a live sensor reading in at 1 Hz, as this example does.
* **A closed menu consumes nothing.** Every key reaches your `KeyCb` while it is shut,
  which is what lets an application implement its own open gesture (see
  `08_radio_mitm`).
* **Scroll indicator on 1- and 2-item menus is `0x00`** (no arrows), per `docs/API.md`
  §8.6. The extracted code returned `0x0B` there and read past the end of a one-item list.
