# 07_virtual_panel — the whole library, no hardware

```
pio run -e ex07_virtual_panel -t exec     # host: builds AND runs it
pio run -e ex07_virtual_panel_c3          # the same file, unchanged, for the C3
```

A `CarminatDisplay` and a `CarminatVirtualPanel` on two `LoopbackLink`s that the example
wires together. Everything is real — the handshake, the lazy `70` function registration,
the ISO-TP fragmentation, the per-frame ACKs, the key decode. The only thing that is not
real is the wire, and the twin's job is to make even that indistinguishable.

After each step it prints the twin's **decoded screen**, which is the point: the assertion
worth making is "the panel shows CLOCK / Hours / Minutes", not "these 96 bytes matched".

## `AckMode::Declared` is why the frame counts come out right

A real panel terminates a transfer at the **declared FF_DL**. The example sets
`AckMode::Declared` — answer PARTIAL while the declared length is unsatisfied, DONE at it —
and the twin then reproduces every count in `docs/WIRE-SPEC.md` without being told them:
`showMenu` at **13** frames ending at PCI `0x2C`, not the self-ACK emulator's 14/`0x2D`;
`setText` at 3; UpdateList `setText` at 4 and its LCD variant at 5.

The other two modes cannot model a panel at all: with `Done` the transmit FSM correctly
treats "DONE while bytes remain" as **success**, so every multi-frame transfer terminates
after frame 0 and the twin sees 8 bytes of a 96-byte screen; with `Partial` the last frame
draws a PARTIAL with no bytes left and the sender reports `SendFailed`. `Done` remains the
default because `docs/API.md` §2.14 says so.

## Two things not to conclude from a twin run

* **The twin pads with the profile filler** (`0x00` / `0x81`); a real panel pads `0xA3`. A
  golden vector recorded from the twin therefore differs from a bench capture in the pad
  bytes, and neither is wrong — the filler is per-node and nothing in the library ever
  matches on a received one.
* **The C3 env exists to prove `proto/` and `vpanel/` compile for the target**, not because
  you would ship a panel model to a board that has a panel attached.
