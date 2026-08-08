# Examples

Four. There were twenty; sixteen were deleted in 2.0 along with this file's catalogue of
them, which by then described more programs than the repository contained.

| | Board | What it is for |
| --- | --- | --- |
| `01_bringup` | any ESP32 | **Does the link come up at all, in the order it has to be proved?** Flash this first on new hardware. On a two-node bus `txErr == 0` is the proof the panel is acknowledging you — see `docs/NOTES.md` §2 on why `rx 0` alone tells you nothing. |
| `03_hello` | any ESP32 | The smallest correct program: bring the panel up, put text on it, stop. Sixty lines. |
| `17_mediascreen` | DevKit V1 | **The console.** Every render the library has, a live frame ring, key capture, the byte-level probes, and the nav-header sweep card. This is what to run to see what the library does, and the reference for the owned task — forty concurrent HTTP renders against a real panel with every counter at zero. |
| `18_aiscreen` | DevKit V1 | A content feed driven by anything that can compose a `DisplayDocument` — an LLM, a weather service, `curl`. The producer composes **screens**, never AffaDisplay commands. Works with no network at all: a built-in deck rotates until a producer takes over. |

```
pio run -e ex01_bringup -t upload
pio run -e ex17_mediascreen -t upload
```

`shared/` is not an example: `media_render.h` draws the 48 × 48 nav pane on the device
(clock, spectrum, VU, rings, robot eyes) and `nav_images.h` holds the baked bitmaps, which
`tools/gen_navicons.js` generates.

The panel opens the conversation, so a silent bus is normal until it speaks — the library
announces a bare `BA` every ~30 s and waits. `docs/WIRE.md` is what goes out, generated from
the vectors CI asserts.
