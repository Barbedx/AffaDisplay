// 02_canspy — is the ESP32 seeing CAN frames at all? Nothing else. No AffaDisplay.
//
// THIS EXAMPLE DELIBERATELY DOES NOT LINK THE LIBRARY. platformio.ini strips src/ out of
// the build (`build_src_filter = -<*>`), so what runs here is the Arduino core, esp32_can,
// and this file. If frames appear here, the wire and the transceiver are doing their job
// and anything that still fails is ours. If they do not appear here, the answer is in the
// driver's configuration — the bitrate first of all — and not in the protocol layer.
//
// WHAT IT ANSWERS, IN ORDER:
//   1. ARE WE HEARING?  /api/frames is every frame the controller hands us, unfiltered.
//      Extended ids and RTR frames included — AffaDisplay's own trampoline discards both
//      before they are ever counted, so a bus carrying them looks silent from inside the
//      library and busy from in here.
//   2. AT WHAT SPEED?   /api/autospeed runs esp32_can's own beginAutoSpeed(), which puts
//      the controller in TRUE listen-only (it calls _init() first, so the queues exist —
//      this is the safe path AffaDisplay refuses, and the refusal is about the OTHER entry
//      point) and sweeps 1M, 500k, 250k, 125k, 800k, 100k, 50k, 25k, 80k, 33k3, 20k,
//      returning the first rate at which real traffic decodes. A wrong bitrate fails in
//      BOTH directions and looks exactly like a dead bus.
//   3. ARE WE ANSWERING? Two independent proofs. Receiving a valid frame at all means the
//      controller acknowledged it — the ACK is generated in hardware, we cannot opt out.
//      And /api/send transmits a frame: if txErr stays 0 afterwards, somebody out there
//      acknowledged US.
//
// WiFi and ElegantOTA come up before the controller is touched, and the server is
// configured with lru_purge_enable so a full socket table can never lock OTA out.
//
//   pio run -e ex02_canspy                build
//   pio run -e ex02_canspy -t upload      first flash, over USB
//   thereafter: http://<ip>/update        ElegantOTA

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <PsychicHttp.h>
#include <ElegantOTA.h>

#include <esp32_can.h>
#include <driver/twai.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

// ESP32-C3 SuperMini on the bench rig: rx = GPIO4, tx = GPIO3. Swapping these produces a
// silent bus with no error anywhere, so they are named, not positional.
#ifndef CANSPY_PINS_MIRRORED
#  define CANSPY_PINS_MIRRORED 0
#endif
#if CANSPY_PINS_MIRRORED
constexpr gpio_num_t kRxPin = GPIO_NUM_3;
constexpr gpio_num_t kTxPin = GPIO_NUM_4;
#else
constexpr gpio_num_t kRxPin = GPIO_NUM_4;
constexpr gpio_num_t kTxPin = GPIO_NUM_3;
#endif

constexpr const char* kWifiNamespace = "megaopen";   // read-only: ssid / pass
constexpr const char* kOwnNamespace  = "canspy";     // ours: the bitrate, and only that
constexpr const char* kApSsid        = "CanSpy";
constexpr const char* kApPass        = "canspy123";
constexpr const char* kMdnsName      = "canspy";
constexpr uint32_t    kStaJoinMs     = 15000;
constexpr uint32_t    kDefaultRate   = 500000;

PsychicHttpServer g_server;

// WHAT IS ACTUALLY RUNNING RIGHT NOW, or nullptr.
//
// Every test here is requested by an HTTP handler and performed in loop(), and loop() clears
// the request flag BEFORE it starts work. So a "busy" field derived from the request flag
// reads false for the entire duration of the test — which is fine for a 6 s test and useless
// for the rate sweep, which can take ten minutes and would report itself finished on the very
// first poll. Anything watching the board needs a flag that spans the WORK, not the request.
volatile const char* g_running = nullptr;

struct RunningScope {
  explicit RunningScope(const char* what) { g_running = what; }
  ~RunningScope() { g_running = nullptr; }
};

uint32_t g_rate       = kDefaultRate;
bool     g_canUp      = false;
bool     g_listen     = false;    // controller in TRUE listen-only: no ACK, no error frames
uint32_t g_rebootAt   = 0;
bool     g_otaRunning = false;

// -1 = nothing requested, 0/1 = switch the controller into that mode from loop().
// setListenOnlyMode() is disable()+enable(), which is only safe once _init() has created the
// queues — i.e. after begin(). It must not run on the web server's task.
volatile int8_t g_wantListen = -1;

// Requested by an HTTP handler, performed in loop(): beginAutoSpeed() blocks for up to
// ~7 s and must not be run on the web server's task.
volatile bool g_wantAutoSpeed = false;
uint32_t      g_autoResult    = 0;      // 0 = never run or nothing found
bool          g_autoRan       = false;

// ---------------------------------------------------------------------------
// The frame ring — the entire point of this firmware
// ---------------------------------------------------------------------------
struct FrameRec {
  uint32_t ms;
  uint32_t id;
  uint8_t  dir;      // 0 = RX, 1 = TX (ours)
  uint8_t  len;
  uint8_t  ext;
  uint8_t  rtr;
  uint8_t  d[8];
};
constexpr size_t kRing = 128;
FrameRec     g_ring[kRing];
size_t       g_head = 0;
uint32_t     g_seq  = 0;
portMUX_TYPE g_mux  = portMUX_INITIALIZER_UNLOCKED;

uint32_t g_rxCount = 0;
uint32_t g_txCount = 0;
uint32_t g_lastRxMs = 0;

void push(uint32_t id, const uint8_t* d, uint8_t len, uint8_t ext, uint8_t rtr, bool tx) {
  portENTER_CRITICAL(&g_mux);
  FrameRec& r = g_ring[g_head];
  r.ms  = ::millis();
  r.id  = id;
  r.dir = tx ? 1 : 0;
  r.len = len > 8 ? 8 : len;
  r.ext = ext;
  r.rtr = rtr;
  memset(r.d, 0, 8);
  if (d) memcpy(r.d, d, r.len);
  g_head = (g_head + 1) % kRing;
  ++g_seq;
  if (tx) { ++g_txCount; } else { ++g_rxCount; g_lastRxMs = r.ms; }
  portEXIT_CRITICAL(&g_mux);
}

// RUNS IN task_CAN (priority 15), on the critical path of every frame, behind a 16-entry
// callbackQueue. Copy and return: no logging, no allocation, no blocking, no clock beyond
// millis(). NOTHING IS FILTERED HERE — extended and RTR frames are recorded too, precisely
// because the library's own callback throws them away.
void onCanFrame(CAN_FRAME* f) {
  if (!f) return;
  push(f->id, f->data.uint8, f->length, f->extended ? 1 : 0, f->rtr ? 1 : 0, /*tx=*/false);
}

// ---------------------------------------------------------------------------
// Log ring
// ---------------------------------------------------------------------------
struct LogRec { uint32_t ms; char msg[112]; };
// 24 was too small for its own sake: the rate sweep emits a header, ten result rows, up to
// ten "panel came back" notes and a verdict — around thirty lines — so the ring evicted the
// early rows, and the winning row is as likely to be early as late. A test that overwrites
// its own answer before anyone reads it is worse than no test.
constexpr size_t kLogRing = 64;
LogRec       g_log[kLogRing];
size_t       g_logHead = 0;
portMUX_TYPE g_logMux = portMUX_INITIALIZER_UNLOCKED;

void logmsg(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void logmsg(const char* fmt, ...) {
  char buf[112];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  portENTER_CRITICAL(&g_logMux);
  LogRec& r = g_log[g_logHead];
  r.ms = ::millis();
  snprintf(r.msg, sizeof(r.msg), "%s", buf);
  g_logHead = (g_logHead + 1) % kLogRing;
  portEXIT_CRITICAL(&g_logMux);

  if (Serial) Serial.printf("[%lu] %s\n", static_cast<unsigned long>(r.ms), buf);
}

// ---------------------------------------------------------------------------
// CAN bring-up
// ---------------------------------------------------------------------------
bool canStart(uint32_t rate, bool listen) {
  // Order is load-bearing: pins, then begin(), then the callback, then watchFor() LAST.
  CAN0.setCANPins(kRxPin, kTxPin);
  CAN0.begin(rate);                      // returns the REQUESTED rate even when it installed
                                         // nothing, so it is not a health check — see below

  // AFTER begin(), never before: setListenOnlyMode() is disable()+enable(), and that enable()
  // starts tasks which block on queues only _init() creates. begin() has now run it.
  if (listen) CAN0.setListenOnlyMode(true);

  CAN0.setGeneralCallback(&onCanFrame);
  CAN0.watchFor();                       // no argument = accept everything

  twai_status_info_t st;
  const bool ok = (twai_get_status_info(&st) == ESP_OK) && (st.state == TWAI_STATE_RUNNING);
  logmsg("can %s at %lu bit/s, mode=%s (rx=GPIO%d tx=GPIO%d)",
         ok ? "RUNNING" : "NOT RUNNING", static_cast<unsigned long>(rate),
         listen ? "LISTEN-ONLY" : "normal",
         static_cast<int>(kRxPin), static_cast<int>(kTxPin));
  return ok;
}

// The A/B that matters: same boot, same wire, same bitrate — only our transmitter changes.
void applyListen(bool on) {
  logmsg("switching to %s ...", on ? "LISTEN-ONLY (we emit nothing at all)" : "normal");
  CAN0.setListenOnlyMode(on);
  CAN0.setGeneralCallback(&onCanFrame);   // survives in principle; re-armed to be certain
  CAN0.watchFor();
  g_listen = on;

  Preferences p;
  if (p.begin(kOwnNamespace, false)) { p.putUChar("listen", on ? 1 : 0); p.end(); }

  twai_status_info_t st{};
  twai_get_status_info(&st);
  logmsg("now %s, drv.state=%u", on ? "LISTEN-ONLY" : "normal",
         static_cast<unsigned>(st.state));
}

void runAutoSpeed() {
  logmsg("autospeed: sweeping, up to ~7 s in true listen-only...");
  const uint32_t found = CAN0.beginAutoSpeed();
  g_autoRan    = true;
  g_autoResult = found;

  if (found) {
    logmsg("autospeed: TRAFFIC FOUND at %lu bit/s", static_cast<unsigned long>(found));
  } else {
    logmsg("autospeed: no traffic decoded at ANY standard rate");
  }

  // beginAutoSpeed() reinstalls the driver, so re-arm what we care about either way.
  CAN0.setGeneralCallback(&onCanFrame);
  CAN0.watchFor();
}

// ---------------------------------------------------------------------------
// The transmit self-test — settles "can we put bits on the wire" WITHOUT a second node
// ---------------------------------------------------------------------------
// TWAI_MODE_NO_ACK is the IDF's own self-test mode: the controller transmits and neither
// sends nor requires an acknowledgment, so a frame succeeds with nobody else on the bus.
// Combined with the SELF RECEPTION REQUEST bit, the controller receives its own
// transmission back through the normal receive path.
//
// WHAT A PASS PROVES: the whole loop works — controller -> TX pin -> transceiver CTX ->
// bus -> transceiver CRX -> RX pin -> controller. There is no way to get the frame back
// without every one of those working.
//
// WHAT A FAIL PROVES: nothing left the controller, or nothing came back. It does NOT by
// itself say which, and on a saturated bus it can also mean we simply never won
// arbitration — which is why it retries and reports the attempt count.
volatile bool g_wantSelfTest = false;
bool     g_stRan  = false;
bool     g_stPass = false;
uint8_t  g_stTries = 0;
uint32_t g_stGot   = 0;

// ID 0x001, NOT some arbitrary spare. On CAN the LOWEST id wins arbitration, and the first
// version of this test used 0x7AB — which loses to the panel's 0x3CF on every single bit
// fight. On a bus the panel is saturating, that test can never transmit and its failure says
// nothing at all. 0x001 outranks everything the panel sends.
constexpr uint32_t kSelfTestId = 0x001;

void runSelfTest() {
  const bool wasListen = g_listen;
  logmsg("selftest: entering NO_ACK + self-reception ...");

  // Full driver restart into NO_ACK: error counters back to zero and the controller
  // guaranteed RUNNING, so the test starts from a clean state rather than from whatever
  // error-passive corner the bus had already pushed us into.
  CAN0.setNoACKMode(true);
  CAN0.setGeneralCallback(&onCanFrame);
  CAN0.watchFor();

  g_stRan  = true;
  g_stPass = false;
  g_stTries = 0;
  const uint32_t before = g_rxCount;

  for (uint8_t attempt = 1; attempt <= 5 && !g_stPass; ++attempt) {
    g_stTries = attempt;

    // A previous attempt may have left the controller bus-off or stopped, which is what
    // ESP_ERR_INVALID_STATE reports. Reinstall before spending the attempt, otherwise every
    // try after the first measures nothing but the state the first one left behind.
    twai_status_info_t pre{};
    if (twai_get_status_info(&pre) != ESP_OK || pre.state != TWAI_STATE_RUNNING) {
      logmsg("selftest: controller state %u before try %u - reinstalling",
             static_cast<unsigned>(pre.state), static_cast<unsigned>(attempt));
      CAN0.setNoACKMode(true);
      CAN0.setGeneralCallback(&onCanFrame);
      CAN0.watchFor();
    }

    twai_message_t m{};
    m.identifier       = kSelfTestId;
    m.data_length_code = 8;
    m.self             = 1;               // self reception request
    m.extd             = 0;
    m.rtr              = 0;
    for (int i = 0; i < 8; ++i) m.data[i] = static_cast<uint8_t>(0xA0 + i);

    const esp_err_t tx = twai_transmit(&m, pdMS_TO_TICKS(250));
    const uint32_t t0 = millis();
    while (millis() - t0 < 300) {         // give the loopback time to land in the ring
      if (g_rxCount != before) break;
      delay(5);
    }

    // Did OUR id come back?
    portENTER_CRITICAL(&g_mux);
    const size_t head = g_head;
    for (size_t i = 0; i < kRing; ++i) {
      const FrameRec& r = g_ring[(head + kRing - 1 - i) % kRing];
      if (r.id == kSelfTestId && r.dir == 0) { g_stPass = true; break; }
    }
    portEXIT_CRITICAL(&g_mux);

    logmsg("selftest try %u: twai_transmit=%s, loopback=%s",
           static_cast<unsigned>(attempt), esp_err_to_name(tx),
           g_stPass ? "RECEIVED" : "nothing");
  }

  g_stGot = g_rxCount - before;

  if (g_stPass)
    logmsg("selftest PASS - our own frame came back: TX pin, CTX, bus, CRX, RX pin all work");
  else
    logmsg("selftest FAIL - no loopback after %u tries", static_cast<unsigned>(g_stTries));

  // Restore whatever mode we were in.
  CAN0.setListenOnlyMode(wasListen);
  if (!wasListen) CAN0.setNoACKMode(false);
  CAN0.setGeneralCallback(&onCanFrame);
  CAN0.watchFor();
  g_listen = wasListen;
  logmsg("selftest: restored %s mode", wasListen ? "listen-only" : "normal");
}

// ---------------------------------------------------------------------------
// The INTERNAL loopback — TWAI peripheral only, transceiver not in the path
// ---------------------------------------------------------------------------
// Points the controller's TX and RX at the SAME free GPIO. The pad is driven by the
// peripheral's transmit output and read straight back by its receive input, so the frame
// never leaves the chip. ESP-IDF supports exactly this for self-test.
//
// This is the clean split, and neither outcome is a guess:
//   PASS -> the TWAI peripheral, the timing config, the driver install and the GPIO matrix
//           are all correct. Whatever is wrong is outside the chip.
//   FAIL -> the fault is inside the ESP32 or its configuration, i.e. ours, and no amount of
//           looking at the bus will find it.
//
// GPIO6 by default: free on the C3 SuperMini and not one of the strapping pins (2, 8, 9).
volatile int16_t g_wantLoop = -1;      // -1 idle, else the pin to test
bool     g_lbRan  = false;
bool     g_lbPass = false;
int      g_lbPin  = -1;

void runLoopback(int pin) {
  const gpio_num_t p = static_cast<gpio_num_t>(pin);
  g_lbRan  = true;
  g_lbPass = false;
  g_lbPin  = pin;
  logmsg("loopback: TX and RX both on GPIO%d, transceiver NOT in the path", pin);

  const uint32_t before = g_rxCount;

  CAN0.disable();
  CAN0.setCANPins(/*rx=*/p, /*tx=*/p);
  CAN0.begin(g_rate);
  CAN0.setNoACKMode(true);              // no second node exists, so no ACK may be required
  CAN0.setGeneralCallback(&onCanFrame);
  CAN0.watchFor();

  for (uint8_t attempt = 1; attempt <= 3 && !g_lbPass; ++attempt) {
    twai_status_info_t pre{};
    if (twai_get_status_info(&pre) != ESP_OK || pre.state != TWAI_STATE_RUNNING) {
      CAN0.setNoACKMode(true);
      CAN0.setGeneralCallback(&onCanFrame);
      CAN0.watchFor();
    }

    twai_message_t m{};
    m.identifier       = kSelfTestId;
    m.data_length_code = 8;
    m.self             = 1;
    for (int i = 0; i < 8; ++i) m.data[i] = static_cast<uint8_t>(0x50 + i);

    const esp_err_t tx = twai_transmit(&m, pdMS_TO_TICKS(250));
    const uint32_t t0 = millis();
    while (millis() - t0 < 300 && g_rxCount == before) delay(5);

    portENTER_CRITICAL(&g_mux);
    const size_t head = g_head;
    for (size_t i = 0; i < kRing; ++i) {
      const FrameRec& r = g_ring[(head + kRing - 1 - i) % kRing];
      if (r.id == kSelfTestId && r.dir == 0) { g_lbPass = true; break; }
    }
    portEXIT_CRITICAL(&g_mux);

    logmsg("loopback try %u: twai_transmit=%s, got=%s", static_cast<unsigned>(attempt),
           esp_err_to_name(tx), g_lbPass ? "OUR FRAME BACK" : "nothing");
  }

  logmsg(g_lbPass ? "loopback PASS - the TWAI peripheral and its config are good"
                  : "loopback FAIL - the fault is inside the ESP32 or its configuration");

  // Back to the real pins and the real mode.
  CAN0.disable();
  CAN0.setCANPins(kRxPin, kTxPin);
  CAN0.begin(g_rate);
  if (g_listen) CAN0.setListenOnlyMode(true);
  CAN0.setGeneralCallback(&onCanFrame);
  CAN0.watchFor();
  logmsg("loopback: restored rx=GPIO%d tx=GPIO%d, %s",
         static_cast<int>(kRxPin), static_cast<int>(kTxPin),
         g_listen ? "listen-only" : "normal");
}

// ---------------------------------------------------------------------------
// The pin-level transmit test — drive CTX by hand, watch CRX
// ---------------------------------------------------------------------------
// TWAI released, GPIO3 driven directly, GPIO4 sampled. Hold CTX recessive (HIGH) and CRX
// should show the panel's traffic — a mixture of highs and lows. Hold CTX dominant (LOW)
// and, if our transmit path reaches the bus at all, we are jamming it: CRX must read solidly
// LOW. It is the one thing every other test in this file leaves unproven.
//
// THE TRAP THIS AVOIDS, WHICH MADE THE OLD MegaOpen /api/can/selftest REPORT NONSENSE:
// twai_driver_uninstall() does NOT hand the pads back to the GPIO matrix. Without the
// gpio_reset_pin() calls below, digitalWrite() writes into a pad still owned by the TWAI
// peripheral, the level never changes, and the test reports a broken transmit path on a
// perfectly good board. That firmware bug is why "the TX path is broken" was believed once
// before — do not remove those two lines.
//
// It jams the bus for a few milliseconds. That is the point, and it is over before anything
// upstream notices.
volatile bool g_wantPinTest = false;
bool     g_ptRan = false;
int      g_ptHighPct = -1;      // % of CRX samples reading 1 while CTX held RECESSIVE
int      g_ptLowPct  = -1;      // ...and while CTX held DOMINANT. This is the verdict.

int samplePct(gpio_num_t pin, uint32_t samples, uint32_t gapUs) {
  uint32_t ones = 0;
  for (uint32_t i = 0; i < samples; ++i) {
    if (digitalRead(pin)) ++ones;
    if (gapUs) delayMicroseconds(gapUs);
  }
  return static_cast<int>((ones * 100) / samples);
}

// THE DOMINANT PULSE MUST BE SHORT, AND THE FIRST VERSION OF THIS TEST GOT IT WRONG.
// Holding TXD low for 10 ms trips the transceiver's TXD DOMINANT TIMEOUT — a protection
// feature that disables the driver so one stuck node cannot wedge the bus. The bus then goes
// idle and CRX reads HIGHER than before, which looks like "our transmit does nothing" when it
// actually means "our transmit worked, and the transceiver defended itself against it".
// Typical timeouts start around 300 us, so stay well inside that: 40 samples, 2 us apart.
constexpr uint32_t kDomSamples = 40;
constexpr uint32_t kDomGapUs   = 2;

void runPinTest() {
  g_ptRan = true;
  logmsg("pintest: releasing TWAI and driving GPIO%d by hand", static_cast<int>(kTxPin));

  CAN0.disable();

  // THE TWO LINES THE OLD TEST WAS MISSING. Without them the pads stay owned by TWAI.
  gpio_reset_pin(kTxPin);
  gpio_reset_pin(kRxPin);

  // INPUT_OUTPUT, not OUTPUT: it keeps the input buffer enabled so we can read the pin BACK
  // and prove it actually reached the level we asked for. Without this readback the whole
  // test is worthless — a pad still owned by TWAI ignores digitalWrite() silently, which is
  // exactly how the old MegaOpen selftest concluded "txPath: broken" on a good board.
  gpio_set_direction(kTxPin, GPIO_MODE_INPUT_OUTPUT);
  pinMode(kRxPin, INPUT);

  digitalWrite(kTxPin, HIGH);            // recessive: the bus belongs to whoever else talks
  delay(2);
  const int ctxHighReadback = digitalRead(kTxPin);
  g_ptHighPct = samplePct(kRxPin, 400, 20);

  // Ten short jabs rather than one long hold, and the WORST (lowest) reading wins: if even
  // one 80 us pulse pulls the bus down, our transmit path reaches it.
  int best = 100;
  int ctxLowReadback = -1;
  for (int rep = 0; rep < 10; ++rep) {
    digitalWrite(kTxPin, LOW);
    delayMicroseconds(5);
    if (ctxLowReadback < 0) ctxLowReadback = digitalRead(kTxPin);
    const int pct = samplePct(kRxPin, kDomSamples, kDomGapUs);
    digitalWrite(kTxPin, HIGH);          // release well inside the dominant timeout
    if (pct < best) best = pct;
    delay(2);                            // let the transceiver and the bus settle
  }
  g_ptLowPct = best;

  logmsg("pintest: CTX readback  -> asked HIGH read %d, asked LOW read %d",
         ctxHighReadback, ctxLowReadback);
  logmsg("pintest: CTX recessive -> CRX high %d%% of samples", g_ptHighPct);
  logmsg("pintest: CTX DOMINANT  -> CRX high %d%% of samples", g_ptLowPct);

  if (ctxHighReadback != 1 || ctxLowReadback != 0) {
    logmsg("pintest INVALID - GPIO%d did not follow digitalWrite (pad not under GPIO "
           "control, or something external is holding it). Verdict below means nothing.",
           static_cast<int>(kTxPin));
  } else if (g_ptLowPct <= 5) {
    logmsg("pintest PASS - driving CTX low pulls the bus down: our transmit path reaches it");
  } else {
    logmsg("pintest FAIL - GPIO%d verified LOW, yet CRX still reads high %d%%: the level is "
           "leaving the ESP32 and not coming back round through the transceiver",
           static_cast<int>(kTxPin), g_ptLowPct);
  }

  // Hand the pads back to TWAI and rebuild the link.
  gpio_reset_pin(kTxPin);
  gpio_reset_pin(kRxPin);
  CAN0.setCANPins(kRxPin, kTxPin);
  CAN0.begin(g_rate);
  if (g_listen) CAN0.setListenOnlyMode(true);
  CAN0.setGeneralCallback(&onCanFrame);
  CAN0.watchFor();
  logmsg("pintest: TWAI restored, %s", g_listen ? "listen-only" : "normal");
}

// ---------------------------------------------------------------------------
// The bit-timing sweep — the experiment the listen-only/normal split points at
// ---------------------------------------------------------------------------
// Listen-only receives 12 000 frames with rxErr 0 and busErr 0. Normal receives NOTHING and
// storms. The controller's only extra behaviour in normal mode is that it drives dominant
// bits: the ACK, and error flags. So our ACK is landing at the wrong moment and destroying
// the very frame it is acknowledging — and everything downstream follows from that.
//
// Where the ACK lands is the SAMPLE POINT, which is bit timing. This sweeps it directly,
// bypassing esp32_can and driving twai_* itself, counting frames actually received in NORMAL
// mode under each configuration. If any row receives, that row is the fix.
//
// The constraint: brp * (1 + tseg_1 + tseg_2) = 80 MHz / 500 kbit/s = 160. And on the C3
// tseg_1 is capped at 16, tseg_2 at 8, sjw at 4 — a tseg_1 of 17 is silently invalid, which
// is why the obvious "push the sample point to 90%" row is 16/3 and not 17/2.
struct TimingCase {
  const char* name;
  uint32_t brp;
  uint8_t  t1, t2, sjw;
  bool     triple;
};
const TimingCase kTimings[] = {
  {"20tq SP80 (default)", 8,  15, 4, 3, false},
  {"20tq SP85",           8,  16, 3, 3, false},
  {"20tq SP70",           8,  13, 6, 4, false},
  {"20tq SP60",           8,  11, 8, 4, false},
  {"20tq SP80 triple",    8,  15, 4, 3, true },
  {"16tq SP81",           10, 12, 3, 2, false},
  {"10tq SP80",           16,  7, 2, 2, false},
  {"8tq  SP75",           20,  5, 2, 2, false},
};
constexpr size_t kTimingCount = sizeof(kTimings) / sizeof(kTimings[0]);

volatile int8_t g_wantNoAck = -1;
volatile bool g_wantSweep = false;
bool     g_swRan = false;
uint32_t g_swGot[kTimingCount] = {0};
int      g_swBest = -1;

void runTimingSweep() {
  g_swRan  = true;
  g_swBest = -1;
  logmsg("timing sweep: %u configs, NORMAL mode, 1.5 s each",
         static_cast<unsigned>(kTimingCount));

  CAN0.disable();                       // hand the peripheral over; this deletes its tasks
  delay(50);

  for (size_t i = 0; i < kTimingCount; ++i) {
    const TimingCase& c = kTimings[i];
    g_swGot[i] = 0;

    twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(kTxPin, kRxPin, TWAI_MODE_NORMAL);
    g.rx_queue_len = 32;
    g.tx_queue_len = 8;
    twai_timing_config_t t{};
    t.brp             = c.brp;
    t.tseg_1          = c.t1;
    t.tseg_2          = c.t2;
    t.sjw             = c.sjw;
    t.triple_sampling = c.triple;
    twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    if (twai_driver_install(&g, &t, &f) != ESP_OK) {
      logmsg("  %-20s INSTALL REFUSED (invalid timing for this chip)", c.name);
      continue;
    }
    if (twai_start() != ESP_OK) {
      logmsg("  %-20s start failed", c.name);
      twai_driver_uninstall();
      continue;
    }

    const uint32_t t0 = millis();
    while (millis() - t0 < 1500) {
      twai_message_t m;
      if (twai_receive(&m, pdMS_TO_TICKS(20)) == ESP_OK) ++g_swGot[i];
    }

    twai_status_info_t st{};
    twai_get_status_info(&st);
    logmsg("  %-20s rx=%lu rxErr=%lu busErr=%lu state=%u", c.name,
           static_cast<unsigned long>(g_swGot[i]),
           static_cast<unsigned long>(st.rx_error_counter),
           static_cast<unsigned long>(st.bus_error_count),
           static_cast<unsigned>(st.state));

    twai_stop();
    twai_driver_uninstall();
    delay(50);

    if (g_swGot[i] && (g_swBest < 0 || g_swGot[i] > g_swGot[g_swBest]))
      g_swBest = static_cast<int>(i);
  }

  if (g_swBest >= 0)
    logmsg("timing sweep: BEST is \"%s\" with %lu frames in NORMAL mode",
           kTimings[g_swBest].name, static_cast<unsigned long>(g_swGot[g_swBest]));
  else
    logmsg("timing sweep: NO configuration received a single frame in normal mode");

  // Give the peripheral back to esp32_can exactly as it was.
  CAN0.setCANPins(kRxPin, kTxPin);
  CAN0.begin(g_rate);
  if (g_listen) CAN0.setListenOnlyMode(true);
  CAN0.setGeneralCallback(&onCanFrame);
  CAN0.watchFor();
  logmsg("timing sweep: restored %s", g_listen ? "listen-only" : "normal");
}

// ---------------------------------------------------------------------------
// The BITRATE sweep — the experiment runTimingSweep() only looked like it ran
// ---------------------------------------------------------------------------
// Read the timing table above and multiply it out: brp * (1 + tseg_1 + tseg_2) is 160 in
// EVERY row. All eight configurations are exactly 500000 bit/s. That sweep varied the sample
// point and never once varied the bitrate, so "8 configs swept, all identical" does not rule
// out what it appears to rule out.
//
// And a small bitrate error is the best fit we have for the symptom. Reception is immune to
// it: a receiver hard-syncs on the start bit and re-syncs on every recessive-to-dominant edge
// after it, so a fraction of a percent never accumulates. Our ACK is not immune. It is a
// single dominant bit we place from our OWN clock, 60-odd bit times after the last edge we
// could have re-synced on, and if our clock is off it lands beside the panel's ACK slot
// instead of inside it. That destroys the frame we were acknowledging — which is exactly what
// we see the instant NORMAL mode is enabled, and exactly why listen-only stays spotless.
//
// So walk the ACHIEVABLE rates around 500k in NORMAL mode and count what decodes. The
// detector needs no interpretation: at every wrong rate the count stays 0, as it is today. At
// the right one our ACK lands, the panel stops retransmitting, and frames appear.
//
// The rates are not free choices. Only brp * tq = 80 MHz / rate with an integer brp is
// reachable, brp is kept even because odd values are rejected on some targets, and the C3
// caps tseg_1 at 16 and tseg_2 at 8. That yields roughly 1-4% steps across +-7%.
struct RateCase {
  uint32_t rate;                 // what this row actually runs at
  uint32_t brp;
  uint8_t  t1, t2, sjw;
};
const RateCase kRates[] = {
  {571429, 14,  7, 2, 2},
  {555556, 12,  9, 2, 2},
  {533333, 10, 11, 3, 3},
  {519481, 14,  8, 2, 2},
  {512821, 12,  9, 3, 3},
  {500000, 10, 12, 3, 3},        // the reference: what every previous sweep row also was
  {493827, 18,  6, 2, 2},
  {476190, 12, 10, 3, 3},
  {470588, 10, 13, 3, 3},
  {454545, 16,  8, 2, 2},
};
constexpr size_t kRateCount = sizeof(kRates) / sizeof(kRates[0]);

volatile bool g_wantRateSweep = false;
bool     g_rsRan  = false;
int      g_rsBest = -1;
uint32_t g_rsGot[kRateCount] = {0};

// SHUT UP AND LET IT COME BACK.
//
// Every NORMAL-mode row leaves the bus in an error storm, and a panel that has been shouted
// at long enough stops transmitting. If the next row starts against a silent panel it scores
// zero no matter how right its bitrate is — so one bad row poisons every row after it, and
// the correct answer is indistinguishable from the wrong ones. That is a false negative the
// original timing sweep had no protection against either.
//
// So between rows: emit NOTHING — listen-only, which cannot even ACK — and wait for the panel
// to start talking again on its own. Returns false if it never does, and then the rows that
// follow are not to be trusted.
bool waitForPanel(uint32_t maxMs) {
  twai_general_config_t g =
      TWAI_GENERAL_CONFIG_DEFAULT(kTxPin, kRxPin, TWAI_MODE_LISTEN_ONLY);
  g.rx_queue_len = 32;
  g.tx_queue_len = 1;
  twai_timing_config_t t = TWAI_TIMING_CONFIG_500KBITS();
  twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();

  if (twai_driver_install(&g, &t, &f) != ESP_OK) return false;
  if (twai_start() != ESP_OK) { twai_driver_uninstall(); return false; }

  bool alive = false;
  const uint32_t t0 = millis();
  while (millis() - t0 < maxMs && !alive) {
    twai_message_t m;
    if (twai_receive(&m, pdMS_TO_TICKS(200)) == ESP_OK) alive = true;
  }
  const uint32_t waited = millis() - t0;

  twai_stop();
  twai_driver_uninstall();
  delay(20);

  if (!alive)
    logmsg("  ... panel STILL SILENT after %lu ms of us saying nothing",
           static_cast<unsigned long>(waited));
  else if (waited > 400)
    logmsg("  ... panel came back after %lu ms of silence",
           static_cast<unsigned long>(waited));
  return alive;
}

void runRateSweep() {
  RunningScope scope("ratesweep");
  g_rsRan  = true;
  g_rsBest = -1;
  logmsg("rate sweep: %u REAL bitrates, NORMAL mode, 1.2 s each - we ACK on every one",
         static_cast<unsigned>(kRateCount));

  CAN0.disable();
  delay(50);

  for (size_t i = 0; i < kRateCount; ++i) {
    const RateCase& c = kRates[i];
    g_rsGot[i] = 0;

    // A row is only worth running against a panel that is actually talking. Up to a minute
    // of complete silence from us first, which is also long enough for it to reset itself.
    if (!waitForPanel(60000)) {
      logmsg("  %6lu  SKIPPED - panel silent, this row would measure nothing",
             static_cast<unsigned long>(c.rate));
      continue;
    }

    twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(kTxPin, kRxPin, TWAI_MODE_NORMAL);
    g.rx_queue_len = 64;
    g.tx_queue_len = 4;
    twai_timing_config_t t{};
    t.brp             = c.brp;
    t.tseg_1          = c.t1;
    t.tseg_2          = c.t2;
    t.sjw             = c.sjw;
    t.triple_sampling = false;
    twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    if (twai_driver_install(&g, &t, &f) != ESP_OK) {
      logmsg("  %6lu  INSTALL REFUSED (brp/tseg invalid on this chip)",
             static_cast<unsigned long>(c.rate));
      continue;
    }
    if (twai_start() != ESP_OK) {
      logmsg("  %6lu  start failed", static_cast<unsigned long>(c.rate));
      twai_driver_uninstall();
      continue;
    }

    // Remember the last payload: if our ACK ever lands, the panel stops repeating its stuck
    // sync request and the DATA changes. That is a stronger signal than the count alone.
    uint8_t  last[8] = {0};
    uint8_t  lastLen = 0;
    const uint32_t t0 = millis();
    while (millis() - t0 < 1200) {
      twai_message_t m;
      if (twai_receive(&m, pdMS_TO_TICKS(20)) == ESP_OK) {
        ++g_rsGot[i];
        lastLen = m.data_length_code > 8 ? 8 : m.data_length_code;
        memcpy(last, m.data, lastLen);
      }
    }

    twai_status_info_t st{};
    twai_get_status_info(&st);
    if (g_rsGot[i]) {
      logmsg("  %6lu  rx=%lu  state=%u txErr=%lu busErr=%lu  last=%02X %02X %02X  <<< DECODES",
             static_cast<unsigned long>(c.rate), static_cast<unsigned long>(g_rsGot[i]),
             static_cast<unsigned>(st.state), static_cast<unsigned long>(st.tx_error_counter),
             static_cast<unsigned long>(st.bus_error_count),
             last[0], last[1], last[2]);
    } else {
      logmsg("  %6lu  rx=0  state=%u txErr=%lu rxErr=%lu busErr=%lu",
             static_cast<unsigned long>(c.rate), static_cast<unsigned>(st.state),
             static_cast<unsigned long>(st.tx_error_counter),
             static_cast<unsigned long>(st.rx_error_counter),
             static_cast<unsigned long>(st.bus_error_count));
    }

    twai_stop();
    twai_driver_uninstall();
    delay(50);

    if (g_rsGot[i] && (g_rsBest < 0 || g_rsGot[i] > g_rsGot[g_rsBest]))
      g_rsBest = static_cast<int>(i);
  }

  if (g_rsBest >= 0)
    logmsg("rate sweep: %lu bit/s DECODES IN NORMAL MODE (%lu frames) - that is the panel's "
           "actual rate, and 500000 was never it",
           static_cast<unsigned long>(kRates[g_rsBest].rate),
           static_cast<unsigned long>(g_rsGot[g_rsBest]));
  else
    logmsg("rate sweep: no rate decoded a single frame while we were allowed to ACK. "
           "The fault is not the bitrate.");

  CAN0.setCANPins(kRxPin, kTxPin);
  CAN0.begin(g_rate);
  if (g_listen) CAN0.setListenOnlyMode(true);
  CAN0.setGeneralCallback(&onCanFrame);
  CAN0.watchFor();
  logmsg("rate sweep: restored %s", g_listen ? "listen-only" : "normal");
}

// ---------------------------------------------------------------------------
// The jam test — does OUR dominant actually reach the bus?
// ---------------------------------------------------------------------------
// This is the question the handoff left open, and /api/pintest cannot answer it. Pintest
// tears the TWAI driver down so it can own GPIO3, which leaves digitalRead() on CRX as the
// only detector — and that detector lied: it reported "CRX high 100%" over an 8 ms window
// while the panel was demonstrably transmitting 1500 frames/s. A sampler that cannot see a
// fully busy bus cannot be trusted to tell us what our own dominant did to it.
//
// So keep the DECODER alive and use IT as the detector. The trick is to install the TWAI
// driver with its TX parked on an unconnected pin: the controller receives on GPIO4 exactly
// as it does now, drives nothing anyone can hear, and leaves GPIO3 free for us to bit-bang.
//
// Then the measurement is three windows of two seconds:
//
//   baseline   CTX recessive           frames decoded = the bus as it normally is
//   jamming    CTX pulsed dominant     frames decoded = the bus with us shouting on it
//   recovery   CTX recessive again     proves we did not break anything permanently
//
// and the verdict needs no interpretation:
//
//   jamming collapses to ~0   our dominant REACHES the bus. The transmit path is physically
//                             fine and the fault is in WHEN we drive, not WHETHER we can.
//   jamming ~= baseline       the bus is indifferent to us. Our dominant never reaches the
//                             wire — transceiver driver dead, disabled, or TXD not connected.
//                             No firmware change can fix that.
//
// Listen-only is deliberate: the controller must not contribute a single dominant bit of its
// own, or the experiment measures the controller and the bit-bang together.
constexpr gpio_num_t kJamParkTx = GPIO_NUM_10;   // unconnected on this board; never driven

volatile bool g_wantJam = false;
bool     g_jamRan  = false;
uint32_t g_jamBase = 0, g_jamJam = 0, g_jamRec = 0;
int      g_jamCrxIdle = -1, g_jamCrxDom = -1;
// % of jam pulses where CTX was READ BACK low after we asked for low. Anything under 100
// means the pad is not ours and every other number this test produces is worthless.
int      g_jamTxLowPct = -1;

// Count frames the controller actually decodes over windowMs. With jam set, CTX is pulsed
// dominant for 100 us at a time — hard enough to destroy any frame in flight, and far inside
// the transceiver's TXD dominant timeout, which is the trap that inverted the old test.
uint32_t jamWindow(uint32_t windowMs, bool jam, int* crxHighPct, int* txLowPct) {
  uint32_t frames = 0, samples = 0, ones = 0, txLow = 0;
  const uint32_t t0 = millis();
  while (millis() - t0 < windowMs) {
    // 50 ms of tight loop, then yield: two seconds without one would trip the task watchdog.
    const uint32_t c0 = millis();
    while (millis() - c0 < 50) {
      if (jam) {
        gpio_set_level(kTxPin, 0);                       // dominant
        delayMicroseconds(20);                           // let the transceiver settle
        // READ CTX BACK, not just CRX. This is the check whose absence would let the test
        // report "the bus ignored us" when the truth is "we never actually drove the pin".
        txLow += gpio_get_level(kTxPin) ? 0u : 1u;
        ones  += gpio_get_level(kRxPin) ? 1u : 0u; ++samples;
        delayMicroseconds(80);
        gpio_set_level(kTxPin, 1);                       // recessive, well inside the timeout
        delayMicroseconds(100);
      } else {
        ones += gpio_get_level(kRxPin) ? 1u : 0u; ++samples;
        delayMicroseconds(200);
      }
      twai_message_t m;
      while (twai_receive(&m, 0) == ESP_OK) ++frames;
    }
    delay(1);
  }
  if (crxHighPct) *crxHighPct = samples ? static_cast<int>((ones  * 100) / samples) : -1;
  if (txLowPct)   *txLowPct   = samples ? static_cast<int>((txLow * 100) / samples) : -1;
  return frames;
}

void runJam() {
  RunningScope scope("jam");
  g_jamRan = true;
  if (g_rate != 500000)
    logmsg("jam: rate is %lu but this test installs 500k - results are meaningless",
           static_cast<unsigned long>(g_rate));

  logmsg("jam: decoder stays LIVE on GPIO%d, TWAI TX parked on unconnected GPIO%d, "
         "GPIO%d is ours to drive", static_cast<int>(kRxPin),
         static_cast<int>(kJamParkTx), static_cast<int>(kTxPin));

  CAN0.disable();
  delay(50);

  twai_general_config_t g =
      TWAI_GENERAL_CONFIG_DEFAULT(kJamParkTx, kRxPin, TWAI_MODE_LISTEN_ONLY);
  g.rx_queue_len = 64;
  g.tx_queue_len = 1;
  twai_timing_config_t t = TWAI_TIMING_CONFIG_500KBITS();
  twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();

  if (twai_driver_install(&g, &t, &f) != ESP_OK || twai_start() != ESP_OK) {
    logmsg("jam: could not install the listen-only decoder - test aborted, nothing measured");
    twai_driver_uninstall();
  } else {
    // The peripheral has GPIO10 and GPIO4. GPIO3 is still matrix-attached to the OLD TWAI TX
    // until it is reset by hand — the same trap documented on runPinTest(), and the reason
    // INPUT_OUTPUT is used: it keeps the input buffer alive so the level can be read back.
    gpio_reset_pin(kTxPin);
    gpio_set_direction(kTxPin, GPIO_MODE_INPUT_OUTPUT);
    gpio_set_level(kTxPin, 1);
    delay(5);
    const int readbackHigh = gpio_get_level(kTxPin);

    g_jamBase = jamWindow(2000, false, &g_jamCrxIdle, nullptr);
    g_jamJam  = jamWindow(2000, true,  &g_jamCrxDom,  &g_jamTxLowPct);
    gpio_set_level(kTxPin, 1);
    g_jamRec  = jamWindow(2000, false, nullptr,       nullptr);

    twai_stop();
    twai_driver_uninstall();

    logmsg("jam: baseline %lu frames/2s (CRX high %d%%)",
           static_cast<unsigned long>(g_jamBase), g_jamCrxIdle);
    logmsg("jam: JAMMING  %lu frames/2s (CRX high %d%% while we held CTX dominant)",
           static_cast<unsigned long>(g_jamJam), g_jamCrxDom);
    logmsg("jam: after    %lu frames/2s", static_cast<unsigned long>(g_jamRec));
    logmsg("jam: CTX read back LOW on %d%% of the pulses we asked to be low", g_jamTxLowPct);

    if (g_jamTxLowPct < 95) {
      logmsg("jam INVALID - we asked GPIO%d for dominant and the PAD DID NOT FOLLOW (%d%%). "
             "The pin is not under GPIO control, so this test says nothing about the bus. "
             "Fix the pad ownership first.", static_cast<int>(kTxPin), g_jamTxLowPct);
    } else if (readbackHigh != 1) {
      logmsg("jam INVALID - GPIO%d did not read back recessive; something external holds it",
             static_cast<int>(kTxPin));
    } else if (g_jamBase < 200) {
      logmsg("jam INCONCLUSIVE - the bus was not busy enough to notice being jammed "
             "(%lu frames baseline). Nothing can be concluded.",
             static_cast<unsigned long>(g_jamBase));
    } else if (g_jamJam * 4 < g_jamBase) {
      logmsg("jam PASS - our dominant REACHES the bus: jamming destroyed %lu%% of the "
             "traffic. The transmit path is physically fine; the fault is in WHEN we drive.",
             static_cast<unsigned long>(100 - (g_jamJam * 100) / g_jamBase));
    } else if (g_jamJam * 10 > g_jamBase * 8) {
      logmsg("jam FAIL - the bus is INDIFFERENT to us (%lu vs %lu baseline). Our dominant "
             "never reaches the wire: transceiver driver dead, disabled, or TXD not "
             "connected to GPIO%d. No firmware change can fix this.",
             static_cast<unsigned long>(g_jamJam), static_cast<unsigned long>(g_jamBase),
             static_cast<int>(kTxPin));
    } else {
      logmsg("jam PARTIAL - %lu vs %lu baseline. Our dominant reaches the bus weakly, which "
             "is what a marginal driver or missing termination looks like.",
             static_cast<unsigned long>(g_jamJam), static_cast<unsigned long>(g_jamBase));
    }
  }

  // Give the peripheral back to esp32_can exactly as it was.
  gpio_reset_pin(kTxPin);
  gpio_reset_pin(kRxPin);
  CAN0.setCANPins(kRxPin, kTxPin);
  CAN0.begin(g_rate);
  if (g_listen) CAN0.setListenOnlyMode(true);
  CAN0.setGeneralCallback(&onCanFrame);
  CAN0.watchFor();
  logmsg("jam: TWAI restored, %s", g_listen ? "listen-only" : "normal");
}

// ---------------------------------------------------------------------------
// The jam sweep — if GPIO3 is not TXD, which pin is?
// ---------------------------------------------------------------------------
// runJam() proves the bus ignores GPIO3. There are two ways that happens: the transceiver
// cannot drive (dead output stage, or a standby/silent pin asserted), or GPIO3 is simply not
// the pin soldered to TXD. The second one is free to rule out, and the rig's own history says
// to take it seriously — the memory of this board records that MeganeCAN's otherwise identical
// module is wired with rx and tx the other way round.
//
// So run the jam measurement once per candidate pin. Any pin that collapses the decoded frame
// count IS the transmit path, whatever the silkscreen says. GPIO4 is excluded because it is
// proven to be RXD; GPIO18/19 are the C3's USB pins and would disconnect the console; GPIO21
// parks the controller's own transmitter, which in listen-only never drives dominant and so
// cannot affect the measurement.
const gpio_num_t kSweepPins[] = {
  GPIO_NUM_0, GPIO_NUM_1, GPIO_NUM_2,  GPIO_NUM_3,  GPIO_NUM_5, GPIO_NUM_6,
  GPIO_NUM_7, GPIO_NUM_8, GPIO_NUM_9,  GPIO_NUM_10, GPIO_NUM_20,
};
constexpr size_t kSweepCount = sizeof(kSweepPins) / sizeof(kSweepPins[0]);
constexpr gpio_num_t kSweepParkTx = GPIO_NUM_21;

volatile bool g_wantJamSweep = false;
bool     g_jsRan = false;
int      g_jsFound = -1;                 // the pin that jammed, or -1
uint32_t g_jsBase = 0;
uint32_t g_jsGot[kSweepCount] = {0};

// The same window as jamWindow(), against an arbitrary pin.
uint32_t sweepWindow(gpio_num_t pin, uint32_t windowMs, bool jam) {
  uint32_t frames = 0;
  const uint32_t t0 = millis();
  while (millis() - t0 < windowMs) {
    const uint32_t c0 = millis();
    while (millis() - c0 < 50) {
      if (jam) {
        gpio_set_level(pin, 0);
        delayMicroseconds(100);
        gpio_set_level(pin, 1);
        delayMicroseconds(100);
      } else {
        delayMicroseconds(200);
      }
      twai_message_t m;
      while (twai_receive(&m, 0) == ESP_OK) ++frames;
    }
    delay(1);
  }
  return frames;
}

void runJamSweep() {
  RunningScope scope("jamsweep");
  g_jsRan   = true;
  g_jsFound = -1;
  logmsg("jam sweep: %u candidate pins, 800 ms each, decoder live on GPIO%d",
         static_cast<unsigned>(kSweepCount), static_cast<int>(kRxPin));

  CAN0.disable();
  delay(50);

  twai_general_config_t g =
      TWAI_GENERAL_CONFIG_DEFAULT(kSweepParkTx, kRxPin, TWAI_MODE_LISTEN_ONLY);
  g.rx_queue_len = 64;
  g.tx_queue_len = 1;
  twai_timing_config_t t = TWAI_TIMING_CONFIG_500KBITS();
  twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();

  if (twai_driver_install(&g, &t, &f) != ESP_OK || twai_start() != ESP_OK) {
    logmsg("jam sweep: decoder would not install - aborted");
    twai_driver_uninstall();
  } else {
    g_jsBase = sweepWindow(kRxPin, 800, false);       // pin unused when jam is false
    logmsg("jam sweep: baseline %lu frames/800ms", static_cast<unsigned long>(g_jsBase));

    if (g_jsBase < 100) {
      logmsg("jam sweep: bus too quiet to detect jamming - nothing measured");
    } else {
      for (size_t i = 0; i < kSweepCount; ++i) {
        const gpio_num_t p = kSweepPins[i];
        gpio_reset_pin(p);
        gpio_set_direction(p, GPIO_MODE_INPUT_OUTPUT);
        gpio_set_level(p, 1);
        delay(2);

        g_jsGot[i] = sweepWindow(p, 800, true);

        gpio_set_level(p, 1);
        gpio_reset_pin(p);

        const bool hit = (g_jsGot[i] * 4 < g_jsBase);
        logmsg("  GPIO%-2d  %4lu frames  %s", static_cast<int>(p),
               static_cast<unsigned long>(g_jsGot[i]), hit ? "<<< JAMS THE BUS" : "no effect");
        if (hit && g_jsFound < 0) g_jsFound = static_cast<int>(p);
        delay(20);
      }

      if (g_jsFound >= 0)
        logmsg("jam sweep: GPIO%d reaches the bus. THAT is the transmit pin - rebuild with "
               "tx=GPIO%d.", g_jsFound, g_jsFound);
      else
        logmsg("jam sweep: NO pin on this board can disturb the bus. The transmit path is "
               "not a wiring choice we can make in firmware - it is the transceiver.");
    }

    twai_stop();
    twai_driver_uninstall();
  }

  gpio_reset_pin(kTxPin);
  gpio_reset_pin(kRxPin);
  CAN0.setCANPins(kRxPin, kTxPin);
  CAN0.begin(g_rate);
  if (g_listen) CAN0.setListenOnlyMode(true);
  CAN0.setGeneralCallback(&onCanFrame);
  CAN0.watchFor();
  logmsg("jam sweep: TWAI restored, %s", g_listen ? "listen-only" : "normal");
}

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------
// Sized for the worst case, which is jLog() serialising a full 64-entry ring.
char   g_out[12288];
size_t g_outN = 0;
size_t jroom()  { return sizeof(g_out) - 1 - g_outN; }
void   jclear() { g_outN = 0; g_out[0] = 0; }

void jf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void jf(const char* fmt, ...) {
  const size_t room = jroom();
  if (room == 0) return;
  va_list ap;
  va_start(ap, fmt);
  const int n = vsnprintf(g_out + g_outN, room + 1, fmt, ap);
  va_end(ap);
  if (n > 0) g_outN += (static_cast<size_t>(n) > room) ? room : static_cast<size_t>(n);
}

void jstr(const char* s) {
  jf("\"");
  for (; *s; ++s) {
    if (*s == '"' || *s == '\\') jf("\\%c", *s);
    else if (static_cast<uint8_t>(*s) < 0x20) jf("\\u%04x", *s);
    else jf("%c", *s);
  }
  jf("\"");
}

void jStatus() {
  twai_status_info_t st{};
  const bool valid = (twai_get_status_info(&st) == ESP_OK);

  jclear();
  jf("{");
  jf("\"uptimeMs\":%lu,", static_cast<unsigned long>(millis()));
  jf("\"heapFree\":%lu,", static_cast<unsigned long>(ESP.getFreeHeap()));
  jf("\"rate\":%lu,", static_cast<unsigned long>(g_rate));
  jf("\"listenOnly\":%s,", g_listen ? "true" : "false");
  jf("\"canUp\":%s,", g_canUp ? "true" : "false");
  jf("\"pins\":{\"rx\":%d,\"tx\":%d},", static_cast<int>(kRxPin), static_cast<int>(kTxPin));

  // THE VERDICT LINE. hearing is the only one that matters first.
  jf("\"hearing\":%s,", g_rxCount ? "true" : "false");
  jf("\"rx\":%lu,\"tx\":%lu,", static_cast<unsigned long>(g_rxCount),
     static_cast<unsigned long>(g_txCount));
  jf("\"sinceRxMs\":%ld,", g_lastRxMs ? static_cast<long>(millis() - g_lastRxMs) : -1L);

  jf("\"autospeed\":{\"ran\":%s,\"found\":%lu,\"busy\":%s},",
     g_autoRan ? "true" : "false", static_cast<unsigned long>(g_autoResult),
     g_wantAutoSpeed ? "true" : "false");

  jf("\"selftest\":{\"ran\":%s,\"pass\":%s,\"tries\":%u,\"framesSeen\":%lu,\"busy\":%s},",
     g_stRan ? "true" : "false", g_stPass ? "true" : "false",
     static_cast<unsigned>(g_stTries), static_cast<unsigned long>(g_stGot),
     g_wantSelfTest ? "true" : "false");

  jf("\"jam\":{\"ran\":%s,\"base\":%lu,\"jam\":%lu,\"after\":%lu,"
     "\"crxIdlePct\":%d,\"crxDomPct\":%d,\"txLowPct\":%d,\"busy\":%s},",
     g_jamRan ? "true" : "false", static_cast<unsigned long>(g_jamBase),
     static_cast<unsigned long>(g_jamJam), static_cast<unsigned long>(g_jamRec),
     g_jamCrxIdle, g_jamCrxDom, g_jamTxLowPct, g_wantJam ? "true" : "false");

  jf("\"running\":");
  if (g_running) jstr(const_cast<const char*>(g_running)); else jf("null");
  jf(",");

  jf("\"ratesweep\":{\"ran\":%s,\"bestRate\":%lu,\"bestRx\":%lu,\"busy\":%s},",
     g_rsRan ? "true" : "false",
     static_cast<unsigned long>(g_rsBest >= 0 ? kRates[g_rsBest].rate : 0),
     static_cast<unsigned long>(g_rsBest >= 0 ? g_rsGot[g_rsBest] : 0),
     g_wantRateSweep ? "true" : "false");

  jf("\"jamsweep\":{\"ran\":%s,\"found\":%d,\"base\":%lu,\"busy\":%s},",
     g_jsRan ? "true" : "false", g_jsFound, static_cast<unsigned long>(g_jsBase),
     g_wantJamSweep ? "true" : "false");

  jf("\"drv\":{\"valid\":%s,\"state\":%u,\"txErr\":%lu,\"rxErr\":%lu,\"busErr\":%lu,"
     "\"arbLost\":%lu,\"rxMissed\":%lu,\"toTx\":%lu,\"toRx\":%lu},",
     valid ? "true" : "false", static_cast<unsigned>(st.state),
     static_cast<unsigned long>(st.tx_error_counter),
     static_cast<unsigned long>(st.rx_error_counter),
     static_cast<unsigned long>(st.bus_error_count),
     static_cast<unsigned long>(st.arb_lost_count),
     static_cast<unsigned long>(st.rx_missed_count),
     static_cast<unsigned long>(st.msgs_to_tx),
     static_cast<unsigned long>(st.msgs_to_rx));

  jf("\"wifi\":{\"mode\":");
  jstr(WiFi.isConnected() ? "sta" : "ap");
  {
    const String ip = WiFi.isConnected() ? WiFi.localIP().toString()
                                         : WiFi.softAPIP().toString();
    jf(",\"ip\":"); jstr(ip.c_str());
  }
  jf(",\"rssi\":%d}", WiFi.isConnected() ? static_cast<int>(WiFi.RSSI()) : 0);
  jf("}");
}

void jFrames(long want) {
  if (want <= 0) want = 48;
  if (want > static_cast<long>(kRing)) want = kRing;

  // STATIC, not a local: 2 kB does not belong on esp_http_server's task stack.
  static FrameRec snap[kRing];
  size_t   head;
  uint32_t seq;
  portENTER_CRITICAL(&g_mux);
  memcpy(snap, g_ring, sizeof(snap));
  head = g_head;
  seq  = g_seq;
  portEXIT_CRITICAL(&g_mux);

  const size_t have = (seq < kRing) ? static_cast<size_t>(seq) : kRing;
  const size_t n    = (static_cast<size_t>(want) < have) ? static_cast<size_t>(want) : have;

  jclear();
  jf("{\"total\":%lu,\"f\":[", static_cast<unsigned long>(seq));
  for (size_t i = 0; i < n; ++i) {                       // oldest first: reads like a trace
    const FrameRec& r = snap[(head + kRing - n + i) % kRing];
    if (i) jf(",");
    jf("[%lu,\"%s\",\"%0*lX\",%u,\"", static_cast<unsigned long>(r.ms),
       r.dir ? "TX" : "RX", r.ext ? 8 : 3, static_cast<unsigned long>(r.id),
       static_cast<unsigned>(r.len));
    for (uint8_t b = 0; b < r.len; ++b)
      jf("%s%02X", b ? " " : "", static_cast<unsigned>(r.d[b]));
    jf("\",\"%s%s\"]", r.ext ? "ext" : "std", r.rtr ? ",rtr" : "");
  }
  jf("]}");
}

void jLog() {
  static LogRec snap[kLogRing];
  size_t head;
  portENTER_CRITICAL(&g_logMux);
  memcpy(snap, g_log, sizeof(snap));
  head = g_logHead;
  portEXIT_CRITICAL(&g_logMux);

  jclear();
  jf("{\"l\":[");
  bool first = true;
  for (size_t i = 0; i < kLogRing; ++i) {
    const LogRec& r = snap[(head + i) % kLogRing];
    if (!r.msg[0]) continue;
    if (!first) jf(",");
    first = false;
    jf("[%lu,", static_cast<unsigned long>(r.ms));
    jstr(r.msg);
    jf("]");
  }
  jf("]}");
}

const char kPage[] PROGMEM = R"HTML(<!doctype html><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>CAN spy</title>
<style>
body{font:13px ui-monospace,Consolas,monospace;margin:0;padding:12px;background:#111;color:#ddd}
h1{font:600 15px system-ui;margin:0 0 8px}
#v{font:600 16px system-ui;padding:8px;border-radius:5px;margin:8px 0;background:#222}
.ok{background:#143!important;color:#8f8}.bad{background:#411!important;color:#f99}
button{font:inherit;padding:5px 9px;margin:2px;background:#243;color:#cfc;border:1px solid #486;border-radius:4px;cursor:pointer}
button.w{background:#432;color:#fda;border-color:#754}
a{color:#8cf}
pre{background:#000;padding:8px;border-radius:4px;overflow:auto;max-height:34vh;margin:8px 0}
label{margin-left:10px}
</style>
<h1>CAN spy — is the ESP32 seeing frames?</h1>
<div id=v>…</div>
<div>
<button onclick=go('/api/listen?on=1')>LISTEN-ONLY (passive)</button>
<button onclick=go('/api/listen?on=0')>normal (we ACK)</button>
<button onclick=go("/api/ratesweep")>RATE SWEEP (real bitrates, we ACK)</button>
<button onclick=go("/api/jam")>JAM TEST (decoder live - does our dominant reach the bus?)</button>
<button onclick=go("/api/pintest")>PIN TEST (drive CTX, watch CRX)</button>
<button onclick=go("/api/looptest?pin=6")>INTERNAL LOOPBACK (no transceiver)</button>
<button onclick=go("/api/selftest")>TX self-test on real pins</button>
<button onclick=go('/api/send')>send test frame</button>
<button onclick=go('/api/clear')>clear</button>
<button onclick=go('/api/autospeed')>autospeed</button>
</div>
<div>
<button class=w onclick=go('/api/rate?v=500000')>500k</button>
<button class=w onclick=go('/api/rate?v=250000')>250k</button>
<button class=w onclick=go('/api/rate?v=125000')>125k</button>
<button class=w onclick=go('/api/rate?v=1000000')>1M</button>
<button class=w onclick=go('/api/reboot')>reboot</button>
<a href=/update>OTA</a>
</div>
<div><button onclick=tick()>refresh</button><label><input type=checkbox id=a checked> auto (2s)</label></div>
<pre id=s></pre><pre id=f></pre><pre id=l></pre>
<script>
const $=i=>document.getElementById(i)
async function go(u){try{await fetch(u)}catch(e){};setTimeout(tick,400)}
async function tick(){
 try{
  const s=await (await fetch('/api/status')).json()
  const v=$('v')
  v.className=s.hearing?'ok':'bad'
  v.textContent=(s.hearing?'HEARING — '+s.rx+' frames in':'NOT HEARING — 0 frames in')
   +'   @'+s.rate+' bit/s   '+(s.listenOnly?'LISTEN-ONLY':'normal')+'   drv.state='+s.drv.state
   +'  txErr='+s.drv.txErr+' rxErr='+s.drv.rxErr+' busErr='+s.drv.busErr
   +(s.autospeed.ran?('   autospeed='+(s.autospeed.found||'nothing')):'')
  $('s').textContent=JSON.stringify(s,null,1)
  const f=await (await fetch('/api/frames?n=48')).json()
  $('f').textContent='frames total '+f.total+'\n'+f.f.map(r=>r[0]+'  '+r[1]+' '+r[2]+' ['+r[3]+'] '+r[4]+'  '+r[5]).join('\n')
  const g=await (await fetch('/api/log')).json()
  $('l').textContent=g.l.map(r=>r[0]+'  '+r[1]).join('\n')
 }catch(e){$('s').textContent='fetch failed: '+e}
}
setInterval(()=>{if($('a').checked)tick()},2000);tick()
</script>)HTML";

long pnum(PsychicRequest* r, const char* k, long dflt) {
  if (!r->hasParam(k)) return dflt;
  const String v = r->getParam(k)->value();
  if (!v.length()) return dflt;
  return strtol(v.c_str(), nullptr, 0);
}

esp_err_t replyJson(PsychicRequest* r) { return r->reply(200, "application/json", g_out); }

void routes() {
  g_server.on("/", HTTP_GET, [](PsychicRequest* r) {
    return r->reply(200, "text/html", kPage);
  });
  g_server.on("/api/ping", HTTP_GET, [](PsychicRequest* r) {
    return r->reply(200, "text/plain", "pong");
  });
  g_server.on("/api/status", HTTP_GET, [](PsychicRequest* r) {
    jStatus(); return replyJson(r);
  });
  g_server.on("/api/frames", HTTP_GET, [](PsychicRequest* r) {
    jFrames(pnum(r, "n", 48)); return replyJson(r);
  });
  g_server.on("/api/log", HTTP_GET, [](PsychicRequest* r) {
    jLog(); return replyJson(r);
  });

  g_server.on("/api/clear", HTTP_GET, [](PsychicRequest* r) {
    portENTER_CRITICAL(&g_mux);
    g_head = 0; g_seq = 0; g_rxCount = 0; g_txCount = 0; g_lastRxMs = 0;
    memset(g_ring, 0, sizeof(g_ring));
    portEXIT_CRITICAL(&g_mux);
    jclear(); jf("{\"cleared\":true}");
    return replyJson(r);
  });

  // THE A/B TEST. Listen-only makes the controller electrically passive: no ACK bits, no
  // error frames, nothing of ours on the wire. If frames decode here and not in normal mode,
  // reception is fine and the trouble is on our transmit side.
  g_server.on("/api/listen", HTTP_GET, [](PsychicRequest* r) {
    g_wantListen = pnum(r, "on", 1) != 0 ? 1 : 0;
    jclear(); jf("{\"queued\":true,\"listenOnly\":%s}", g_wantListen ? "true" : "false");
    return replyJson(r);
  });

  g_server.on("/api/selftest", HTTP_GET, [](PsychicRequest* r) {
    g_wantSelfTest = true;
    jclear(); jf("{\"queued\":true}");
    return replyJson(r);
  });

  // The third mode, and the one that splits "we transmit at all" from "we send the ACK".
  // NO_ACK drives the bus like normal mode but does not acknowledge what it receives. If
  // frames arrive here and not in normal mode, the ACK bit alone is the problem.
  // Watch OUR OWN transmit pin while TWAI owns it. We send no frames, so in every mode this
  // pad should sit recessive (HIGH) essentially all of the time — dipping only for an ACK.
  // If it reads LOW in normal/NO_ACK and HIGH in listen-only, our transmit output is holding
  // the bus dominant and that alone explains why reception dies the moment we are allowed to
  // drive. Reading a pad owned by a peripheral is not guaranteed, so a constant answer in
  // BOTH modes means the probe failed, not that the pin is fine.
  g_server.on("/api/txwatch", HTTP_GET, [](PsychicRequest* r) {
    uint32_t ones = 0;
    constexpr uint32_t kN = 2000;
    for (uint32_t i = 0; i < kN; ++i) {
      if (gpio_get_level(kTxPin)) ++ones;
      delayMicroseconds(10);
    }
    const uint32_t rxOnes = [] {
      uint32_t o = 0;
      for (uint32_t i = 0; i < 2000; ++i) { if (gpio_get_level(kRxPin)) ++o; delayMicroseconds(10); }
      return o;
    }();
    jclear();
    jf("{\"txHighPct\":%lu,\"rxHighPct\":%lu,\"listenOnly\":%s}",
       static_cast<unsigned long>((ones * 100) / kN),
       static_cast<unsigned long>((rxOnes * 100) / 2000),
       g_listen ? "true" : "false");
    return replyJson(r);
  });

  g_server.on("/api/noack", HTTP_GET, [](PsychicRequest* r) {
    g_wantNoAck = pnum(r, "on", 1) != 0 ? 1 : 0;
    jclear(); jf("{\"queued\":true,\"noack\":%s}", g_wantNoAck ? "true" : "false");
    return replyJson(r);
  });

  g_server.on("/api/timingsweep", HTTP_GET, [](PsychicRequest* r) {
    g_wantSweep = true;
    jclear(); jf("{\"queued\":true}");
    return replyJson(r);
  });

  g_server.on("/api/pintest", HTTP_GET, [](PsychicRequest* r) {
    g_wantPinTest = true;
    jclear(); jf("{\"queued\":true}");
    return replyJson(r);
  });

  // Six seconds of measurement; the answer lands in /api/log and /api/status.
  g_server.on("/api/jam", HTTP_GET, [](PsychicRequest* r) {
    g_wantJam = true;
    jclear(); jf("{\"queued\":true}");
    return replyJson(r);
  });

  g_server.on("/api/ratesweep", HTTP_GET, [](PsychicRequest* r) {
    g_wantRateSweep = true;
    jclear(); jf("{\"queued\":true}");
    return replyJson(r);
  });

  g_server.on("/api/jamsweep", HTTP_GET, [](PsychicRequest* r) {
    g_wantJamSweep = true;
    jclear(); jf("{\"queued\":true}");
    return replyJson(r);
  });

  g_server.on("/api/looptest", HTTP_GET, [](PsychicRequest* r) {
    g_wantLoop = static_cast<int16_t>(pnum(r, "pin", 6));
    jclear(); jf("{\"queued\":true,\"pin\":%d}", static_cast<int>(g_wantLoop));
    return replyJson(r);
  });

  // Requested here, performed in loop(): it blocks for seconds.
  g_server.on("/api/autospeed", HTTP_GET, [](PsychicRequest* r) {
    g_wantAutoSpeed = true;
    jclear(); jf("{\"queued\":true}");
    return replyJson(r);
  });

  // Persist and reboot rather than reinstalling the driver under a live callback.
  g_server.on("/api/rate", HTTP_GET, [](PsychicRequest* r) {
    const long v = pnum(r, "v", kDefaultRate);
    Preferences p;
    if (p.begin(kOwnNamespace, false)) { p.putULong("rate", static_cast<uint32_t>(v)); p.end(); }
    jclear(); jf("{\"rate\":%ld,\"rebooting\":true}", v);
    g_rebootAt = millis() + 400;
    return replyJson(r);
  });

  // Transmit one frame. Defaults to the AFFA heartbeat so the panel sees something it knows.
  // If txErr stays 0 afterwards, somebody acknowledged us — that is the "are we answering"
  // half, and it needs another node on the bus to mean anything.
  g_server.on("/api/send", HTTP_GET, [](PsychicRequest* r) {
    CAN_FRAME f;
    memset(&f, 0, sizeof(f));
    f.id       = static_cast<uint32_t>(pnum(r, "id", 0x3AF));
    f.extended = 0;
    f.rtr      = 0;
    f.length   = 8;
    f.data.uint8[0] = static_cast<uint8_t>(pnum(r, "b0", 0xB9));
    CAN0.sendFrame(f);
    push(f.id, f.data.uint8, f.length, 0, 0, /*tx=*/true);

    twai_status_info_t st{};
    twai_get_status_info(&st);
    jclear();
    jf("{\"sent\":true,\"id\":\"%03lX\",\"txErrAfter\":%lu,\"state\":%u}",
       static_cast<unsigned long>(f.id),
       static_cast<unsigned long>(st.tx_error_counter),
       static_cast<unsigned>(st.state));
    return replyJson(r);
  });

  g_server.on("/api/reboot", HTTP_GET, [](PsychicRequest* r) {
    jclear(); jf("{\"rebooting\":true}");
    g_rebootAt = millis() + 300;
    return replyJson(r);
  });
}

void startNetwork() {
  Preferences p;
  String ssid, pass;
  if (p.begin(kWifiNamespace, /*readOnly=*/true)) {
    ssid = p.getString("ssid", "");
    pass = p.getString("pass", "");
    p.end();
  }

  WiFi.persistent(false);
  WiFi.setSleep(true);

  bool sta = false;
  if (ssid.length()) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());
    const uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < kStaJoinMs) delay(100);
    sta = (WiFi.status() == WL_CONNECTED);
  }
  if (!sta) { WiFi.mode(WIFI_AP); WiFi.softAP(kApSsid, kApPass); }
  if (MDNS.begin(kMdnsName)) MDNS.addService("http", "tcp", 80);

  const String ip = sta ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  Serial.printf("\n[wifi] %s ip=%s  http://%s/  OTA http://%s/update\n",
                sta ? "STA" : "AP", ip.c_str(), ip.c_str(), ip.c_str());
}

void startHttp() {
  // The lockout fix: without lru_purge_enable a full socket table is PERMANENT — httpd stops
  // accepting and never resumes, ICMP and mDNS keep answering, and OTA is gone.
  g_server.config.lru_purge_enable  = true;
  g_server.config.max_open_sockets  = 7;
  g_server.config.recv_wait_timeout = 3;
  g_server.config.send_wait_timeout = 3;
  // THE SECOND LOCKOUT, and it cost a cable to learn. esp_http_server's URI table is a FIXED
  // ARRAY of max_uri_handlers entries, and httpd_register_uri_handler() past the end simply
  // fails — PsychicHttp does not check the return, so the route is silently absent and every
  // request to it falls through to the not-found handler. With this at 20, seventeen routes
  // here plus ElegantOTA's three (/update, /ota/start, /ota/upload) sat EXACTLY on the limit.
  // Adding one diagnostic endpoint pushed /ota/upload — the last one registered — off the
  // end, and the board could no longer be flashed: /ota/start still answered "OK" and the
  // upload came back "Request body must be less than 16384 bytes!", which is PsychicHttp's
  // default handler talking, not ElegantOTA.
  g_server.config.max_uri_handlers  = 32;
  g_server.config.stack_size        = 8192;

  g_server.listen(80);

  // OTA FIRST, ALWAYS. It is the way back into a board with no cable, so it takes its slots
  // before anything else can crowd it out. Diagnostics are what this firmware is FOR and they
  // will keep being added; if the table ever fills again, the thing that breaks must be a
  // diagnostic endpoint, never the route that lets the next image in.
  ElegantOTA.onStart([]() { g_otaRunning = true; logmsg("ota started"); });
  ElegantOTA.onEnd([](bool ok) { logmsg("ota %s", ok ? "ok, rebooting" : "FAILED"); });
  ElegantOTA.begin(&g_server);

  routes();
}

}  // namespace

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  delay(300);
  Serial.println("\n02_canspy — bare esp32_can, no AffaDisplay");

  // NETWORK AND OTA FIRST. Everything after this may fail without costing us the board.
  startNetwork();
  startHttp();

  {
    Preferences p;
    if (p.begin(kOwnNamespace, /*readOnly=*/true)) {
      g_rate = p.getULong("rate", kDefaultRate);
      p.end();
    }
  }
  if (!g_rate) g_rate = kDefaultRate;

  {
    Preferences p;
    if (p.begin(kOwnNamespace, /*readOnly=*/true)) {
      g_listen = p.getUChar("listen", 0) != 0;
      p.end();
    }
  }

  g_canUp = canStart(g_rate, g_listen);
}

void loop() {
  const uint32_t now = millis();

  if (g_wantListen >= 0 && !g_otaRunning) {
    const bool on = g_wantListen != 0;
    g_wantListen = -1;
    applyListen(on);
  }

  if (g_wantNoAck >= 0 && !g_otaRunning) {
    const bool on = g_wantNoAck != 0;
    g_wantNoAck = -1;
    logmsg("switching to %s ...", on ? "NO_ACK (we transmit, but never acknowledge)"
                                     : "normal");
    CAN0.setNoACKMode(on);
    CAN0.setGeneralCallback(&onCanFrame);
    CAN0.watchFor();
    g_listen = false;
    twai_status_info_t st{};
    twai_get_status_info(&st);
    logmsg("now %s, drv.state=%u", on ? "NO_ACK" : "normal", static_cast<unsigned>(st.state));
  }

  if (g_wantSweep && !g_otaRunning) {
    g_wantSweep = false;
    runTimingSweep();
  }

  if (g_wantPinTest && !g_otaRunning) {
    g_wantPinTest = false;
    runPinTest();
  }

  if (g_wantJam && !g_otaRunning) {
    g_wantJam = false;
    runJam();
  }

  if (g_wantRateSweep && !g_otaRunning) {
    g_wantRateSweep = false;
    runRateSweep();
  }

  if (g_wantJamSweep && !g_otaRunning) {
    g_wantJamSweep = false;
    runJamSweep();
  }

  if (g_wantLoop >= 0 && !g_otaRunning) {
    const int pin = g_wantLoop;
    g_wantLoop = -1;
    runLoopback(pin);
  }

  if (g_wantSelfTest && !g_otaRunning) {
    g_wantSelfTest = false;
    runSelfTest();
  }

  if (g_wantAutoSpeed && !g_otaRunning) {
    g_wantAutoSpeed = false;
    runAutoSpeed();
  }

  ElegantOTA.loop();
  if (g_rebootAt && static_cast<int32_t>(now - g_rebootAt) >= 0) ESP.restart();

  delay(10);
}
