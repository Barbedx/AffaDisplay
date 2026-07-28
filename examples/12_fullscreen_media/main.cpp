// 12_fullscreen_media — examples/11_mediascreen on the FULLSCREEN screen instead.
//
//     [----] 0:13              progress bar + elapsed, ticks once a second
//     solar mpanels            drifts 1 s per symbol
//     solar escimo forever     drifts 2 s per symbol
//
// Identical content and identical drift to 11_mediascreen. The ONE difference is the panel
// primitive underneath, and the point of having both is that the difference is visible:
//
//   11_mediascreen   showMenu(header, row0, row1, scroll)   0x21 mode 0x01, the windowed
//                                                           two-row menu with a header
//   12 (this one)    showFullscreenText(l1, l2, l3)         0x21 mode 0x05, the whole glass
//
// WHY THAT CHANGES THE LAYOUT. showMenu has a header and two ROWS, and the rows carry row
// tags (0x7E / 0x7F) that a highlight can address — it is a list. Fullscreen has three
// EQUAL lines separated by 0x0D and no tags at all, so nothing can be highlighted and there
// is no "selected" line. If you want a list, use 11; if you want three lines of text, this.
//
// THE WIDTHS ARE NOT THE SAME EITHER, and that is why the geometry is a constructor
// argument. showMenu gives each row its own fixed field (26 usable bytes). Fullscreen is a
// single flowing block — content[34..95], 62 bytes for all three lines PLUS their
// separators — so the lines share a budget rather than owning one. 18 cells each leaves
// room for the separators and the two leading spaces the OEM capture starts with.
//
// IT DOES NEED CLOSING, AND THIS IS THE TRAP. Confirmed on a real Carminat 2026-07-28:
//
//   * BETWEEN FRAMES, no. Each fullscreen screen replaces the last, so the animation below
//     runs with no close anywhere — closing and reopening would blank the glass every step.
//   * TO GET BACK OUT, YES. With a fullscreen up, setText() was delivered Ok — the panel
//     acknowledged every frame — and the glass did not change. The fullscreen owns the
//     display until hideFullscreenText() takes it away.
//
// So a delivered Ok is NOT evidence that anything appeared. This is the same shape as the
// display-power trap (a render to a powered-off panel also completes Ok and shows nothing):
// the transport succeeded and the screen still did not change. Call hideFullscreenText()
// before you go back to setText() or showMenu().

#include <Arduino.h>
#include <AffaDisplay.h>
#include <cstdio>
#include <cstring>

#if !AFFA_PANEL_CARMINAT || !AFFA_ENABLE_MARQUEE || !AFFA_ENABLE_FULLSCREEN
#  error "12_fullscreen_media needs AFFA_PANEL_CARMINAT=1, AFFA_ENABLE_MARQUEE=1, AFFA_ENABLE_FULLSCREEN=1"
#endif

namespace {

struct ArduinoClock final : affa::IClock {
  uint32_t millis() const override { return ::millis(); }
};

constexpr affa::CanPins kPins{ .rx = GPIO_NUM_4, .tx = GPIO_NUM_3 };

affa::Esp32CanLink    g_link;
ArduinoClock          g_clock;
affa::CarminatDisplay g_display(g_link, g_clock);

// 62 content bytes for three lines and their separators, so ~18 each with margin. Narrower
// than 11_mediascreen's 24 for exactly that reason — the same text, a different budget.
constexpr uint8_t kCols = 18;
constexpr uint8_t kGap  = 4;

constexpr const char* kLine2  = "solar mpanels";
constexpr const char* kLine3  = "solar escimo forever";
constexpr uint32_t    kMs2    = 1000;   // 1 s per symbol
constexpr uint32_t    kMs3    = 2000;   // 2 s per symbol
constexpr uint16_t    kTrackSec = 217;  // 3:37

affa::widget::Marquee g_l2{ affa::widget::MarqueeGeometry{kCols, kGap, kMs2} };
affa::widget::Marquee g_l3{ affa::widget::MarqueeGeometry{kCols, kGap, kMs3} };

// The link clock — see 11_mediascreen for why it is re-armed on a failed DELIVERY and not
// merely on a successful enqueue. The clock is the one thing the panel draws with the
// display powered off, so it doubles as a link light.
constexpr const char* kLinkClock = "1000";
bool           g_clockPending = true;
affa::TxTicket g_clockTicket  = affa::kNoTicket;

uint32_t g_startMs = 0;
uint32_t g_nextMs  = 0;
char     g_last1[40] = {0};
char     g_last2[40] = {0};
char     g_last3[40] = {0};

void onSync(affa::SyncState, void*) {
  if (!g_display.synced()) g_clockPending = true;
}

void onComplete(affa::TxTicket t, affa::Result r, void*) {
  if (t == affa::kNoTicket || t != g_clockTicket) return;
  g_clockTicket = affa::kNoTicket;
  if (r != affa::Result::Ok) g_clockPending = true;   // the panel never saw it; try again
}

void clockTick() {
  if (!g_clockPending || !g_display.synced()) return;
  if (g_display.setTime(kLinkClock) != affa::Result::Ok) return;
  g_clockTicket  = g_display.lastEnqueued();          // read it IMMEDIATELY
  g_clockPending = false;
}

void buildLine1(char* out, size_t n, uint16_t elapsed) {
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
  g_nextMs = now + 100;

  // RENDER AT THE RATE THE WIRE SUSTAINS. Fullscreen is 96 bytes — FOURTEEN frames here,
  // one more than showMenu, because the declared 0x60 = 96 is correct and the panel does
  // not stop early. Enqueueing faster than that just supersedes the queued screen through
  // RenderSlot::Fullscreen coalescing and the glass looks frozen. Skipping a tick is free:
  // the marquee position is derived from the clock, not accumulated.
  if (g_display.busy()) return;

  const uint16_t elapsed =
      static_cast<uint16_t>(((now - g_startMs) / 1000u) % (kTrackSec + 1u));

  char l1[40], l2[40], l3[40];
  buildLine1(l1, sizeof(l1), elapsed);
  g_l2.window(g_l2.windowAt(now), l2, sizeof(l2));
  g_l3.window(g_l3.windowAt(now), l3, sizeof(l3));

  if (strcmp(l1, g_last1) == 0 && strcmp(l2, g_last2) == 0 && strcmp(l3, g_last3) == 0)
    return;                                   // nothing moved; 14 frames saved
  strcpy(g_last1, l1); strcpy(g_last2, l2); strcpy(g_last3, l3);

  // NO hideFullscreenText() between frames. Each screen replaces the last; closing and
  // reopening would blank the glass every step.
  const affa::Result res = g_display.showFullscreenText(l1, l2, l3);
  if (res != affa::Result::Ok && res != affa::Result::NoSync)
    Serial.printf("[full] showFullscreenText refused: %d\n", static_cast<int>(res));
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);
  if (!g_link.begin(kPins, 500000)) { Serial.println("CAN did not come up"); return; }

  g_display.onSync(&onSync, nullptr);
  g_display.onComplete(&onComplete, nullptr);
  g_display.begin();
  (void)g_display.setPower(true);

  const uint32_t now = ::millis();
  g_startMs = now;
  g_l2.setText(kLine2, now);
  g_l3.setText(kLine3, now);
  g_l2.setActive(true, now);
  g_l3.setActive(true, now);

  Serial.println("fullscreen media: three lines, three clocks");
}

void loop() {
  g_display.poll();
  clockTick();
  tick();
}
