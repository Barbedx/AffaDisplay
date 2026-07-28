// 11_mediascreen — three rows, three clocks.
//
//     [----] 0:13              progress bar + elapsed. Ticks once a second, never scrolls.
//     solar mpanels            drifts 1 s per symbol
//     solar escimo forever     drifts 2 s per symbol — half the speed, on purpose
//
// THE THREE ROWS ARE INDEPENDENT AND THAT IS THE WHOLE POINT. Each text row owns its own
// widget::Marquee with its own geometry and its own step rate, so row 1 has moved twice by
// the time row 2 has moved once and the two drift apart continuously. One shared scroll
// position cannot express that, which is exactly why the geometry is injected per instance
// rather than baked into the panel.
//
// It also runs on a CARMINAT. The marquee used to be welded into UpdateListDisplay and
// hard-coded to that panel's eight cells; nothing about scrolling text was ever
// AFFA2-specific, and here it drives a panel that never had one — at 24 cells and two
// different rates at once.
//
// WHAT IS NOT HERE, DELIBERATELY: where the track came from. No Bluetooth, no AVRCP, no
// media router. The application hands this screen two strings and a position in seconds.
// Everything above that line is policy and belongs to the application.

#include <Arduino.h>
#include <AffaDisplay.h>
#include <cstdio>
#include <cstring>

#if !AFFA_PANEL_CARMINAT || !AFFA_ENABLE_MARQUEE
#  error "11_mediascreen needs AFFA_PANEL_CARMINAT=1 and AFFA_ENABLE_MARQUEE=1"
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
// The three rows
// ---------------------------------------------------------------------------
// The Carminat window row is 26 usable bytes (AffaConfig.h — the last cells of row1 never
// reach the panel, which stops at the declared FF_DL). Scroll inside 24 and there is margin
// whatever the panel does with the tail.
constexpr uint8_t kCols = 24;
constexpr uint8_t kGap  = 4;      // blank cells before the wrap, so it reads as a pause

constexpr const char* kRow1   = "solar mpanels";
constexpr const char* kRow2   = "solar escimo forever";
constexpr uint32_t    kRow1Ms = 1000;   // 1 s per symbol
constexpr uint32_t    kRow2Ms = 2000;   // 2 s per symbol

constexpr uint16_t kTrackSec = 217;     // 3:37, so the bar visibly fills

affa::widget::Marquee g_r1{ affa::widget::MarqueeGeometry{kCols, kGap, kRow1Ms} };
affa::widget::Marquee g_r2{ affa::widget::MarqueeGeometry{kCols, kGap, kRow2Ms} };

// THE ONE THING A CARMINAT DRAWS WITH THE DISPLAY POWERED OFF IS THE CLOCK. Setting it on
// every fresh sync turns the glass into a link light: if the panel reads 10:00, the
// handshake completed and registration latched — without a console, a serial cable or a
// laptop. Re-armed on sync LOSS, not done once at boot, because the question it answers is
// "is the link up now", not "did it ever come up".
// RE-ARMED ON A FAILED DELIVERY AS WELL AS ON SYNC LOSS, and that half is the one worth
// copying. Clearing the flag when the render is ENQUEUED looks right and is not: a render
// that then completes Timeout — because the panel was not acknowledging yet — is never
// retried, and the clock sits frozen on the glass while the link comes good underneath it.
// An acceptance verdict is not a delivery verdict (docs/API.md §3).
constexpr const char* kLinkClock = "1000";
bool           g_clockPending = true;
affa::TxTicket g_clockTicket  = affa::kNoTicket;

void onSync(affa::SyncState, void*) {
  if (!g_display.synced()) g_clockPending = true;
}

void onComplete(affa::TxTicket t, affa::Result r, void*) {
  if (t == affa::kNoTicket || t != g_clockTicket) return;
  g_clockTicket = affa::kNoTicket;
  if (r != affa::Result::Ok) g_clockPending = true;    // the panel never saw it; try again
}

void clockTick() {
  if (!g_clockPending || !g_display.synced()) return;
  // Lazy registration rides on this: as the first render after a resync it drags the 0x70
  // probes with it, so FUNCSREG latches on the clock rather than on the media screen.
  if (g_display.setTime(kLinkClock) != affa::Result::Ok) return;   // retry next pump
  // Read it IMMEDIATELY: the next enqueue overwrites lastEnqueued().
  g_clockTicket  = g_display.lastEnqueued();
  g_clockPending = false;
}

uint32_t g_startMs = 0;
uint32_t g_nextMs  = 0;
char     g_lastHdr[40] = {0};
char     g_lastR1 [40] = {0};
char     g_lastR2 [40] = {0};

// `[----] 0:13` — four cells of bar, filled by elapsed/total, then m:ss.
void buildHeader(char* out, size_t n, uint16_t elapsed) {
  constexpr uint8_t kBar = 4;
  char bar[kBar + 1];
  const uint8_t filled =
      static_cast<uint8_t>((static_cast<uint32_t>(elapsed) * kBar) / kTrackSec);
  for (uint8_t i = 0; i < kBar; ++i) bar[i] = (i < filled) ? '#' : '-';
  bar[kBar] = '\0';
  snprintf(out, n, "[%s] %u:%02u", bar,
           static_cast<unsigned>(elapsed / 60), static_cast<unsigned>(elapsed % 60));
}

void tick() {
  const uint32_t now = ::millis();
  if (!affa::expired(now, g_nextMs)) return;
  g_nextMs = now + 100;                    // 10 Hz ceiling; both rows are far slower

  // RENDER AT THE RATE THE WIRE SUSTAINS, NOT THE RATE THE TEXT MOVES. showMenu is 96
  // bytes — 13 frames, each waiting on a panel ACK — so a screen takes longer to deliver
  // than a fast marquee takes to step. Enqueueing anyway does not make the panel faster:
  // RenderSlot::Menu coalescing supersedes the queued screen, the completion arrives as
  // Result::Aborted, and the glass LOOKS FROZEN. Measured on the bench at 300 ms steps
  // without this line: 255 of 377 renders superseded, the row unchanged for eight seconds.
  //
  // Skipping a tick costs nothing, because the marquee position is DERIVED from the clock
  // rather than accumulated: we sample it later and get the position the wall clock says,
  // not a backlog. That is the whole reason windowAt() takes `now`.
  if (g_display.busy()) return;

  const uint16_t elapsed =
      static_cast<uint16_t>(((now - g_startMs) / 1000u) % (kTrackSec + 1u));

  char hdr[40], r1[40], r2[40];
  buildHeader(hdr, sizeof(hdr), elapsed);
  g_r1.window(g_r1.windowAt(now), r1, sizeof(r1));
  g_r2.window(g_r2.windowAt(now), r2, sizeof(r2));

  // Change-driven on top of the deadline: an identical screen is 13 frames of bus time
  // that changes nothing on the glass.
  if (strcmp(hdr, g_lastHdr) == 0 && strcmp(r1, g_lastR1) == 0 &&
      strcmp(r2, g_lastR2) == 0)
    return;
  strcpy(g_lastHdr, hdr); strcpy(g_lastR1, r1); strcpy(g_lastR2, r2);

  // kScrollNone: the arrows mean "more list items this way", and this is not a list —
  // showing one would point at nothing.
  const affa::Result res = g_display.showMenu(hdr, r1, r2, affa::carminat::kScrollNone);
  if (res != affa::Result::Ok && res != affa::Result::NoSync)
    Serial.printf("[media] showMenu refused: %d\n", static_cast<int>(res));
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);
  if (!g_link.begin(kPins, 500000)) { Serial.println("CAN did not come up"); return; }

  g_display.onSync(&onSync, nullptr);
  g_display.onComplete(&onComplete, nullptr);
  g_display.begin();

  // The panel renders nothing while the display is powered off, and the symptom is a
  // perfectly healthy link drawing to a dark screen. The clock above is the exception, and
  // that is exactly why it makes a good link indicator.
  (void)g_display.setPower(true);

  const uint32_t now = ::millis();
  g_startMs = now;
  g_r1.setText(kRow1, now);
  g_r2.setText(kRow2, now);
  g_r1.setActive(true, now);
  g_r2.setActive(true, now);

  Serial.println("mediascreen: three rows, three clocks");
}

void loop() {
  g_display.poll();
  clockTick();   // first thing after a sync: put 10:00 on the glass
  tick();
}
