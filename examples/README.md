# Examples

Five. There were twenty; the rest went in 2.0 along with this file's catalogue of them,
which by then described more programs than the repository contained.

`01_bringup` went last, and for a reason worth recording: it was a SECOND CONSOLE — its own
frame ring, log ring and JSON builder — carried for the sake of one thing `17_mediascreen`
lacked, a sequenced self-check. So the self-check moved and the console did not need
repeating.

| | Board | What it is for |
| --- | --- | --- |
| `03_hello` | any ESP32 | The smallest correct program: bring the panel up, put text on it, stop. Sixty lines. |
| `17_mediascreen` | DevKit V1 | **The console, and what to flash first on new hardware.** Every render the library has, a live frame ring, key capture, a **self-check** that proves the link one step at a time and names the step that failed, and a byte-level **override** that reads the last payload back and sends one you typed. Also the reference for the owned task — forty concurrent HTTP renders against a real panel with every counter at zero. |
| `18_aiscreen` | DevKit V1 | A content feed driven by anything that can compose a `DisplayDocument` — an LLM, a weather service, `curl`. The producer composes **screens**, never AffaDisplay commands. Works with no network at all: a built-in deck rotates until a producer takes over. |
| `20_carminat_fsm` | either | **The opening with nothing around it.** One page, one file: the handshake, the registration and the clock, written by hand on `esp32_can` and driven entirely from the CAN receive callback — no queue, no ISO-TP, no library FSM, no network. Every frame it sends answers one the panel sent; the announce (`BA`) is the single exception and is off unless you type `ba`. Read this to learn the protocol; ship `03_hello`. |
| `19_cantest` | both, flash **both** | **No panel and no protocol** — the bring-up rig for when a board hears nothing. `selftest` proves one board alone (self-reception, no ACK, out through the transceiver and back); `pingpong` proves the bus between two; `listen` watches without even acknowledging. Switch the bitrate, or the whole CAN stack between raw TWAI and `CanCommonLink`, from a link on the page. |

```
pio run -e ex17_mediascreen -t upload
```

Reach for `19_cantest` before diagnosing anything through the protocol. Every conclusion that
had to be retracted on this bench came from measuring across three moving variables at once —
panel, bus and library — and this example removes two of them.

`shared/` is not an example: `net.h` brings up WiFi, mDNS, OTA and the PsychicHttp knobs
that keep a board reachable; `media_render.h` draws the 48 × 48 nav pane on the device
(clock, spectrum, VU, rings, robot eyes); `nav_images.h` holds the baked bitmaps, which
`tools/gen_navicons.js` generates.

The panel opens the conversation, so a silent bus is normal until it speaks — the library
announces a bare `BA` every ~30 s and waits. `docs/WIRE.md` is what goes out, generated from
the vectors CI asserts.
