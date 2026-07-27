// 11_now_playing — three rows, each one moving on its own clock.
//
//     SPOTIFY [####----] 0:12      <- source, progress bar, elapsed. Live, not scrolled.
//     My favourite song - Fidel..  <- title,  scrolling at 300 ms/cell
//     Michael Jackson              <- artist, scrolling at 450 ms/cell
//
// THE POINT IS THAT THE THREE ARE INDEPENDENT. Each row owns a widget::Marquee with its own
// geometry and its own step rate, so they slide past each other rather than marching in
// lockstep — which is what a real now-playing screen looks like and what a single shared
// scroll position cannot do.
//
// This example is also the argument for the marquee having left UpdateList. It used to be
// welded into UpdateListDisplay and hard-coded to that panel's eight cells; nothing about
// scrolling text was ever AFFA2-specific, and here it is driving a Carminat — a panel that
// never had a marquee at all — at three different widths and speeds at once.
//
// WHAT IS NOT HERE, DELIBERATELY: where the track came from. No Bluetooth, no AVRCP, no
// ANCS, no media router. The application hands this screen three strings and a position in
// seconds; everything above that line is policy and belongs in the application.

#include <Arduino.h>
#include <AffaDisplay.h>
#include <cstdio>
#include <cstring>

#if !AFFA_PANEL_CARMINAT || !AFFA_ENABLE_MARQUEE
#  error "11_now_playing needs AFFA_PANEL_CARMINAT=1 and AFFA_ENABLE_MARQUEE=1"
#endif

namespace {

struct ArduinoClock final : affa::IClock {
  uint32_t millis() const override { return ::millis(); }
};

constexpr affa::CanPins kPins{ .rx = GPIO_NUM_4, .tx = GPIO_NUM_3 };

affa::Esp32CanLink    g_link;
ArduinoClock          g_clock;
affa::CarminatDisplay g_display(g_link, g_clock);

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------
// The Carminat window row is 26 usable bytes (AffaConfig.h; the last four cells of row1
// never reach the panel because it terminates at the declared FF_DL). Scroll inside 24 and
// there is margin whatever the panel does with the tail.
constexpr uint8_t  kCols     = 24;
constexpr uint8_t  kGap      = 4;     // blank cells before the wrap, so it reads as a pause
constexpr uint32_t kTitleMs  = 300;   // title scrolls faster...
constexpr uint32_t kArtistMs = 450;   // ...than the artist. Deliberately not a multiple.

affa::widget::Marquee g_title { affa::widget::MarqueeGeometry{kCols, kGap, kTitleMs}  };
affa::widget::Marquee g_artist{ affa::widget::MarqueeGeometry{kCols, kGap, kArtistMs} };

// ---------------------------------------------------------------------------
// The "now playing" state an application would own
// ---------------------------------------------------------------------------
constexpr const char* kSource   = "SPOTIFY";
constexpr uint16_t    kTrackSec = 217;          // 3:37

uint32_t g_startMs  = 0;
uint32_t g_nextMs   = 0;
char     g_lastHdr[32] = {0};
char     g_lastR0 [32] = {0};
char     g_lastR1 [32] = {0};

// `SPOTIFY [####----] 0:12` — eight cells of bar, filled by elapsed/total.
void buildHeader(char* out, size_t n, uint16_t elapsed) {
  constexpr uint8_t kBar = 8;
  char bar[kBar + 1];
  const uint8_t filled = static_cast<uint8_t>(
      (static_cast<uint32_t>(elapsed) * kBar) / (kTrackSec ? kTrackSec : 1));
  for (uint8_t i = 0; i < kBar; ++i) bar[i] = (i < filled) ? '#' : '-';
  bar[kBar] = '\0';
  snprintf(out, n, "%s [%s] %u:%02u", kSource, bar,
           static_cast<unsigned>(elapsed / 60), static_cast<unsigned>(elapsed % 60));
}

// ---------------------------------------------------------------------------
// The render tick
// ---------------------------------------------------------------------------
// Deadline-driven and CHANGE-DRIVEN, both. The deadline caps how often we may build a
// screen; the comparison decides whether we actually send one. showMenu is 96 bytes — 13
// frames on the wire — so re-sending an identical screen is the difference between a busy
// bus and an idle one, and RenderSlot::Menu coalescing would hide the waste rather than
// remove it.
void tick() {
  const uint32_t now = ::millis();
  if (!affa::expired(now, g_nextMs)) return;
  g_nextMs = now + 100;                       // 10 Hz ceiling; the marquees are slower

  const uint16_t elapsed =
      static_cast<uint16_t>(((now - g_startMs) / 1000u) % (kTrackSec + 1u));

  char hdr[32], r0[32], r1[32];
  buildHeader(hdr, sizeof(hdr), elapsed);
  g_title .window(g_title .windowAt(now), r0, sizeof(r0));
  g_artist.window(g_artist.windowAt(now), r1, sizeof(r1));

  if (strcmp(hdr, g_lastHdr) == 0 && strcmp(r0, g_lastR0) == 0 &&
      strcmp(r1, g_lastR1) == 0)
    return;                                   // nothing moved; say nothing

  strcpy(g_lastHdr, hdr); strcpy(g_lastR0, r0); strcpy(g_lastR1, r1);

  // kScrollNone: the arrows mean "there are more list items this way", and a now-playing
  // screen is not a list. Showing one would point at nothing.
  const affa::Result res = g_display.showMenu(hdr, r0, r1, affa::carminat::kScrollNone);
  if (res != affa::Result::Ok && res != affa::Result::NoSync)
    Serial.printf("[np  ] showMenu refused: %d\n", static_cast<int>(res));
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);
  if (!g_link.begin(kPins, 500000)) { Serial.println("CAN did not come up"); return; }

  g_display.begin();

  // The panel renders nothing while the display is powered off, and the symptom is a
  // perfectly healthy link drawing to a dark screen.
  (void)g_display.setPower(true);

  const uint32_t now = ::millis();
  g_startMs = now;
  g_title .setText("My favourite song - Fidel Castro", now);
  g_artist.setText("Michael Jackson", now);
  g_title .setActive(true, now);
  g_artist.setActive(true, now);

  Serial.println("now playing: three rows, three clocks");
}

void loop() {
  g_display.poll();
  tick();
}
