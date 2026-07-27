// 04_updatelist_segment — the AFFA2 8-segment panel: a scrolling title and a clock.
//
// The marquee is a LIBRARY feature because the eight-cell window and its 400 ms cadence
// are panel geometry; what a track is CALLED is application territory, so the seam is a
// plain string plus a play/pause bit. Position is a pure function of the clock —
// (base + (now - epoch) / 400) mod len — so it cannot drift, cannot burst after a stalled
// loop, and is identical whether poll() runs at 5 Hz or 5 kHz.
//
// THE CLOCK IS DRAWN AS TEXT, ON PURPOSE. supports(Feature::Time) is FALSE on this family
// and setTime() returns NotSupported: there is no clock command on this wire. The extracted
// code's setTime() returned NoError and put nothing on the bus, which is exactly the silent
// no-op this library exists to stop returning. So the application formats "HH:MM:SS" — it
// is exactly eight characters, which is the whole display — and renders it with setText().
//
// Publishing an EMPTY title switches the marquee off and transmits NOTHING, rather than
// blanking the panel: that is how the clock phase gets the screen to itself.

#include <Arduino.h>
#include <AffaDisplay.h>

#if !AFFA_PANEL_UPDATELIST
#  error "04_updatelist_segment needs -D AFFA_PANEL_UPDATELIST=1"
#endif

namespace {

struct ArduinoClock final : affa::IClock {
  uint32_t millis() const override { return ::millis(); }
};

constexpr affa::CanPins kPins{ .rx = GPIO_NUM_4, .tx = GPIO_NUM_3 };

affa::Esp32CanLink     g_link;
ArduinoClock           g_clock;
affa::UpdateListDisplay g_display(g_link, g_clock);

constexpr char     kTitle[]     = "AFFADISPLAY - NON BLOCKING MARQUEE";
constexpr uint32_t kScrollPhase = 12000;   // ms of scrolling title
constexpr uint32_t kClockPhase  =  6000;   // ms of clock
constexpr uint32_t kBootSeconds = 12 * 3600;   // pretend we booted at 12:00:00

bool     g_showingClock = false;
uint32_t g_phaseEndMs   = 0;
uint32_t g_nextClockMs  = 0;

void drawClock() {
  const uint32_t s = (kBootSeconds + ::millis() / 1000) % 86400;
  char buf[9];                                     // 8 cells + NUL, exactly the display
  snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu",
           static_cast<unsigned long>(s / 3600),
           static_cast<unsigned long>((s / 60) % 60),
           static_cast<unsigned long>(s % 60));
  g_display.setText(buf);        // RenderSlot::Text — a re-render supersedes a queued one
}

void onSync(affa::SyncState s, void*) {
  Serial.printf("[sync] 0x%02X synced=%d registered=%d\n", static_cast<unsigned>(s),
                g_display.synced(), g_display.registered());
}

void onKey(affa::Key k, affa::KeyEdge e, void*) {
  Serial.printf("[key ] 0x%04X %s  amsKeysEnabled=%d\n", static_cast<unsigned>(k),
                e == affa::KeyEdge::Hold ? "hold" : "click",
                g_display.amsKeysEnabled());
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);
  if (!g_link.begin(kPins, 500000)) { Serial.println("CAN did not come up"); return; }

  Serial.printf("supports Text=%d Time=%d Menu=%d Power=%d KeyTx=%d\n",
                g_display.supports(affa::Feature::Text),
                g_display.supports(affa::Feature::Time),      // 0 — see the header
                g_display.supports(affa::Feature::Menu),
                g_display.supports(affa::Feature::Power),
                g_display.supports(affa::Feature::KeyTx));

  g_display.onSync(&onSync, nullptr);
  g_display.onKey(&onKey, nullptr);
  g_display.begin();
  g_display.setPower(true);            // 04 52 02 FF FF on 0x1B1, padded with 0x81

  // Hold-Load toggles AMS key forwarding and draws its own banner — the OEM gesture,
  // shipped ON as a replaceable default. Left alone here so the banner is visible; 05
  // clears it, because there the application wants that gesture for itself.
  g_display.setScrollText(kTitle);
  g_display.setScrollActive(true);
  g_phaseEndMs = ::millis() + kScrollPhase;
}

void loop() {
  g_display.poll();                    // the marquee steps from inside onPoll()

  const uint32_t now = ::millis();

  if (affa::expired(now, g_phaseEndMs)) {
    g_showingClock = !g_showingClock;
    g_phaseEndMs   = now + (g_showingClock ? kClockPhase : kScrollPhase);
    if (g_showingClock) {
      g_display.setScrollText(nullptr);   // marquee off, and NOTHING is transmitted
      g_nextClockMs = now;
    } else {
      g_display.setScrollText(kTitle);    // new content -> redraws from position 0
      g_display.setScrollActive(true);
    }
    Serial.printf("[app ] phase -> %s\n", g_showingClock ? "clock" : "marquee");
  }

  if (g_showingClock && affa::expired(now, g_nextClockMs)) {
    g_nextClockMs = now + 1000;        // = now + period, never +=
    drawClock();
  }
}
