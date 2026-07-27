# 05_updatelist_menu — a menu on the mono LCD

```
pio run -e ex05_updatelist_menu -t upload -t monitor
```

`-D AFFA_PANEL_UPDATELIST_MENU=1` (which promotes `AFFA_PANEL_UPDATELIST` automatically —
the LCD class derives from the 8-segment one and inherits its marquee).

## The point of this example

```
supports Text=1 Menu=0 Popup=0 Power=1 KeyTx=1
```

**`Menu=0`, and that is the correct answer.** The library's `Menu` is a two-row sliding
window over a 96-byte Carminat screen payload — a state machine welded to a wire format
this panel does not have. Here a "menu" is a list the *application* keeps and draws one
line at a time through `setText()`. The boundary principle decides it: rendering geometry
the panel does not possess cannot be library-owned, so the honest answer is `NotSupported`
rather than a `showMenu()` that silently draws nothing.

What stays the library's, and is doing real work in this file:

* the `10 1C 7F 55 55 FF 60 03 old(8) 10 new(12) 00` LCD `setText` encoding on `0x121` —
  30 payload bytes, 5 frames, last PCI `0x24`. The segment panel's `10 19 76 ..` form is a
  different method on a different class, and neither leaks into the application;
* the handshake on `0x3DF`/`0x3CF`, the lazy `70` registration of `0x121` and `0x1B1`;
* the key decode on `0x0A9` including the `03 89` guard, and the auto-ACK on **`0x4A9`**
  (`0x0A9 | 0x400` — bit 8 is already clear in `0x0A9`, uniquely in the table, so it is
  `0x4A9` and not `0x5A9`; always compute it);
* the marquee, borrowed here to scroll a label longer than eight cells. That is why this
  file contains no scrolling code at all.

## Controls

| Gesture | Effect |
| --- | --- |
| wheel | move the selection; while editing, change the value |
| Load | enter / leave edit on the selected item |
| hold Load | "back" — leaves edit |

## `clearAmsHotkey()` is load-bearing here

Hold-Load is this family's **AMS key-forwarding toggle** by default, drawn on screen as
`AMS  ON ` / `AMS OFF `. It is the OEM gesture, so it ships on; it is UI policy, so it can
be replaced. This application wants hold-Load for "back", so it calls `clearAmsHotkey()`
in `setup()`.

Without that call the first hold-Load would toggle forwarding **off**, and then *no key
reaches `onKey()` at all* — the panel would look dead while Layer 0 kept showing perfectly
good `0x0A9` frames. That is the intended semantic of the switch, and it is exactly the
kind of thing worth knowing before you debug it.

The `AMS` item in the list drives `setAmsKeysEnabled()` directly, which is the silent form:
it changes the state without drawing the banner, because a programmatic change is the
application's to announce or not.
