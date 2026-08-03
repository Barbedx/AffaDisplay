// 03_hello — the smallest correct way to put SUCCESS on a Carminat panel.
//
// THIS FILE IS THE API, NOT A DEPLOYMENT. It has no WiFi and no OTA, so it can only be
// flashed over a cable. For a board you can only reach over the network — the bench rig —
// flash examples/01_bringup instead: same sequence, plus WiFi, ElegantOTA and a console.
//
// WHAT IT DEMONSTRATES, AND THE ONE THING PEOPLE GET WRONG:
//
// Every render call ENQUEUES and returns immediately. The Result it gives you is an
// ACCEPTANCE verdict — "did this go into the queue" — never a delivery verdict. Delivery
// arrives later, through onComplete.
//
// So this does NOT work:
//
//     display.setPower(true);
//     display.setText("SUCCESS");     // <- on the wire microseconds later
//     display.setTime("1000");
//
// The text reaches the panel while the glass is still coming up, so nothing appears, and
// the usual conclusion is "setText does not work". Worse, two setText calls in a row
// COALESCE — they share RenderSlot::Text, so the second silently replaces the first.
//
// The correct shape is one render in flight at a time, each step waiting for the panel's
// verdict, with a settling delay after power-on because "the glass is lit" is not something
// the panel tells you:
//
//     power on -> wait for its ACK -> wait 750 ms -> "SUCCESS" -> wait for ACK -> 10:00
//
// Build (needs a cable for the upload):
//   pio run -e ex03_hello -t upload

#include <Arduino.h>
#include <AffaDisplay.h>

#if !AFFA_PANEL_CARMINAT
#  error "03_hello is a Carminat example: build with -D AFFA_PANEL_CARMINAT=1"
#endif
#if !AFFA_ENABLE_TASK
#  error "03_hello uses the library-owned poll task: build with -D AFFA_ENABLE_TASK=1"
#endif

namespace {

// ESP32-C3 SuperMini + transceiver. RX FIRST — the named fields are what stop the swap
// from becoming a silent bus with no error anywhere.
constexpr affa::CanPins kPins{ .rx = GPIO_NUM_3, .tx = GPIO_NUM_4 };
constexpr uint32_t      kBitrate = 500000;

// The panel needs a moment after power-on before anything drawn on it is visible.
constexpr uint32_t kWarmUpMs = 750;

struct ArduinoClock final : affa::IClock {
  uint32_t millis() const override { return ::millis(); }
};

affa::Esp32CanLink    g_link;
ArduinoClock          g_clock;
affa::CarminatDisplay g_display(g_link, g_clock);
affa::rtos::AffaTask  g_task;     // the library's own poll task

enum class Step : uint8_t { WaitLink, Power, WarmUp, Text, Time, Done };

Step     g_step  = Step::WaitLink;
uint32_t g_req   = 0;      // the request we are waiting on
uint32_t g_until = 0;      // deadline for the current wait

// Set by the library on its own task when a render completes; read by loop().
volatile uint32_t g_ackReq = 0;
volatile uint8_t  g_ackRes = 0;
volatile uint32_t g_ackSeq = 0;
uint32_t          g_seen   = 0;

void onDone(affa::rtos::TxRequest req, affa::Result r, void*) {
  g_ackReq = req;
  g_ackRes = static_cast<uint8_t>(r);
  ++g_ackSeq;          // publish last, so a reader that sees the bump sees the values
}

// Did the render we are waiting for finish? 0 = still waiting, 1 = Ok, 2 = failed.
uint8_t ackState() {
  while (g_seen != g_ackSeq) {
    ++g_seen;
    if (g_ackReq == g_req)
      return g_ackRes == static_cast<uint8_t>(affa::Result::Ok) ? 1 : 2;
    // Someone else's completion — the library makes renders of its own. Keep looking.
  }
  return 0;
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\nAffaDisplay 03_hello");

  if (!g_link.begin(kPins, kBitrate))
    Serial.println("[can] controller did not come up");

  // THE ORDER IS THE CONTRACT: callbacks, then begin(), then start(). start() refuses a
  // display that was never begun, and a callback installed after the task is running would
  // miss whatever it had already delivered.
  g_task.onComplete(&onDone, nullptr);
  g_display.begin();

  if (!g_task.start(g_display))
    Serial.println("[task] start() FAILED — nothing will be polled");
}

void loop() {
  // NO poll() HERE, and that is the point: the library polls itself on its own task. loop()
  // may block for as long as it likes without costing a timed-out ACK or a lost render.
  const affa::rtos::Status st = g_task.status();
  const uint32_t now = millis();

  // Losing sync invalidates everything behind us — the panel forgets its registration, and
  // it may have been powered down. Start again rather than carrying on.
  if (g_step != Step::WaitLink && affa::hasFlag(st.sync, affa::SyncState::Failed)) {
    Serial.println("[seq] sync lost — restarting");
    g_step = Step::WaitLink;
  }

  switch (g_step) {
    case Step::WaitLink:
      // NOT waiting for registration: it is lazy and happens ON the first render, so
      // waiting for it here would wait for ever.
      if (affa::hasFlag(st.sync, affa::SyncState::Failed)) break;
      Serial.println("[seq] panel answering — powering the display on");
      g_req  = g_task.setPower(true);
      g_seen = g_ackSeq;
      g_step = Step::Power;
      break;

    case Step::Power:
      if (ackState() != 1) break;
      Serial.printf("[seq] power acknowledged — warming up %lu ms\n",
                    static_cast<unsigned long>(kWarmUpMs));
      g_until = now + kWarmUpMs;
      g_step  = Step::WarmUp;
      break;

    case Step::WarmUp:
      // The one hard-coded delay in the file, and it is here because the panel does not
      // announce that its glass is lit.
      if (!affa::expired(now, g_until)) break;
      g_req  = g_task.setText("SUCCESS");
      g_seen = g_ackSeq;
      g_step = Step::Text;
      break;

    case Step::Text:
      if (ackState() != 1) break;
      Serial.println("[seq] \"SUCCESS\" delivered — setting the clock");
      g_req  = g_task.setTime("1000");        // HHMM, four ASCII digits: 10:00
      g_seen = g_ackSeq;
      g_step = Step::Time;
      break;

    case Step::Time:
      if (ackState() != 1) break;
      Serial.println("[seq] clock set to 10:00 — done");
      g_step = Step::Done;
      break;

    case Step::Done:
      break;
  }

  // loopTask runs at priority 1 and IDLE at 0, so a loop() that never yields starves IDLE
  // and the single-core C3 panics with "Task watchdog got triggered (IDLE)".
  delay(10);
}
