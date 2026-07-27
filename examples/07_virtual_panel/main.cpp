// 07_virtual_panel — the whole library, driven against a model of the panel, with no CAN
// controller and no panel attached.
//
// A CarminatDisplay and a CarminatVirtualPanel sit on two LoopbackLinks that this file
// wires together. Everything is real: the sync handshake, the lazy 0x70 function
// registration, the ISO-TP fragmentation, the per-frame ACKs, the key decode. The only
// thing that is not real is the wire, and the twin's job is to make even that indistin-
// guishable — it terminates transfers at the declared FF_DL exactly as the hardware does,
// so showMenu is 13 frames ending at PCI 0x2C here, not the 14 a self-ACK emulator gives.
//
// After each step it prints the twin's DECODED SCREEN. That is the point of the twin:
// the assertion you want to make is "the panel shows CLOCK / Hours / Minutes", not "these
// 96 bytes matched".
//
// Builds for the host (`pio run -e ex07_virtual_panel`) and, unchanged, for the C3 with
// the twin gate on (`pio run -e ex07_virtual_panel_c3`) — the second is there to prove
// that vpanel/ compiles for the target, not because you would ship it.

#include "AffaConfig.h"

#if defined(ARDUINO)
#  include <Arduino.h>
#  define OUT(...) Serial.printf(__VA_ARGS__)
#else
#  include <cstdio>
#  define OUT(...) std::printf(__VA_ARGS__)
#endif

#include <AffaDisplay.h>

#if !AFFA_ENABLE_VIRTUAL_PANEL || !AFFA_PANEL_CARMINAT
#  error "07_virtual_panel needs -D AFFA_ENABLE_VIRTUAL_PANEL=1 -D AFFA_PANEL_CARMINAT=1"
#endif

namespace {

// ---------------------------------------------------------------------------
// The harness. It lives HERE and not in src/ deliberately: connecting two ICanLink
// endpoints is a test/example concern, and putting a fabric in the library would invite
// an application to ship one.
// ---------------------------------------------------------------------------

struct FakeClock final : affa::IClock {
  uint32_t t = 0;
  uint32_t millis() const override { return t; }
  void     advance(uint32_t ms) { t += ms; }
};

affa::LoopbackLink<>       g_radioLink;   // what the library transmits through
affa::LoopbackLink<>       g_panelLink;   // what the twin transmits through
FakeClock                  g_clock;
affa::CarminatDisplay      g_display(g_radioLink, g_clock);
affa::CarminatVirtualPanel g_twin;

bool     g_trace     = false;
uint32_t g_radioTx   = 0;   // frames the library put on the wire, total
// Counted separately from g_radioTx, because the 1 Hz heartbeat keeps running underneath
// every step and would otherwise make showMenu look like 14 frames instead of 13 — which
// is precisely the miscount this library's documentation warns about, arriving from a
// different direction.
uint32_t g_radioData  = 0;
uint32_t g_stepStart  = 0;

void dumpFrame(const char* dir, const affa::Frame& f) {
  OUT("      %s %03X ", dir, static_cast<unsigned>(f.id));
  for (uint8_t i = 0; i < f.len; ++i) OUT("%02X ", f.data[i]);
  OUT("\n");
}

// One bus cycle: pump the library, hand what it transmitted to the twin, hand what the
// twin transmitted back to the library, advance the clock.
//
// Note the twin is FED frames and never drains a link of its own. That is what lets the
// identical class sit on a real bus in PASSIVE mode, or replay a recorded capture.
void cycle(int n, uint32_t stepMs = 5) {
  affa::Frame f;
  for (int i = 0; i < n; ++i) {
    g_display.poll();

    while (g_radioLink.takeSent(f)) {           // library -> panel
      ++g_radioTx;
      if (f.id != affa::carminat_twin::kIdSync) ++g_radioData;
      if (g_trace) dumpFrame("radio->panel", f);
      g_twin.onFrame(f);
    }
    while (g_panelLink.takeSent(f)) {           // panel -> library
      if (g_trace) dumpFrame("panel->radio", f);
      g_radioLink.inject(f);
    }
    g_clock.advance(stepMs);
  }
}

void beginStep(const char* title) {
  g_stepStart = g_radioData;
  OUT("\n=== %s\n", title);
}

const char* modeName(affa::ScreenModel::Mode m) {
  switch (m) {
    case affa::ScreenModel::Mode::Menu: return "MENU";
    case affa::ScreenModel::Mode::Info: return "INFO";
    default:                            return "NONE";
  }
}

void dumpScreen() {
  const affa::ScreenModel& s = g_twin.screen();
  OUT("    data frames on the wire for this step : %lu   (heartbeats excluded)\n",
      static_cast<unsigned long>(g_radioData - g_stepStart));
  OUT("    twin.screen()  mode=%s  scroll=0x%02X  sel=%d  decoded@%lums\n",
      modeName(s.mode), static_cast<unsigned>(s.scroll), static_cast<int>(s.sel),
      static_cast<unsigned long>(g_twin.lastDecodeMs()));
  OUT("      header : \"%s\"\n", s.header);
  OUT("      row0   : \"%s\"   (tag 0x%02X)\n", s.row0, static_cast<unsigned>(s.row0Id));
  OUT("      row1   : \"%s\"   (tag 0x%02X)\n", s.row1, static_cast<unsigned>(s.row1Id));
  for (uint8_t i = 0; i < s.infoCount; ++i)
    OUT("      info%u  : \"%s\"\n", static_cast<unsigned>(i), s.info[i]);
}

const char* resultName(affa::Result r) {
  switch (r) {
    case affa::Result::Ok:           return "Ok";
    case affa::Result::NoSync:       return "NoSync";
    case affa::Result::UnknownFunc:  return "UnknownFunc";
    case affa::Result::SendFailed:   return "SendFailed";
    case affa::Result::Timeout:      return "Timeout";
    case affa::Result::TooLong:      return "TooLong";
    case affa::Result::QueueFull:    return "QueueFull";
    case affa::Result::NotSupported: return "NotSupported";
    case affa::Result::BadArgument:  return "BadArgument";
    case affa::Result::LinkDown:     return "LinkDown";
    case affa::Result::Cancelled:    return "Cancelled";
    case affa::Result::Busy:         return "Busy";
    case affa::Result::Aborted:      return "Aborted";
  }
  return "?";
}

int      g_keys     = 0;
affa::Key g_lastKey = affa::Key::Load;

void onKey(affa::Key k, affa::KeyEdge e, void*) {
  ++g_keys;
  g_lastKey = k;
  OUT("    [key] 0x%04X %s\n", static_cast<unsigned>(k),
      e == affa::KeyEdge::Hold ? "hold" : "click");
}

void runDemo() {
  OUT("AffaDisplay 07_virtual_panel — CarminatDisplay against CarminatVirtualPanel\n");

  // Declared, not Done: answer PARTIAL while the declared FF_DL is unsatisfied and DONE at
  // it, which is what the hardware does. With the default Done the twin would terminate
  // every multi-frame transfer after frame 0 — the transmit FSM treats "DONE while bytes
  // remain" as SUCCESS, because showMenu depends on exactly that — and the twin would see
  // 8 bytes of a 96-byte screen.
  g_twin.setAckMode(affa::VirtualPanelBase::AckMode::Declared);
  g_twin.begin(g_panelLink, g_clock);

  g_display.onKey(&onKey, nullptr);
  g_display.begin();

  // ---- 1. handshake + registration ---------------------------------------
  // begin() transmits nothing; the first heartbeat leaves on the first poll(). The twin
  // answers 0xB9 with the panel's 0x69 peer-alive and 0xBA with `61 11`, which makes the
  // driver emit its three hello frames; the 0x70 in the first of them is what tells the
  // twin the link is up.
  beginStep("1. sync handshake");
  g_trace = true;
  cycle(6);
  g_trace = false;
  cycle(120);      // let a couple of heartbeat periods pass
  OUT("    display.synced()=%d registered()=%d   twin.synced()=%d syncReplies=%lu\n",
      g_display.synced() ? 1 : 0, g_display.registered() ? 1 : 0,
      g_twin.synced() ? 1 : 0,
      static_cast<unsigned long>(g_twin.syncRepliesSent()));

  // ---- 2. setText --------------------------------------------------------
  // The first payload after a resync drags the lazy 0x70 registration burst in front of
  // it: one probe per entry of the function table, IN DECLARATION ORDER (0x151 then
  // 0x1F1), then the payload. A twin that acknowledged only 0x151 would stall here.
  beginStep("2. setText(\"RENAULT\")   -> 0x151, command 0x77, 3 frames");
  // Two statements and not one OUT() with two calls in it: the order in which a call's
  // arguments are evaluated is unspecified, and lastEnqueued() read before setText() runs
  // reports the PREVIOUS ticket. Cheap to write, silent when wrong.
  const affa::Result r2 = g_display.setText("RENAULT");
  OUT("    enqueue -> %s (ticket %u)\n", resultName(r2),
      static_cast<unsigned>(g_display.lastEnqueued()));
  cycle(40);
  OUT("    registered()=%d  lastResult=%s\n", g_display.registered() ? 1 : 0,
      resultName(g_display.lastResult()));
  dumpScreen();

  // ---- 3. showMenu -------------------------------------------------------
  // 96 bytes built, 0x5A = 90 declared. The twin stops the transfer at the declared
  // length, so 13 frames go out and it holds 92 payload bytes — payload[92..95], the last
  // four cells of row1, never reach a real panel either.
  beginStep("3. showMenu(\"CLOCK\", \"Hours\", \"Minutes\")   -> 13 frames, last PCI 0x2C");
  OUT("    enqueue -> %s\n",
      resultName(g_display.showMenu("CLOCK", "Hours", "Minutes")));
  cycle(60);
  OUT("    lastResult=%s\n", resultName(g_display.lastResult()));
  dumpScreen();

  // ---- 4. highlightItem --------------------------------------------------
  // A single frame `07 29 01 7F 80 00 00 00`. sel carries the ROW TAG, 0x7F, because that
  // is what the wire carries and what the panel latches.
  beginStep("4. highlightItem(1)   -> single frame, sel becomes 0x7F");
  OUT("    enqueue -> %s\n", resultName(g_display.highlightItem(1)));
  cycle(20);
  dumpScreen();

  // ---- 5. showInfoPopup --------------------------------------------------
  // THREE SEPARATE MESSAGES, one per row, two frames each. The legacy delay(5) between
  // them is gone, so nothing guarantees a row's frames stay adjacent if an Urgent message
  // lands in between — which is exactly why they are three messages and not one.
  beginStep("5. showInfoPopup(\"AUX ON\", \"AF ON\", \"SPEED 0\")   -> 3 messages");
  OUT("    enqueue -> %s\n",
      resultName(g_display.showInfoPopup("AUX ON", "AF ON", "SPEED 0")));
  cycle(60);
  dumpScreen();

  // ---- 6. a key from the panel -------------------------------------------
  // The twin transmits `03 89 01 41` on 0x1C1 and the driver decodes it. RollDown is one
  // of the two encoder codes that are EXEMPT from hold masking: 0x40 is simultaneously
  // its direction bit and half the hold mask, so masking would turn every wheel-down
  // detent into a wheel-up.
  beginStep("6. twin.transmitKey(RollDown)   -> 0x1C1 03 89 01 41");
  g_trace = true;
  g_twin.transmitKey(static_cast<uint16_t>(affa::Key::RollDown), false);
  cycle(4);
  g_trace = false;
  OUT("    keys delivered=%d  last=0x%04X (expect 0x0141)\n", g_keys,
      static_cast<unsigned>(g_lastKey));

  OUT("\ndone. %lu frames from the radio, %lu ACKs and %lu sync replies from the twin.\n",
      static_cast<unsigned long>(g_radioTx),
      static_cast<unsigned long>(g_twin.acksSent()),
      static_cast<unsigned long>(g_twin.syncRepliesSent()));
}

}  // namespace

#if defined(ARDUINO)
void setup() {
  Serial.begin(115200);
  delay(200);        // the application may sleep; the library may not
  runDemo();
}
void loop() {}
#else
int main() {
  runDemo();
  return 0;
}
#endif
