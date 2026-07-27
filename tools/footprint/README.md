# tools/footprint — the footprint instrument

Not a demo, and not something to flash. It exists so that a claim like
"`AFFA_ENABLE_MENU=0` removes 3 266 bytes" can be checked rather than believed.

```
pio run -c platformio_footprint.ini -e g_base      # the reference build
pio run -c platformio_footprint.ini               # every gate, both baselines, the guards
```

## Why the `size_*` envs cannot answer this

`size_all`, `size_carminat` and `size_min` build `examples/01_link_check`, which declares its
own minimal `AffaDisplayBase` subclass and **names no panel class at all**. `--gc-sections`
therefore drops every compiled-but-unreferenced panel before the image is written. That is a
real and useful result — *an unused panel costs zero, measurably* — and it is the wrong
instrument for "what did this gate actually remove", because the gate has nothing left to
remove.

This probe does the opposite: it instantiates **every panel the build selected** and calls
**every optional render** on each, plus the menu, the marquee, the subscription table, the
transliterator, the twin and the real `Esp32CanLink`. Nothing is collectable. One `AFFA_*`
flag flipped against `g_base` is then that flag's honest cost, and `riscv32-esp-elf-nm -C -S`
on the two `firmware.elf` files says which symbols went and how much each shrank by.

## Reading the result

Flash and RAM come straight from the `pio run` size line. For symbol-level evidence:

```
nm -C -S .pio/gates/g_base/firmware.elf     | grep CarminatDisplay::showPopupText
nm -C -S .pio/gates/g_no_popup/firmware.elf | grep CarminatDisplay::showPopupText
```

A working screen gate collapses its builder to a **four-byte** `return NotSupported`; a
working panel or feature gate makes the symbol vanish outright.

## The negative environments

`g_neg_typo` and `g_neg_task` are expected to **fail**. They exercise the two `#error`s in
`AffaConfig.h`:

* `g_neg_typo` passes `-D AFFA_PANEL_CARMINET=1` — the misspelling that `-Wundef` structurally
  cannot catch, because the misspelled macro *is* defined and merely never read. All three
  real panel flags stay 0, and the "no panel selected" guard fires.
* `g_neg_task` sets `AFFA_ENABLE_TASK=1`, a knob that was documented and never implemented.

If either one ever *succeeds*, a guard has been broken.
