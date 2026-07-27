// 06_counter_preempt — why "the buttons are laggy" is usually not a key-handling bug.
//
// A counter runs 1..1000 and is re-rendered at 10 Hz, deliberately faster than a full
// render round-trip: each setText is 3 frames, each frame waits for the panel's ACK, and
// the panel is not in a hurry. Press Pause on the remote and the counter must stop
// INSTANTLY.
//
// THE FAILURE MODE THIS DEMONSTRATES. The key is received promptly in ANY design — one
// poll(), bounded, because poll() drains RX before it pumps the transmit FSM. What goes
// wrong without coalescing and preemption is everything ALREADY QUEUED: at 10 Hz in front
// of a ~300 ms transfer there are several stale counter values waiting, and the panel
// visibly keeps counting for a second after the user pressed Pause and after the library
// correctly delivered the key. It reads as key latency and it is a queueing bug. The
// numbers this example prints separate the two beyond argument:
//
//   key->cb   micros from the key FRAME arriving to the application callback running
//   cb->wire  micros from the callback to the first byte of OUR reply on the link
//   dropped   how many stale counter renders abortPending() threw away
//
// `dropped` is the whole story. With AFFA_TX_COALESCE=1 (the default) it stays small,
// because a re-render supersedes a queued-not-started render of the same RenderSlot
// instead of stacking behind it. Rebuild with -D AFFA_TX_COALESCE=0 and watch it climb.

#include <Arduino.h>
#include <AffaDisplay.h>

#if !AFFA_PANEL_CARMINAT
#  error "06_counter_preempt needs -D AFFA_PANEL_CARMINAT=1"
#endif

namespace {

struct ArduinoClock final : affa::IClock {
  uint32_t millis() const override { return ::millis(); }
};

constexpr affa::CanPins kPins{ .rx = GPIO_NUM_4, .tx = GPIO_NUM_3 };
constexpr uint32_t kMinPeriodMs = 20;
constexpr uint32_t kMaxPeriodMs = 1000;

affa::Esp32CanLink    g_link;
ArduinoClock          g_clock;
affa::CarminatDisplay g_display(g_link, g_clock);

uint32_t g_periodMs   = 100;      // 10 Hz
uint32_t g_nextTickMs = 0;
uint16_t g_counter    = 0;
bool     g_paused     = false;

volatile uint32_t g_keyFrameUs = 0;   // when the key FRAME was seen at Layer 0
uint32_t g_cbUs        = 0;           // when our callback ran
bool     g_awaitingTx  = false;       // measuring cb -> first byte on the link
uint32_t g_lastDropped = 0;

// Layer 0 sees every frame in both directions, in wire order. It is the earliest
// application-visible timestamp there is — the frame has been drained from the RX ring and
// nothing has interpreted it yet.
void onFrameTap(const affa::Frame& f, affa::Direction d, void*) {
  if (d == affa::Direction::Rx) {
    if (f.id == 0x1C1 && f.len >= 2 && f.data[0] == 0x03 && f.data[1] == 0x89)
      g_keyFrameUs = micros();
    return;
  }
  // Our reply. Match the TEXT id, not "the next TX frame": the 1 Hz sync heartbeat on
  // 0x3AF would otherwise be timed instead, and it would flatter the number.
  if (g_awaitingTx && f.id == 0x151) {
    Serial.printf("       cb->wire %lu us   (dropped %lu stale renders)\n",
                  static_cast<unsigned long>(micros() - g_cbUs),
                  static_cast<unsigned long>(g_lastDropped));
    g_awaitingTx = false;
  }
}

void render(const char* what) {
  char buf[16];
  if (what) snprintf(buf, sizeof(buf), "%s", what);
  else      snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(g_counter));
  (void)g_display.setText(buf);
}

void onKey(affa::Key k, affa::KeyEdge e, void*) {
  g_cbUs = micros();
  const uint32_t keyToCb = g_cbUs - g_keyFrameUs;

  switch (k) {
    case affa::Key::Pause:
      g_paused = !g_paused;
      // PREEMPTION, in one call. Every job that has not had a single byte handed to the
      // link is dropped and completes Result::Aborted. The message ON THE WIRE is not
      // touched — it is never split — and pending registration jobs survive, because a
      // payload that reaches the panel before its function is registered is rejected and
      // the resulting SendFailed looks exactly like a wire-format bug.
      g_lastDropped = g_display.abortPending();
      render(g_paused ? "PAUSED" : nullptr);
      break;

    case affa::Key::RollUp:      // faster
      g_periodMs = (g_periodMs > kMinPeriodMs * 2) ? g_periodMs / 2 : kMinPeriodMs;
      g_lastDropped = g_display.abortPending();
      render(nullptr);           // render HERE, from the callback: measuring cb->wire
      break;                     // against the next scheduled tick would time the tick

    case affa::Key::RollDown:    // slower
      g_periodMs = (g_periodMs * 2 < kMaxPeriodMs) ? g_periodMs * 2 : kMaxPeriodMs;
      g_lastDropped = g_display.abortPending();
      render(nullptr);
      break;

    default:
      return;
  }

  g_awaitingTx = true;
  Serial.printf("[key ] 0x%04X %s  key->cb %lu us  period %lu ms  %s\n",
                static_cast<unsigned>(k), e == affa::KeyEdge::Hold ? "hold" : "click",
                static_cast<unsigned long>(keyToCb),
                static_cast<unsigned long>(g_periodMs), g_paused ? "PAUSED" : "running");
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);
  if (!g_link.begin(kPins, 500000)) { Serial.println("CAN did not come up"); return; }

  g_display.onFrame(&onFrameTap, nullptr);
  g_display.onKey(&onKey, nullptr);
  g_display.begin();
  (void)g_display.setText("COUNTER");
  Serial.printf("coalescing=%d queue depth=%d\n", AFFA_TX_COALESCE, AFFA_TX_QUEUE_DEPTH);
}

void loop() {
  g_display.poll();      // RX and keys FIRST, then the transmit FSM. Never the other way.

  if (g_paused) return;
  const uint32_t now = ::millis();
  if (!affa::expired(now, g_nextTickMs)) return;
  g_nextTickMs = now + g_periodMs;        // a deadline; a stalled loop must not burst

  if (++g_counter > 1000) g_counter = 1;
  render(nullptr);
}
