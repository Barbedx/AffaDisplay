// 10_callbacks — the smallest complete statement of the callback model.
//
// Four things, and deliberately nothing else:
//
//   1. bring the display up,
//   2. log every button press                     -> onKey
//   3. log every string another node draws         -> onText
//   4. render an incrementing counter once a second, and say so when the text is "AUX".
//
// There is no menu here, no page stack, no state machine and no application policy of any
// kind. That is the point: this is the whole seam an application actually sits on. Compare
// examples/08_radio_mitm, which builds real behaviour on the same four calls.
//
// WHAT IS AND IS NOT A CALLBACK. onKey and onText are decoded events — the library did the
// protocol work and hands you meaning. They are C function pointers, not std::function: no
// allocation, and a plain pointer can be compared and reset. Every one of them fires from
// inside poll(), on the task that called poll(). Render from them freely; block in them
// never.

#include <Arduino.h>
#include <AffaDisplay.h>
#include <cstring>

// onText needs the inbound reassembler, which is off by default on target — it exists to
// decode somebody else's traffic, and the radio role never needs it.
#if !AFFA_PANEL_CARMINAT || !AFFA_ENABLE_ISOTP_RX
#  error "10_callbacks needs AFFA_PANEL_CARMINAT=1 and AFFA_ENABLE_ISOTP_RX=1"
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
// 2. Every button press
// ---------------------------------------------------------------------------
// Fires for a key decoded off the wire AND for one injected with pressKey(). The edge
// matters: Click and Hold are different gestures on the same physical button, and a
// wheel detent only ever arrives as a Click.
void onKey(affa::Key k, affa::KeyEdge e, void*) {
  Serial.printf("[key ] 0x%04X %s\n", static_cast<unsigned>(k),
                e == affa::KeyEdge::Hold ? "hold" : "click");
}

// ---------------------------------------------------------------------------
// 3. Every string another node draws, and the AUX case
// ---------------------------------------------------------------------------
// `text` POINTS INTO LIBRARY STORAGE and is valid only until this function returns. Copy
// it if you want to keep it — the strstr below reads it in place, which is fine.
//
// This only ever reports what SOMEBODY ELSE transmitted: our own renders come back off an
// echoing link stamped fromSelf and are dropped before the decoder. On a bench with no
// second head unit, nothing arrives here, and that is correct rather than broken.
void onText(const char* text, void*) {
  Serial.printf("[text] \"%s\"\n", text);
  if (strstr(text, "AUX")) Serial.println("[aux ] the source banner says AUX");
}

// ---------------------------------------------------------------------------
// 4. One render per second
// ---------------------------------------------------------------------------
// A DEADLINE, never a counter and never a delay(). Every periodic thing in this library
// compares against the clock, so the behaviour does not change with the loop rate.
uint32_t g_nextMs = 0;
uint32_t g_count  = 0;

void counterTick() {
  if (!affa::expired(::millis(), g_nextMs)) return;
  g_nextMs = ::millis() + 1000;

  char buf[16];
  snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(g_count++));

  // setText carries RenderSlot::Text, so a render queued behind a slower one is REPLACED
  // rather than stacked. That is what keeps the panel showing the newest count instead of
  // working through a backlog of stale ones — see docs/API.md §3b.4.
  //
  // The Result is an acceptance verdict ("was it queued?"), never a delivery verdict.
  // NoSync before the handshake completes is normal and not an error.
  const affa::Result r = g_display.setText(buf, 255);
  if (r != affa::Result::Ok && r != affa::Result::NoSync)
    Serial.printf("[tx  ] setText refused: %d\n", static_cast<int>(r));
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);

  if (!g_link.begin(kPins, 500000)) {
    Serial.println("CAN did not come up");
    return;
  }

  // Callbacks BEFORE begin(): begin() transmits nothing by itself, but it does reset the
  // FSMs, and installing a sink afterwards would miss whatever the first poll() produced.
  g_display.onKey(&onKey, nullptr);
  g_display.onText(&onText, nullptr);
  g_display.begin();

  Serial.println("up: logging keys and inbound text, counting once a second");
}

void loop() {
  g_display.poll();   // the single pump: RX before TX, every call
  counterTick();
}
