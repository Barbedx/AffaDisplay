// 13_updatelist_text — a running text row on the UpdateList LCD.
//
// The whole example is four calls: begin(), setPower(true), setScrollText(), and poll().
// Everything else — the handshake, the lazy registration, the ISO-TP framing, the scroll
// cadence — is the library's, and none of it is visible here. That is the point.
//
// DIFFERENT PANEL FAMILY FROM THE CARMINAT EXAMPLES. UpdateList is AFFA2: sync on 0x3DF
// (we transmit) answered on 0x3CF, data on 0x121 and 0x1B1, keys in on 0x0A9, and the
// packet filler is 0x81 rather than 0x00. docs/WIRE-SPEC.md §9.
//
// THE LCD VARIANT, NOT THE 8-SEGMENT ONE. UpdateListMenuDisplay overrides exactly one
// thing — setText emits the `10 1C 7F ..` text-plus-icons form (§9.2) where the segment
// panel emits `10 19 76 ..` (§9.1) — and widens the marquee to the LCD's 12-cell field.
// For the 8-segment panel, swap the class for UpdateListDisplay and change nothing else.
//
// CORROBORATION. Every byte this example puts on the wire was cross-checked against an
// independent reverse-engineering of the same display (mroczny, "Smart car radio",
// hackaday.io/project/27439), which matches our spec on the sync bytes (`7A 01 81..`,
// `79 00 81..`), the hello (`70 1A 11 00 00 00 00 01`), the 0x70 registration probes to
// both function ids, the enable command (`04 52 02 FF FF`) and the LCD text header
// (`10 1C 7F ..`) with its 0x10 separator before the shown text.

#include <Arduino.h>
#include <AffaDisplay.h>

#if !AFFA_PANEL_UPDATELIST_MENU || !AFFA_ENABLE_MARQUEE
#  error "13_updatelist_text needs AFFA_PANEL_UPDATELIST_MENU=1 and AFFA_ENABLE_MARQUEE=1"
#endif

namespace {

struct ArduinoClock final : affa::IClock {
  uint32_t millis() const override { return ::millis(); }
};

constexpr affa::CanPins kPins{ .rx = GPIO_NUM_4, .tx = GPIO_NUM_3 };

affa::Esp32CanLink          g_link;
ArduinoClock                g_clock;
affa::UpdateListMenuDisplay g_display(g_link, g_clock);

constexpr const char* kText = "AFFA2 UPDATE LIST - RUNNING TEXT DEMO";

// The link clock, same idea as the Carminat examples: re-armed on sync loss AND on a
// failed DELIVERY, because setTime() returns an acceptance verdict and a message that
// completes Timeout was never seen by the panel (docs/API.md §3).
constexpr const char* kLinkClock = "1000";
bool           g_clockPending = true;
affa::TxTicket g_clockTicket  = affa::kNoTicket;

void onSync(affa::SyncState, void*) {
  if (!g_display.synced()) g_clockPending = true;
}

void onComplete(affa::TxTicket t, affa::Result r, void*) {
  if (t == affa::kNoTicket || t != g_clockTicket) return;
  g_clockTicket = affa::kNoTicket;
  if (r != affa::Result::Ok) g_clockPending = true;
}

void clockTick() {
  if (!g_clockPending || !g_display.synced()) return;
  if (g_display.setTime(kLinkClock) != affa::Result::Ok) return;
  g_clockTicket  = g_display.lastEnqueued();     // read it IMMEDIATELY
  g_clockPending = false;
}

void onKey(affa::Key k, affa::KeyEdge e, void*) {
  Serial.printf("[key ] 0x%04X %s\n", static_cast<unsigned>(k),
                e == affa::KeyEdge::Hold ? "hold" : "click");
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);
  if (!g_link.begin(kPins, 500000)) { Serial.println("CAN did not come up"); return; }

  g_display.onKey(&onKey, nullptr);
  g_display.onSync(&onSync, nullptr);
  g_display.onComplete(&onComplete, nullptr);
  g_display.begin();

  // MANDATORY, AND THE MOST COMMON REASON A HEALTHY LINK DRAWS NOTHING. This is the
  // `0x1B1 04 52 02 FF FF` enable (§9.3) — without it the panel acknowledges every frame
  // and renders none of them. Registration rides along: it is the first render after a
  // resync, so it drags the two 0x70 probes to 0x121 and 0x1B1 with it.
  (void)g_display.setPower(true);

  // The whole marquee API: a string and a play bit. The library owns the window, the
  // cadence and the re-render; the application owns the text.
  //
  // Re-setting the SAME text is a deliberate no-op that does NOT reset the position, so a
  // media app republishing the current track every second does not pin it to character 0.
  g_display.setScrollText(kText);
  g_display.setScrollActive(true);

  Serial.println("updatelist LCD: running text");
}

void loop() {
  // poll() is also the KEEP-ALIVE. UpdateList drops the link if the 0x3DF heartbeat stops,
  // so a loop that blocks does not merely delay a render — it disconnects the display.
  g_display.poll();
  clockTick();
}
