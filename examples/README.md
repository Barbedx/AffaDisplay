# Examples

Four, all on `affa::TwaiLink`.

| | Board | What it is for |
| --- | --- | --- |
| `03_hello` | any ESP32 (pins from `AFFA_CAN_RX` / `AFFA_CAN_TX`; the env builds for a C3) | The smallest correct program: bring the panel up, put text on it, stop. No WiFi: flash over USB. |
| `04_sniffer` | any ESP32 | The link with **no panel**: `<AffaTwai.h>`, listen-only, one line per frame. Built with no `AFFA_PANEL_*` flag, which is what proves the library still compiles without one. |
| `17_mediascreen` | DevKit V1 (`ex17_mediascreen`), C3 (`ex17_mediascreen_c3`) | **The console, and what to flash first on new hardware.** Every render the library has, a live frame ring, key capture, a **self-check** that proves the link one step at a time and names the step that failed, and a byte-level **override** that reads the last payload back and sends one you typed. Also the reference for the owned task — forty concurrent HTTP renders against a real panel with every counter at zero. |
| `18_aiscreen` | DevKit V1 | A content feed driven by anything that can compose a `DisplayDocument` — an LLM, a weather service, `curl`. The producer composes **screens**, never AffaDisplay commands. Works with no network at all: a built-in deck rotates until a producer takes over. |

```
pio run -e ex17_mediascreen -t upload
```

`shared/` is not an example: `net.h` brings up WiFi, mDNS, OTA and the PsychicHttp settings
that keep a board reachable; `media_render.h` draws the 48 × 48 nav pane on the device
(clock, spectrum, VU, rings, robot eyes); `nav_images.h` holds ready-made bitmaps.

The panel opens the conversation, so a silent bus is normal until it speaks — the library
announces a bare `BA` every ~30 s and waits. `docs/WIRE.md` is what goes out, generated from
the vectors CI asserts.
