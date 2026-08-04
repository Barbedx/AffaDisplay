// ============================================================================
// HISTORICAL. DO NOT COPY THIS HANDSHAKE. Superseded 2026-08-04 by the OEM captures.
// This file DID put 10:00 on real glass and its black box is still worth reading — but it
// is built on an authorization model the captures disproved:
//   * it treats `61 11 00` as the only authorizing request, with `01` behind a compatibility
//     flag. THEY ARE THE SAME REQUEST: "cONNECT OT POWER" completes an entire session on
//     `01` with zero `00` frames. The low bit reports the panel's state, not permission.
//   * it answers the FIRST request with the hello burst rather than with `BA`.
//   * it registers 151/1F1 without waiting for the display's own `1C1 70`, which the OEM
//     always sends first (0.81-1.55 ms after B0#1, ~61 ms before our registration).
// See docs/CARMINAT-HANDSHAKE-GROUND-TRUTH.md and examples/README.md. Use 09_golden.
// ============================================================================
// 06_authclock — panel-initiated authorization + clock 10:00. Black box.
//
// THE EXPERIMENT THIS EXAMPLE EXISTS FOR: every image on this bench so far configured the
// driver — forceRecovery, a peripheral reset before install, a TXD hold at boot. The owner
// suspects exactly those settings. So this image touches NONE of them. The complete driver
// setup is four stock driver operations:
//
//     twai_driver_install(...);       // board wiring + bitrate + accept-all filter
//     twai_start();                    // the 500 kbit/s controller starts
//     twai_receive() in one RX task;  // direct receive path copies every accepted frame
//     twai_transmit() with alerts;     // report queueing and controller completion
//
// No setForceRecovery. No periph_module_reset. No pinMode on TXD before install. With the
// corrected CTX/CRX routing it also runs in stock normal mode, so an outbound controller
// success requires a physical ACK from the bus.
//
// WHAT IT DOES: it waits for the AFFA3 NAV panel to speak first (`3CF 69` liveness or a
// full `3CF 61 11 xx` request). The primary opening sequence is taken from the supplied
// real-radio monitor captures: `69 -> B9`, one `BA` probe, then `B0 14 11 ...` three times
// about 31 ms apart. `61 11 00` is the normal confirmation. An explicitly enabled legacy
// compatibility mode may promote a persistent `01` only after BA had a chance to yield `00`;
// it never creates a BA or hello storm. A raw byte alone never authorizes rendering: the final B0 must TX+
// and `151/1F1 70 -> 74` must complete. The historical `70/B0/B0` fallback is explicitly
// disabled by default, so a measurement run never silently changes wire spelling. The POC powers the glass and sends
// clock 10:00 (`151 05 56 "1000"`). BA is never a periodic keepalive.
//
// THE BLACK BOX: every CAN frame, both directions, is captured from the instant CAN is
// enabled into /can-boot.bin on LittleFS (first history wins; it never wraps), and into a
// small RAM black box for instant inspection.  Download the binary capture from
// /api/capture; /api/boot pages the first 2400 decoded rows.  A full filesystem therefore
// preserves the BEGINNING of the experiment rather than overwriting the handshake that
// explains it.
//
// AND THE WHOLE STORY STREAMS TO SERIAL (USB) AS IT HAPPENS — one line per event, same
// format as /api/boot, interleaved with the goal log. Point any terminal at 115200 and
// pipe it to a file; that file IS the concrete history from power-on to completion.
// Attached the terminal late? PRESS ENTER — the board replays the entire black box from
// row 0, so nothing is ever lost to a missed connect.
//
//   pio run -e ex06_authclock              build
//   pio run -e ex06_authclock -t upload    first flash over USB; thereafter /update
//   pio device monitor -b 115200           watch / capture (any terminal works)
//
// Console: http://<ip>/ — status, the boot black box, the live trace, the log.

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <PsychicHttp.h>
#include <ElegantOTA.h>

#include <driver/twai.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_err.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

constexpr const char* kVersion = "06_authclock 0.6.0 (captured opening; 61 11 01 authorizes; CAN auto-retry)";

// Board wiring — the one thing that is not a "setting". TWAI's config macro takes TX, then RX;
// Corrected bench wiring is CRX -> GPIO3 and CTX -> GPIO4; named constants keep the
// driver's TX-first / RX-second call in the same physical direction.
constexpr gpio_num_t kRxPin   = GPIO_NUM_3;
constexpr gpio_num_t kTxPin   = GPIO_NUM_4;
constexpr uint32_t   kBitrate = 500000;
// With the corrected CTX/CRX routing this proof uses normal CAN mode: TX success requires
// a physical ACK. The recovery path stays non-blocking if the bus is temporarily unavailable.
constexpr bool       kNoAckMode = false;
constexpr bool       kDirectSmokeOnly = false;
// TRUE, AND THE CAPTURE IS WHY. docs/captures/"aknowledge offed display cONNECT OT POWER.csv"
// is a real OEM radio meeting a display that had been powered with no radio present. That
// display repeats `3CF 61 11 01` every ~104 ms and NEVER sends `61 11 00` — not once in
// sixteen requests — yet the radio answers it with the ordinary B0 burst 30.75 ms later and
// the session completes: registration, `03 52`, ISO-TP text, and the `01` never reappears.
// `61 11 01` is the SAME request as `00`; the low bit reports the panel's own state, it is
// not an authorization grade. Waiting for a `00` that a warm display never sends is exactly
// how this bench sat at "badAuth" for fifteen seconds at a time.
constexpr bool       kEnable01Compatibility = true;
// The monitor-captured B0/B0/B0 is the only automatic opening. Keep the historical
// MeganeCAN 70/B0/B0 spelling for a deliberate compatibility experiment, but never inject
// it merely because a registration reply was late or missing.
constexpr bool       kEnableLegacyHelloFallback = false;

// ---------------------------------------------------------------------------
// Protocol constants — each one is a line in docs/PROTOCOL.md
// ---------------------------------------------------------------------------
constexpr uint16_t kSyncTx    = 0x3AF;
constexpr uint16_t kSyncRx    = 0x3CF;               // NOT 3AF|0x400 — hard-coded pair
constexpr uint16_t kReplyFlag = 0x400;
constexpr uint16_t kFuncs[]   = { 0x151, 0x1F1 };    // registration order is on the wire
constexpr uint8_t  kFuncCount = sizeof(kFuncs) / sizeof(kFuncs[0]);
constexpr uint8_t  kRegisterCommand[1] = { 0x70 };

constexpr uint8_t FAILED     = 0x01;                 // initial state
constexpr uint8_t PEER_ALIVE = 0x02;
constexpr uint8_t FUNCSREG   = 0x08;

constexpr uint32_t kTickMs      = 500;    // real monitor: sustained 69/B9 cadence is ~500 ms
constexpr uint32_t kAckMs       = 2000;   // per-frame ACK timeout, no retry
constexpr uint32_t kPeerAliveMs = 5000;   // wall-clock peer watchdog, never a call count
constexpr uint32_t kHelloStepMs = 31;     // monitor: B0-to-B0 30.8..31.2 ms, all scenarios
constexpr uint32_t kHelloRetryMs = 100;   // local outbox retry only, never a hello storm
constexpr uint32_t kStartCompatMs = 110;  // BA -> B0 is 112 ms in the 01-only real capture
constexpr uint32_t kPongMinMs   = 250;    // 126 pings in 32 ms is one ping
constexpr uint32_t kRetryMs     = 1000;   // between registration passes
constexpr uint32_t kClockRetryMs = 5000;  // between clock attempts after a failure
constexpr uint32_t kPostRegisterSettleMs = 400; // monitor: final 5F1/74 -> 03 52 is ~400 ms
constexpr uint32_t kPowerWarmUpMs = 50;   // short post-power settle before a one-frame clock
constexpr uint32_t kBootstrapQueueRetryMs = 100; // local outbox retry only; never a BA keepalive
// A BA that the CONTROLLER could not put on the wire is retried slowly and a bounded number
// of times. The display asks again every ~104 ms on its own timer, so a fast retry buys
// nothing and costs arbitration.
constexpr uint32_t kBootstrapRetryBackoffMs = 500;
constexpr uint8_t  kBootstrapMaxTxFailures  = 3;
constexpr uint32_t kTxOutcomeMs = 500;    // a classic 500 kbit/s frame must settle long before this
constexpr uint32_t kTxRestartRetryMs = 1000; // paced retry if stop/start cannot recover immediately
constexpr uint8_t  kControlAckMaxRetries = 1; // one immediate retry of the required 5C1 74

// The goal: the panel clock at 10:00. `05 56` + four ASCII digits (§5.1).
constexpr char kClockHHMM[5] = "1000";

bool expired(uint32_t now, uint32_t at) { return static_cast<int32_t>(now - at) >= 0; }

// ---------------------------------------------------------------------------
// Log ring — watched over HTTP
// ---------------------------------------------------------------------------
struct LogRec { uint32_t ms = 0; char msg[112] = {0}; };
constexpr size_t kLogRing = 64;
LogRec       g_log[kLogRing];
size_t       g_logHead = 0;
portMUX_TYPE g_logMux  = portMUX_INITIALIZER_UNLOCKED;

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
// THE BLACK BOX — the first 2400 events after power-on, then frozen
// ---------------------------------------------------------------------------
// Kind 0 = RX frame, 1 = TXQ (controller accepted), 2 = controller state, 3 = RX self
// echo, 4 = TX+ completion, 5 = TX- failure, 6 = TX! local refusal. State rows are written
// once a second AND on every state change, so the exact
// moment the counters start moving is in the record even between seconds).
//
// KEEPS THE FIRST N, NOT THE LAST N. A ring holds the newest history; the question this
// buffer answers is the OLDEST — what did the bus look like from the very first frame,
// and what was the last thing that happened before it died. On a polite bus (~5 rows/s)
// it holds ~8 minutes; a line-rate storm fills it in under two seconds, which pins the
// transition inside the record by construction.
struct EventStamp {
  uint32_t ms;
  uint32_t us;       // microsecond offset within ms, always 0..999
};

// esp_timer is monotonic and avoids the 71-minute rollover of Arduino micros().
// We retain the millisecond field for compact, familiar logs and add its precise
// sub-millisecond offset so back-to-back CAN frames are distinguishable.
EventStamp eventStamp() {
  const int64_t nowUs = esp_timer_get_time();
  return {static_cast<uint32_t>(nowUs / 1000), static_cast<uint32_t>(nowUs % 1000)};
}

struct BootRec {
  uint32_t ms;
  uint32_t us;
  uint16_t id;       // frames: CAN id. stat rows: 0
  uint8_t  kind;
  uint8_t  len;      // frames: DLC. stat rows: twai state (0 stop/1 run/2 busoff/3 rec)
  uint8_t  d[8];     // stat rows: [0]=txErr [1]=rxErr, [2..5]=busErr le32, [6..7]=rx/s
};
constexpr uint16_t kBoot = 2400;
BootRec      g_boot[kBoot];
volatile uint16_t g_bootN = 0;             // rows written; == kBoot means frozen
portMUX_TYPE g_bootMux = portMUX_INITIALIZER_UNLOCKED;

void bootPush(uint8_t kind, uint16_t id, uint8_t len, const uint8_t* d) {
  portENTER_CRITICAL(&g_bootMux);
  if (g_bootN < kBoot) {
    const EventStamp when = eventStamp();
    BootRec& r = g_boot[g_bootN];
    r.ms   = when.ms;
    r.us   = when.us;
    r.id   = id;
    r.kind = kind;
    r.len  = len;
    memset(r.d, 0, 8);
    if (d) memcpy(r.d, d, 8);
    ++g_bootN;
  }
  portEXIT_CRITICAL(&g_bootMux);
}

// ---------------------------------------------------------------------------
// Persistent wire capture — all frame events from the start of CAN service
// ---------------------------------------------------------------------------
// The direct TWAI RX task cannot touch LittleFS: it runs at high priority and file I/O there would
// lose frames exactly when a burst is interesting.  It instead copies a compact, fixed
// 20-byte record into this FIFO; loop() drains it to flash in batches.  At 1,500 frames/s
// this consumes about 30 kB/s, while the 1.125 MB partition keeps roughly 37 seconds of
// the COMPLETE beginning of a storm.  When storage fills, it freezes rather than wraps.
// That makes the first handshake inspectable even after a long failed experiment.
struct CaptureHeader {
  char     magic[8];     // "AFFACAN1"
  uint32_t version;
  uint32_t bitrate;
};
struct CaptureRec {
  uint32_t ms;
  uint32_t us;
  uint16_t id;
  uint8_t  kind;         // RX / TXQ / RXE / TX+ / TX- / TX! (see above)
  uint8_t  len;
  uint8_t  d[8];
};
static_assert(sizeof(CaptureRec) == 20, "capture records must stay fixed-size");

constexpr char     kCapturePath[] = "/can-boot.bin";
// The RX task may receive a retransmit storm while flash is busy. Keep several
// seconds of complete raw records so the persistent boot capture remains useful.
constexpr uint16_t kCaptureFifo   = 4096;
CaptureRec         g_captureFifo[kCaptureFifo];
volatile uint16_t  g_captureW = 0;
volatile uint16_t  g_captureR = 0;
uint32_t           g_captureDrop = 0;
uint32_t           g_captureRows = 0;
uint32_t           g_captureBytes = 0;
bool               g_captureOpen = false;
bool               g_captureFull = false;
File               g_capture;
portMUX_TYPE       g_captureMux = portMUX_INITIALIZER_UNLOCKED;

void capturePush(uint8_t kind, uint16_t id, uint8_t len, const uint8_t* d) {
  if (!g_captureOpen || g_captureFull) return;
  portENTER_CRITICAL(&g_captureMux);
  const uint16_t next = static_cast<uint16_t>((g_captureW + 1) % kCaptureFifo);
  if (next == g_captureR) {
    ++g_captureDrop;                 // visible in /api/status; never overwrite history
  } else {
    const EventStamp when = eventStamp();
    CaptureRec& r = g_captureFifo[g_captureW];
    r.ms = when.ms;
    r.us = when.us;
    r.id = id;
    r.kind = kind;
    r.len = len > 8 ? 8 : len;
    memset(r.d, 0, sizeof(r.d));
    if (d) memcpy(r.d, d, r.len);
    g_captureW = next;
  }
  portEXIT_CRITICAL(&g_captureMux);
}

bool capturePop(CaptureRec& out) {
  bool got = false;
  portENTER_CRITICAL(&g_captureMux);
  if (g_captureR != g_captureW) {
    out = g_captureFifo[g_captureR];
    g_captureR = static_cast<uint16_t>((g_captureR + 1) % kCaptureFifo);
    got = true;
  }
  portEXIT_CRITICAL(&g_captureMux);
  return got;
}

// The pinned ESP32_CAN wrapper hides ESP-IDF transmit failures and leaves message flags
// uninitialised. Own the small TWAI transport here so this POC records controller results,
// with the self-reception flag explicitly cleared for every outbound frame.
void onCanFrame(const twai_message_t& f);

bool         g_canReady = false;
bool         g_canRecovering = false;
TaskHandle_t g_twaiRxTask = nullptr;
uint32_t     g_busOffs = 0, g_busRecoveries = 0, g_busRecoveryFailures = 0;

void twaiRxTask(void*) {
  for (;;) {
    twai_message_t f{};
    const esp_err_t err = twai_receive(&f, pdMS_TO_TICKS(20));
    if (err == ESP_OK) {
      onCanFrame(f);
    } else if (err != ESP_ERR_TIMEOUT) {
      // Recovery temporarily stops the driver. Never spin a high-priority RX task while
      // it is stopped or while a transient driver error is being reported.
      vTaskDelay(pdMS_TO_TICKS(1));
    }
  }
}

bool startCan() {
  if (kDirectSmokeOnly) {
    Serial.println("[can] direct-TWAI boot smoke: controller intentionally not installed");
    return true;
  }
  twai_general_config_t general =
      TWAI_GENERAL_CONFIG_DEFAULT(kTxPin, kRxPin,
                                  kNoAckMode ? TWAI_MODE_NO_ACK : TWAI_MODE_NORMAL);
  general.rx_queue_len = 256;
  // One software-owned frame is in flight. A zero driver queue makes transmit(, 0)
  // genuinely nonblocking and makes every completion alert unambiguous.
  general.tx_queue_len = 0;
  general.alerts_enabled = TWAI_ALERT_TX_SUCCESS | TWAI_ALERT_TX_FAILED |
                           TWAI_ALERT_TX_IDLE | TWAI_ALERT_BUS_ERROR |
                           TWAI_ALERT_BUS_OFF | TWAI_ALERT_RECOVERY_IN_PROGRESS |
                           TWAI_ALERT_BUS_RECOVERED | TWAI_ALERT_ERR_PASS |
                           TWAI_ALERT_ERR_ACTIVE | TWAI_ALERT_RX_QUEUE_FULL |
                           TWAI_ALERT_RX_FIFO_OVERRUN | TWAI_ALERT_ARB_LOST;
  const twai_timing_config_t timing = TWAI_TIMING_CONFIG_500KBITS();
  const twai_filter_config_t filter = TWAI_FILTER_CONFIG_ACCEPT_ALL();

  const esp_err_t installed = twai_driver_install(&general, &timing, &filter);
  if (installed != ESP_OK) {
    Serial.printf("[can] twai_driver_install failed: %s\n", esp_err_to_name(installed));
    return false;
  }
  const esp_err_t started = twai_start();
  if (started != ESP_OK) {
    Serial.printf("[can] twai_start failed: %s\n", esp_err_to_name(started));
    twai_driver_uninstall();
    return false;
  }
  if (xTaskCreate(&twaiRxTask, "affa-can-rx", 4096, nullptr, 19, &g_twaiRxTask) != pdPASS) {
    Serial.println("[can] RX task creation failed");
    twai_stop();
    twai_driver_uninstall();
    return false;
  }
  g_canReady = true;
  return true;
}

// Defined after the session FSM: alert handling needs to invalidate a stale session and
// dispatch the per-frame completion hook.
void pumpCanAlerts();
void pumpTx();

void startCapture() {
  // Formatting only happens before CAN is enabled.  It cannot steal time from the capture
  // itself, and a new POC boot gets a fresh, unambiguous record.
  // The custom OTA table names the data partition `littlefs`; Arduino's default label is
  // `spiffs`, so spell the label out instead of silently falling back to RAM-only logging.
  if (!LittleFS.begin(true, "/littlefs", 10, "littlefs")) {
    Serial.println("[capture] LittleFS mount failed; USB stream + RAM black box remain active");
    return;
  }
  if (LittleFS.exists(kCapturePath)) LittleFS.remove(kCapturePath);
  g_capture = LittleFS.open(kCapturePath, FILE_WRITE);
  if (!g_capture) {
    Serial.println("[capture] cannot open /can-boot.bin; USB stream + RAM black box remain active");
    return;
  }
  const CaptureHeader h{{'A', 'F', 'F', 'A', 'C', 'A', 'N', '1'}, 3, kBitrate};
  if (g_capture.write(reinterpret_cast<const uint8_t*>(&h), sizeof(h)) != sizeof(h)) {
    g_capture.close();
    Serial.println("[capture] cannot write header; persistent capture disabled");
    return;
  }
  g_capture.flush();
  g_captureBytes = sizeof(h);
  g_captureOpen = true;
  Serial.println("[capture] /can-boot.bin armed before CAN starts (GET /api/capture)");
}

void pumpCapture() {
  if (!g_captureOpen || g_captureFull) return;
  CaptureRec batch[64];
  size_t n = 0;
  while (n < (sizeof(batch) / sizeof(batch[0])) && capturePop(batch[n])) ++n;
  if (!n) return;

  const size_t bytes = n * sizeof(batch[0]);
  const size_t wrote = g_capture.write(reinterpret_cast<const uint8_t*>(batch), bytes);
  g_captureBytes += static_cast<uint32_t>(wrote);
  g_captureRows += static_cast<uint32_t>(wrote / sizeof(batch[0]));
  if (wrote != bytes) {
    g_capture.flush();
    g_capture.close();
    g_captureFull = true;
    Serial.println("[capture] filesystem full or write failed; beginning of trace preserved");
    return;
  }
  // Flash writes are batched, but a download after a test should see everything up to at
  // most this small interval. The flush is intentionally outside the RX task.
  if ((g_captureRows & 0x7Fu) == 0) g_capture.flush();
}

// ---------------------------------------------------------------------------
// Live coalesced trace — the ongoing view once the black box is full
// ---------------------------------------------------------------------------
struct FrameRec {
  uint32_t firstMs = 0, lastMs = 0, count = 0;
  uint16_t id = 0;
  uint8_t  dir = 0, len = 0;
  uint8_t  d[8] = {0};
};
constexpr uint8_t kFrameRing = 48;
FrameRec     g_frames[kFrameRing];
uint8_t      g_frameHead = 0;
portMUX_TYPE g_frameMux  = portMUX_INITIALIZER_UNLOCKED;

void tap(uint16_t id, const uint8_t* d, uint8_t len, bool tx) {
  const uint32_t ms = ::millis();
  portENTER_CRITICAL(&g_frameMux);
  FrameRec& last = g_frames[(g_frameHead + kFrameRing - 1) % kFrameRing];
  const bool same = last.count && last.dir == (tx ? 1 : 0) && last.id == id &&
                    last.len == len && memcmp(last.d, d, 8) == 0;
  if (same) {
    ++last.count;
    last.lastMs = ms;
  } else {
    FrameRec& r = g_frames[g_frameHead];
    r.firstMs = ms; r.lastMs = ms; r.count = 1;
    r.id = id; r.dir = tx ? 1 : 0; r.len = len;
    memcpy(r.d, d, 8);
    g_frameHead = static_cast<uint8_t>((g_frameHead + 1) % kFrameRing);
  }
  portEXIT_CRITICAL(&g_frameMux);
}

// ---------------------------------------------------------------------------
// RX FIFO — the direct RX task copies frames; the protocol runs in loop()
// ---------------------------------------------------------------------------
struct RxF {
  uint16_t id;
  uint8_t  len;
  uint8_t  d[8];
  // Captured in the RX task so the reply matcher can reject an old FIFO reply while
  // still accepting a panel reply that physically arrives before loop() observes TX+.
  int64_t  rxUs;
};
constexpr size_t kFifo = 512;
RxF          g_fifo[kFifo];
volatile size_t g_fifoW = 0, g_fifoR = 0;
uint32_t     g_fifoDrop = 0;
portMUX_TYPE g_fifoMux  = portMUX_INITIALIZER_UNLOCKED;

uint32_t g_rxCount = 0, g_txScheduled = 0, g_txCount = 0, g_txRefused = 0;
uint32_t g_txBusy = 0, g_selfEchoes = 0;
uint32_t g_txSuccessAlerts = 0, g_txFailedAlerts = 0, g_twaiAlerts = 0;
uint32_t g_txOutcomeTimeouts = 0, g_txRestarts = 0, g_txRestartFailures = 0;
uint32_t g_lastRxMs = 0;

// A physical/controller self echo can still be received by the direct RX task on this
// C3/transceiver combination. Those frames are useful evidence about the wire,
// but they are NOT panel traffic: treating a self-echoed 151 70 as inbound causes us to
// manufacture our own 551 74 and falsely declare registration successful. Keep a small,
// timestamped fingerprint table so the protocol consumes only frames from the panel.
struct SelfEcho {
  uint32_t ms = 0;
  uint16_t id = 0;
  uint8_t  len = 0;
  uint8_t  d[8] = {0};
  bool     pending = false;
};
constexpr uint8_t  kSelfEchoSlots = 24;
constexpr uint32_t kSelfEchoMs = 80;
SelfEcho      g_selfEcho[kSelfEchoSlots];
uint8_t       g_selfEchoNext = 0;
portMUX_TYPE  g_selfEchoMux = portMUX_INITIALIZER_UNLOCKED;

uint8_t noteSelfTx(uint16_t id, const uint8_t* d, uint8_t len) {
  portENTER_CRITICAL(&g_selfEchoMux);
  const uint8_t slot = g_selfEchoNext;
  SelfEcho& e = g_selfEcho[slot];
  e.ms = ::millis();
  e.id = id;
  e.len = len;
  memcpy(e.d, d, 8);
  e.pending = true;
  g_selfEchoNext = static_cast<uint8_t>((g_selfEchoNext + 1) % kSelfEchoSlots);
  portEXIT_CRITICAL(&g_selfEchoMux);
  return slot;
}

void cancelSelfTx(uint8_t slot) {
  portENTER_CRITICAL(&g_selfEchoMux);
  g_selfEcho[slot].pending = false;
  portEXIT_CRITICAL(&g_selfEchoMux);
}

bool takeSelfEcho(uint16_t id, const uint8_t* d, uint8_t len) {
  const uint32_t now = ::millis();
  bool match = false;
  portENTER_CRITICAL(&g_selfEchoMux);
  for (uint8_t i = 0; i < kSelfEchoSlots; ++i) {
    SelfEcho& e = g_selfEcho[i];
    if (!e.pending) continue;
    if (now - e.ms > kSelfEchoMs) { e.pending = false; continue; }
    if (e.id != id || e.len != len || memcmp(e.d, d, 8) != 0) continue;
    e.pending = false;
    match = true;
    break;
  }
  portEXIT_CRITICAL(&g_selfEchoMux);
  return match;
}

void onCanFrame(const twai_message_t& f) {
  if (f.extd || f.rtr) return;                 // 11-bit data frames only
  const uint8_t len = f.data_length_code > 8 ? 8 : f.data_length_code;
  const uint16_t id = static_cast<uint16_t>(f.identifier);
  const int64_t rxUs = esp_timer_get_time();

  // Preserve even a physical self echo in the black box. It is not panel traffic and
  // must never manufacture our own reply, but hiding it would violate the capture claim.
  if (takeSelfEcho(id, f.data, len)) {
    ++g_selfEchoes;
    bootPush(3, id, len, f.data);       // RXE, distinct from real panel RX
    capturePush(2, id, len, f.data);    // RXE, preserved in capture format v3
    tap(id, f.data, len, /*tx=*/false);
    return;
  }

  bootPush(0, id, len, f.data);
  capturePush(0, id, len, f.data);
  tap(id, f.data, len, /*tx=*/false);

  portENTER_CRITICAL(&g_fifoMux);
  ++g_rxCount;
  g_lastRxMs = ::millis();
  const size_t next = (g_fifoW + 1) % kFifo;
  if (next == g_fifoR) {
    ++g_fifoDrop;
  } else {
    RxF& r = g_fifo[g_fifoW];
    r.id  = id;
    r.len = len;
    memset(r.d, 0, 8);
    memcpy(r.d, f.data, len);
    r.rxUs = rxUs;
    g_fifoW = next;
  }
  portEXIT_CRITICAL(&g_fifoMux);
}

bool fifoPop(RxF& out) {
  bool got = false;
  portENTER_CRITICAL(&g_fifoMux);
  if (g_fifoR != g_fifoW) {
    out = g_fifo[g_fifoR];
    g_fifoR = (g_fifoR + 1) % kFifo;
    got = true;
  }
  portEXIT_CRITICAL(&g_fifoMux);
  return got;
}

// ---------------------------------------------------------------------------
// TX — DLC always 8, padded with our filler 0x00 (§1, §1.1)
// ---------------------------------------------------------------------------
bool g_txGate = true;                        // shut only during an OTA write

enum class TxAfter : uint8_t { None, Probe, ControlAck, HelloStep, HelloComplete, GoalReply };

struct TxItem {
  uint16_t id = 0;
  uint8_t  d[8] = {0};
  TxAfter  after = TxAfter::None;
  uint32_t completionToken = 0;              // auth epoch or goal token
  uint32_t sessionEpoch = 0;                 // 0: sync frame; nonzero: app frame
  uint8_t  retryCount = 0;                   // bounded retry count for control-plane ACKs
};

// One bounded, software-owned outbox means an alert can always be attributed to exactly
// one frame. Only loop() touches it; the RX task owns neither TX nor protocol state.
constexpr uint8_t kTxOutbox = 48;
TxItem            g_txOutbox[kTxOutbox];
uint8_t           g_txOutW = 0, g_txOutR = 0;
// Control-plane ACKs are allowed to overtake ordinary heartbeat/hello work. In the real
// trace `1C1 70 -> 5C1 74` lands between B0#1 and B0#2; keeping a tiny separate lane makes
// that invariant survive an otherwise busy normal outbox.
constexpr uint8_t kCtrlOutbox = 8;
TxItem            g_ctrlOutbox[kCtrlOutbox];
uint8_t           g_ctrlOutW = 0, g_ctrlOutR = 0;
TxItem            g_txCurrent;
bool              g_txInFlight = false;
bool              g_txCurrentIsControl = false;
uint8_t           g_txCurrentSelfSlot = 0;
uint32_t          g_txInFlightSince = 0;
bool              g_txRestartPending = false;
uint32_t          g_nextTxRestartMs = 0;

uint8_t txOutboxUsed() {
  return static_cast<uint8_t>((g_txOutW + kTxOutbox - g_txOutR) % kTxOutbox);
}

uint8_t txOutboxFree() { return static_cast<uint8_t>(kTxOutbox - 1 - txOutboxUsed()); }

uint8_t ctrlOutboxUsed() {
  return static_cast<uint8_t>((g_ctrlOutW + kCtrlOutbox - g_ctrlOutR) % kCtrlOutbox);
}

bool enqueueTx(uint16_t id, const uint8_t* d, uint8_t n, TxAfter after = TxAfter::None,
                uint32_t completionToken = 0, uint32_t sessionEpoch = 0,
                bool control = false, uint8_t retryCount = 0) {
  if (!g_txGate || !g_canReady) { ++g_txRefused; return false; }
  const uint8_t cap = control ? kCtrlOutbox : kTxOutbox;
  uint8_t& write = control ? g_ctrlOutW : g_txOutW;
  const uint8_t read = control ? g_ctrlOutR : g_txOutR;
  const uint8_t next = static_cast<uint8_t>((write + 1) % cap);
  if (next == read) { ++g_txRefused; return false; }

  TxItem& item = control ? g_ctrlOutbox[write] : g_txOutbox[write];
  item.id = id;
  memset(item.d, 0, sizeof(item.d));
  if (d) memcpy(item.d, d, n > 8 ? 8 : n);
  item.after = after;
  item.completionToken = completionToken;
  item.sessionEpoch = sessionEpoch;
  item.retryCount = retryCount;
  write = next;
  ++g_txScheduled;                           // scheduled, not yet in the controller
  return true;
}

bool sendRaw(uint16_t id, const uint8_t* d, uint8_t n) { return enqueueTx(id, d, n); }

bool sendRawAfter(uint16_t id, const uint8_t* d, uint8_t n, TxAfter after,
                  uint32_t completionToken, uint32_t sessionEpoch = 0) {
  return enqueueTx(id, d, n, after, completionToken, sessionEpoch);
}

bool send1(uint16_t id, uint8_t b0) { return sendRaw(id, &b0, 1); }

// ---------------------------------------------------------------------------
// Session state
// ---------------------------------------------------------------------------
uint8_t  g_sync = FAILED;
uint32_t g_lastPingMs = 0, g_lastPongMs = 0, g_nextHelloMs = 0, g_nextTickMs = 0;
uint32_t g_lastPeerMs = 0;                 // accepted full 61 / 69, once the opening starts
uint32_t g_authEpoch = 0;                  // invalidates queued application work on re-opening
uint32_t g_helloAuthEpochPending = 0;      // epoch whose final opening frame may release output
bool     g_panelSeen = false;             // diagnostic: any panel sync traffic was observed
bool     g_syncRequestSeen = false;       // a FULL `61 11 xx` request was observed
bool     g_authRequestSeen = false;       // accepted 00, or 01 only with explicit compatibility enabled
bool     g_authHelloPending = false;      // no application output until the final opening frame TX+
bool     g_helloPending = false;          // exactly one staged opening is waiting to begin
bool     g_helloActive = false;           // one opening frame queued/in flight; wait for TX outcome
uint8_t  g_helloIndex = 0;
enum class HelloStyle : uint8_t { CapturedB0x3, Legacy70B0B0 };
HelloStyle g_helloStyle = HelloStyle::CapturedB0x3;
bool     g_legacyFallbackUsed = false;    // at most one legacy greeting per opening epoch
bool     g_start01Seen = false;            // display's "radio, where are you?" request
bool     g_syncProbePending = false;       // one BA probe waiting for a normal TX slot
bool     g_syncProbeAwaitingTx = false;    // BA is queued/in flight; do not send a duplicate
bool     g_syncProbeIssued = false;        // BA has physically reported TX+
uint8_t  g_probeTxFailures = 0;            // bounded BA TX- budget, reset on a fresh session
uint32_t g_probeGeneration = 0;            // rejects a delayed BA result after a new session
uint32_t g_nextBootstrapQueueMs = 0;       // paced local scheduling retry; never a periodic BA
uint32_t g_startCompatAtMs = 0;            // let BA yield 00 before accepting persistent 01
uint32_t g_bootstrapQueueDeferrals = 0;

uint32_t g_pings = 0, g_pongs = 0, g_syncReqs = 0, g_badAuths = 0, g_hellos = 0;
uint32_t g_bootstraps = 0, g_autoAcks = 0, g_autoAckRetries = 0, g_autoAckFailures = 0;
uint32_t g_keys = 0, g_replyStray = 0;
uint32_t g_regOk = 0, g_regFail = 0, g_powerOkCnt = 0, g_powerFail = 0;
uint32_t g_clockOkCnt = 0, g_clockFail = 0, g_syncLosses = 0;

// Control replies are valid before the application output gate opens, but they still carry
// the current auth epoch so a TX- cannot retry an ACK from a previous panel incarnation.
bool sendControl1(uint16_t id, uint8_t b0, uint8_t retryCount = 0) {
  return enqueueTx(id, &b0, 1, TxAfter::ControlAck, g_authEpoch, 0, true, retryCount);
}

// ---------------------------------------------------------------------------
// Stop-and-wait: one frame in flight, ever (§4.2)
// ---------------------------------------------------------------------------
struct Wait {
  bool     active = false;
  bool     txConfirmed = false;             // TX+ starts the timeout, not TXQ
  uint16_t replyId = 0;
  int64_t  acceptedAtUs = 0;                // TXQ fence against an old FIFO reply
  uint32_t since = 0;
  bool     got = false;
  uint8_t  code = 0;
};
Wait g_wait;

void reserveWait(uint16_t funcId, int64_t acceptedAtUs) {
  g_wait.active  = true;
  g_wait.txConfirmed = false;
  g_wait.replyId = funcId | kReplyFlag;
  g_wait.acceptedAtUs = acceptedAtUs;
  g_wait.since   = 0;
  g_wait.got     = false;
  g_wait.code    = 0;
}

void confirmWait(uint16_t funcId, uint32_t now) {
  if (!g_wait.active || g_wait.replyId != (funcId | kReplyFlag)) return;
  g_wait.txConfirmed = true;
  g_wait.since = now;
}

// ---------------------------------------------------------------------------
// The goal: authorization, display power, then a visible clock at 10:00
// ---------------------------------------------------------------------------
enum class Goal : uint8_t {
  WaitSync, Register, SettleBeforePower, PowerOn, WarmUp, SetClock, Done
};

const char* goalName(Goal g) {
  switch (g) {
    case Goal::WaitSync: return "wait-sync   no accepted full 3CF 61 11 opening yet";
    case Goal::Register: return "register    70 -> 74 on each func id, sequential";
    case Goal::SettleBeforePower: return "settle      400 ms after registration before display ON";
    case Goal::PowerOn:  return "power-on    03 52 09 -> 74 before visible proof";
    case Goal::WarmUp:   return "warm-up     display ON acknowledged; waiting 50 ms";
    case Goal::SetClock: return "set-clock   05 56 '1000' in flight";
    case Goal::Done:     return "DONE        clock command 10:00 acknowledged";
  }
  return "?";
}

Goal     g_goal = Goal::WaitSync;
uint8_t  g_regIdx = 0;
uint32_t g_retryAt = 0;
uint32_t g_doneAtMs = 0;
uint32_t g_goalToken = 1;                  // rejects stale queued registration/power/clock frames
bool     g_goalTxPending = false;
bool     g_goalTxFailed = false;

void goalReset(Goal to, const char* why) {
  ++g_goalToken;
  if (g_goalToken == 0) ++g_goalToken;     // reserve zero for untagged sync traffic
  g_goal    = to;
  g_regIdx  = 0;
  g_retryAt = ::millis();
  g_wait = Wait{};
  g_goalTxPending = false;
  g_goalTxFailed = false;
  logmsg("goal -> %.9s: %s", goalName(to), why);
}

// ---------------------------------------------------------------------------
// RX handling
// ---------------------------------------------------------------------------
bool sendAlive(uint32_t now) {
  // B9 is the radio half of the observed 500 ms liveness pair. It is safe before a full
  // opening request: `69` is the display's proof it is awake, while BA is the one-shot
  // request that asks it to enter the 61/B0 registration exchange.
  const bool queued = send1(kSyncTx, 0xB9);
  if (queued) g_lastPongMs = now;
  return queued;
}

bool appSessionReady() {
  return g_authRequestSeen && !g_authHelloPending && !(g_sync & FAILED);
}

const char* helloStyleName(HelloStyle s) {
  return s == HelloStyle::CapturedB0x3 ? "captured B0/B0/B0" : "legacy 70/B0/B0";
}

void authorizeAfterHello(uint32_t epoch) {
  if (!epoch || epoch != g_authEpoch || !g_authRequestSeen || !g_authHelloPending) return;

  g_authHelloPending = false;
  const bool wasFailed = (g_sync & FAILED) != 0;
  g_sync = static_cast<uint8_t>(g_sync & ~FAILED);
  if (wasFailed)
    logmsg("%s final opening frame TX+ - session declared up", helloStyleName(g_helloStyle));

  // A panel that re-authenticates after DONE has been reborn: re-power it before the
  // visible clock proof. If registration vanished silently, WaitSync routes through the
  // normal registration pass first.
  if (g_goal == Goal::Done) goalReset(Goal::PowerOn, "panel re-synced after DONE");
}

// The supplied monitor captures have B0#1, the panel's 1C1 registration, its 5C1 ACK,
// then B0#2 and B0#3.  Do not prequeue a whole greeting: this state machine leaves a real
// slot between fragments, so a control ACK cannot be trapped behind the rest of the hello.
constexpr uint8_t kCapturedHello[3][8] = {
    {0xB0, 0x14, 0x11, 0x00, 0x1F, 0x00, 0x00, 0x00},
    {0xB0, 0x14, 0x11, 0x00, 0x1F, 0x00, 0x00, 0x00},
    {0xB0, 0x14, 0x11, 0x00, 0x1F, 0x00, 0x00, 0x00},
};
// This is not the primary path. It preserves the historical MeganeCAN source variant and
// is attempted once only if the captured variant reaches registration but receives no 74.
constexpr uint8_t kLegacyHello[3][8] = {
    {0x70, 0x1A, 0x11, 0x00, 0x00, 0x00, 0x00, 0x01},
    {0xB0, 0x14, 0x11, 0x00, 0x1F, 0x00, 0x00, 0x00},
    {0xB0, 0x14, 0x11, 0x00, 0x1F, 0x00, 0x00, 0x00},
};

const uint8_t* helloFrame(uint8_t index) {
  return g_helloStyle == HelloStyle::CapturedB0x3 ? kCapturedHello[index] : kLegacyHello[index];
}

void scheduleHello(uint32_t now, uint32_t authEpoch, HelloStyle style,
                   uint32_t firstDelayMs = 0) {
  g_helloStyle = style;
  g_helloAuthEpochPending = authEpoch;
  g_helloIndex = 0;
  g_helloActive = false;
  g_helloPending = true;
  g_nextHelloMs = now + firstDelayMs;
}

void pumpHello(uint32_t now) {
  // THE DISPLAY'S REQUEST IS THE TRIGGER, NOT OUR BA. Requiring g_syncProbeIssued here was a
  // deadlock: pumpHello waited for a BA TX+, and while that BA kept failing the bootstrap
  // retried it at 10 Hz, so the bus filled with BA frames and the opening it was gating never
  // ran. BA is only our QUESTION — a display already sending a complete `61 11 xx` every
  // ~104 ms has asked us first, and the captures answer that request with B0 regardless of
  // how our own probe fared.
  if (!g_helloPending || g_helloActive || !expired(now, g_nextHelloMs)) {
    return;
  }
  const bool last = g_helloIndex + 1u == 3u;
  const TxAfter after = last ? TxAfter::HelloComplete : TxAfter::HelloStep;
  const uint32_t token = g_helloAuthEpochPending;
  // Tag every opening fragment with its auth epoch. A new `01` / watchdog / bus recovery
  // can then discard stale B0 traffic before it reaches the controller.
  if (!sendRawAfter(kSyncTx, helloFrame(g_helloIndex), 8, after, token, token)) {
    g_nextHelloMs = now + kHelloRetryMs;
    return;
  }
  // Do not queue B0#2 merely because 31 ms elapsed: first establish that B0#1 actually
  // left the controller. This makes a TX- restart the whole bounded opening cleanly.
  g_helloActive = true;
}

void invalidateAuthorization(const char* why) {
  // App work and control ACKs waiting from the old incarnation must not leak through a
  // fresh `61 11 01`. An already in-flight CAN frame cannot be recalled; its epoch/ticket
  // makes its eventual result harmless.
  g_txOutR = g_txOutW;
  g_ctrlOutR = g_ctrlOutW;
  ++g_authEpoch;
  if (g_authEpoch == 0) ++g_authEpoch;
  g_sync = FAILED;                           // assignment drops PEER_ALIVE/FUNCSREG too
  g_authRequestSeen = false;
  g_authHelloPending = false;
  g_helloAuthEpochPending = 0;
  g_helloPending = false;
  g_helloActive = false;
  g_helloIndex = 0;
  g_start01Seen = false;
  g_syncProbePending = false;
  g_syncProbeAwaitingTx = false;
  g_syncProbeIssued = false;
  g_probeTxFailures = 0;
  ++g_probeGeneration;
  if (g_probeGeneration == 0) ++g_probeGeneration;
  g_startCompatAtMs = 0;
  g_lastPeerMs = 0;
  g_wait = Wait{};                            // never complete a transaction from old auth
  goalReset(Goal::WaitSync, why);
}

void sendBootstrapControl(uint32_t now) {
  // BA is the radio's one-shot "I am here / begin discovery" probe. The display's 61
  // response is its answer. It is deliberately not tied to a periodic FAILED tick.
  if (!g_syncProbePending || g_syncProbeAwaitingTx || g_syncProbeIssued ||
      !expired(now, g_nextBootstrapQueueMs)) {
    return;
  }
  if (!g_txGate || !g_canReady || g_canRecovering || txOutboxFree() < 1) {
    g_nextBootstrapQueueMs = now + kBootstrapQueueRetryMs;
    ++g_bootstrapQueueDeferrals;
    if (g_bootstrapQueueDeferrals == 1 || (g_bootstrapQueueDeferrals % 25u) == 0) {
      logmsg("BA probe local queue busy; retrying scheduling in %lums",
             static_cast<unsigned long>(kBootstrapQueueRetryMs));
    }
    return;
  }

  const uint8_t ba = 0xBA;
  if (enqueueTx(kSyncTx, &ba, 1, TxAfter::Probe, g_probeGeneration)) {
    g_syncProbePending = false;
    g_syncProbeAwaitingTx = true;
    g_nextBootstrapQueueMs = 0;
    logmsg("BA probe TXQ pending; waiting for TX+ before opening an auth path");
    return;
  }

  g_nextBootstrapQueueMs = now + kBootstrapQueueRetryMs;
  ++g_bootstrapQueueDeferrals;
  logmsg("BA probe local scheduling failed; deferred (no BA sent)");
}

void requestSyncProbe(uint32_t now, const char* why) {
  if (g_syncProbeIssued || g_syncProbePending || g_syncProbeAwaitingTx) return;
  ++g_probeGeneration;
  if (g_probeGeneration == 0) ++g_probeGeneration;
  g_syncProbePending = true;
  g_nextBootstrapQueueMs = now;
  g_bootstrapQueueDeferrals = 0;
  logmsg("%s -> BA probe armed", why);
}

void startCapabilityOpening(uint32_t now, bool compatibility01) {
  if (g_authHelloPending) return;          // repeated 61 traffic is one opening, not a storm
  ++g_authEpoch;
  if (g_authEpoch == 0) ++g_authEpoch;
  g_sync = FAILED;                          // every capability pass must re-register 151/1F1
  g_authRequestSeen = true;
  g_authHelloPending = true;
  g_lastPeerMs = now ? now : 1;
  g_nextTickMs = now + kTickMs;
  g_legacyFallbackUsed = false;
  // Every captured good-00 session waits about 31 ms before B0#1. The 01 compatibility
  // route has already waited BA+110 ms, so it intentionally starts its first B0 promptly.
  scheduleHello(now, g_authEpoch, HelloStyle::CapturedB0x3,
                compatibility01 ? 0 : kHelloStepMs);
  ++g_syncReqs;
  goalReset(Goal::WaitSync, compatibility01 ?
      "persistent 61 11 01 after BA; running captured compatibility opening" :
      "display confirmed 61 11 00; running captured opening");
  logmsg("%s -> staged B0/B0/B0; output waits for final B0 TX+ and 74 registrations",
         compatibility01 ? "61 11 01 compatibility" : "61 11 00");
}

bool startLegacyHelloFallback(uint32_t now, const char* why) {
  if (!kEnableLegacyHelloFallback || g_legacyFallbackUsed ||
      g_helloStyle != HelloStyle::CapturedB0x3 ||
      !g_authRequestSeen || !g_syncProbeIssued) {
    return false;
  }
  // The monitor profile gets first refusal. This one compatibility retry preserves the
  // historical MeganeCAN 70/B0/B0 variant without turning either form into periodic noise.
  g_legacyFallbackUsed = true;
  ++g_authEpoch;
  if (g_authEpoch == 0) ++g_authEpoch;
  g_sync = FAILED;
  g_authHelloPending = true;
  scheduleHello(now, g_authEpoch, HelloStyle::Legacy70B0B0);
  goalReset(Goal::WaitSync, why);
  logmsg("captured opening got no registration ACK -> one legacy 70/B0/B0 fallback");
  return true;
}

void handleSyncRx(const RxF& f, uint32_t now) {
  if (f.len >= 2 && f.d[0] == 0x61 && f.d[1] == 0x11) {
    g_panelSeen = true;

    // A Carminat request is three bytes. Do not inspect absent data[2], and do not let a
    // truncated 61 11 manufacture either hello traffic or authorization.
    if (f.len < 3) {
      ++g_badAuths;
      if (g_badAuths == 1 || (g_badAuths % 100u) == 0)
        logmsg("short auth 61 11 (DLC %u) ignored - waiting for full 61 11 xx", f.len);
      return;
    }

    // A complete `61 11 xx` opens the inbound control plane. `01` is the display asking
    // for a radio; BA is our request for its `00` confirmation. The supplied 01-only trace
    // is available only through the explicit compatibility flag after BA has had time to
    // yield 00.
    g_syncRequestSeen = true;
    const uint8_t auth = f.d[2];
    if (auth == 0x01) {
      // A persistent `01`-only display is still alive. Keep the wall-clock watchdog from
      // tearing down its one bounded compatibility attempt while it continues speaking.
      g_lastPeerMs = now ? now : 1;
      // A fresh 01 after a usable session means the panel forgot registrations. Do not let
      // old 151/1F1/time frames cross that boundary; begin from a new BA probe instead.
      if (appSessionReady()) {
        invalidateAuthorization("fresh display 61 11 01 - registrations are void");
        g_syncRequestSeen = true;
      }
      if (kEnable01Compatibility) g_start01Seen = true;
      requestSyncProbe(now, "display 61 11 01");
      return;
    }

    if (auth == 0x00) {
      // BA -> 61 11 00 is the normal captured discovery confirmation. A duplicate 00
      // merely refreshes liveness; it must not tear down a working registration pass.
      if (g_authRequestSeen || g_authHelloPending) {
        g_lastPeerMs = now ? now : 1;
        return;
      }
      requestSyncProbe(now, "display 61 11 00 without a visible BA");
      startCapabilityOpening(now, /*compatibility01=*/false);
      return;
    }

    {
      ++g_badAuths;
      if (g_badAuths == 1 || (g_badAuths % 100u) == 0)
        logmsg("unknown opening 61 11 %02X - control ACK only, no output", auth);
      return;
    }
  }

  if (f.len >= 1 && f.d[0] == 0x69) {          // ping: d[0] only, DLC may be 1
    ++g_pings;
    g_panelSeen = true;
    g_lastPingMs = now ? now : 1;
    g_lastPeerMs = g_lastPingMs;
    g_nextTickMs = now + kTickMs;

    // `69` is enough to prove the display is awake, so it starts the B9 heartbeat and a
    // single BA discovery probe. It is NOT enough to send B0/151/power/time.
    if (g_authRequestSeen || g_authHelloPending) {
      g_sync |= PEER_ALIVE;
    }
    requestSyncProbe(now, "display 69 liveness");
    // The pong is paced against a retransmitting panel, including before discovery.
    if (now - g_lastPongMs >= kPongMinMs || g_lastPongMs == 0) {
      if (sendAlive(now)) ++g_pongs;
    }
    return;
  }
}

void handleReply(const RxF& f) {
  // Reserve the expected reply at TXQ, not TX+, because a fast panel can physically answer
  // before loop() has drained the controller's TX-success alert. The RX-task timestamp
  // keeps a late FIFO record from a previous command from satisfying this transaction.
  if (g_wait.active && f.id == g_wait.replyId && !g_wait.got &&
      f.rxUs >= g_wait.acceptedAtUs) {
    g_wait.code = f.d[0];
    g_wait.got  = true;
    return;
  }
  ++g_replyStray;                              // consumed silently by design (§2.3)
}

void handleAppFrame(const RxF& f, uint32_t now) {
  (void)now;
  // A full `61 11 xx` opens the panel control-plane reply path independently from our
  // application-output gate. In particular `1C1 70` needs `5C1 74` during bootstrap.
  // This reply stays untagged: an epoch-tagged application command is correctly rejected
  // before the good 00 + hello authorization is complete.
  // Scope control ACKs to the proven Carminat channel. A complete 61 must never grant
  // permission to ACK arbitrary vehicle CAN IDs on a busy shared bus.
  if (!g_syncRequestSeen || f.id != 0x1C1) return;
  // Every frame is individually acknowledged (§4.2): 74 on id|0x400, our filler. This is
  // what lets the panel register its 0x1C1 channel and have its keys answered.
  if (!sendControl1(f.id | kReplyFlag, 0x74)) {
    ++g_autoAckFailures;
    if (g_autoAckFailures <= 3 || (g_autoAckFailures % 25u) == 0) {
      logmsg("panel ACK 0x%03X could not enter control outbox", f.id | kReplyFlag);
    }
  }

  // Key delivery is application-facing and waits for the completed opening. The generic ACK
  // above is protocol control traffic, not an authorization shortcut.
  if (!appSessionReady()) return;

  if (f.d[0] == 0x70) {
    logmsg("panel registered its channel 0x%03X - acked 74 on 0x%03X",
           f.id, f.id | kReplyFlag);
    return;
  }
  if (f.d[0] == 0x03 && f.d[1] == 0x89) {      // keys need 03 89, else not a key (§7)
    const uint16_t raw = static_cast<uint16_t>((f.d[2] << 8) | f.d[3]);
    uint16_t key = raw;
    bool hold = false;
    if (raw != 0x0101 && raw != 0x0141) {      // encoder detents are EXEMPT (§7)
      hold = (f.d[3] & 0xC0) != 0;
      key  = raw & 0xFF3F;
    }
    ++g_keys;
    logmsg("key 0x%04X%s on 0x%03X", key, hold ? " (hold)" : "", f.id);
  }
}

void pumpRx(uint32_t now) {
  RxF f;
  // Never let an input flood monopolize loop(): capture flushing, the watchdog, and
  // deferred handshake work must still run.  Duplicate sync requests are inherently
  // coalesced by the protocol, so sacrificing excess duplicates is safe.
  constexpr uint16_t kRxPerLoop = 96;
  for (uint16_t i = 0; i < kRxPerLoop && fifoPop(f); ++i) {
    if (f.id == kSyncRx)   { handleSyncRx(f, now); continue; }
    if (f.id & kReplyFlag) { handleReply(f);       continue; }
    handleAppFrame(f, now);
  }
}

// ---------------------------------------------------------------------------
// 500 ms liveness + opening + watchdog
// ---------------------------------------------------------------------------
void pumpTick(uint32_t now) {
  // The display speaks first (`69` or `61`). BA is one bounded discovery probe, never a
  // periodic FAILED-state resend.
  if (g_syncProbePending) sendBootstrapControl(now);

  // Normal path: BA -> 61 11 00. The observed 01-only radio path is an explicit opt-in;
  // the strict default keeps `01` in bootstrap and waits for the panel's good `00`.
  if (kEnable01Compatibility && g_start01Seen && !g_authRequestSeen &&
      !g_authHelloPending && g_syncProbeIssued &&
      expired(now, g_startCompatAtMs)) {
    startCapabilityOpening(now, /*compatibility01=*/true);
  }

  pumpHello(now);

  // A B9 already queued as a 69 reply satisfies this 500 ms cadence too; this prevents a
  // ping arriving on a heartbeat boundary from creating two B9 frames.
  if (g_panelSeen && expired(now, g_nextTickMs)) {
    g_nextTickMs = now + kTickMs;
    if (now - g_lastPongMs >= kTickMs || g_lastPongMs == 0) sendAlive(now);
  }

  if (g_lastPeerMs && now - g_lastPeerMs > kPeerAliveMs) {
    // No queued sync/protocol frame from the vanished peer may emerge after recovery. This
    // also removes a BA whose generation was invalidated below before it reaches TWAI.
    g_txOutR = g_txOutW;
    g_ctrlOutR = g_ctrlOutW;
    ++g_authEpoch;
    if (g_authEpoch == 0) ++g_authEpoch;
    g_sync = FAILED;                           // ASSIGNED: drops FUNCSREG too (§3.6)
    g_panelSeen = false;
    g_syncRequestSeen = false;                 // recovery needs fresh liveness / a full 61
    g_authRequestSeen = false;
    g_authHelloPending = false;
    g_helloPending = false;
    g_helloActive = false;
    g_helloIndex = 0;
    g_start01Seen = false;
    g_syncProbePending = false;
    g_syncProbeAwaitingTx = false;
    g_syncProbeIssued = false;
    ++g_probeGeneration;
    if (g_probeGeneration == 0) ++g_probeGeneration;
    g_nextBootstrapQueueMs = 0;
    g_startCompatAtMs = 0;
    g_lastPeerMs = 0;
    ++g_syncLosses;
    logmsg("peer-alive expired - state = FAILED, all opening state dropped");
    goalReset(Goal::WaitSync, "peer-alive watchdog expired");
  }
}

// ---------------------------------------------------------------------------
// The serial streamer — the black box narrated to USB as it happens
// ---------------------------------------------------------------------------
// A CURSOR INTO THE BLACK BOX, drained from loop(). Nothing prints from the direct RX task
// — a blocking write in that priority-19 task would sit on the critical path of every
// frame. loop() prints whatever rows the cursor has not seen yet, bounded per pass so a
// storm cannot starve the protocol pumps. With no host attached the writes are dropped
// (setTxTimeoutMs(0)), costing microseconds. ANY byte received on serial rewinds the
// cursor to 0 and replays the whole record — the recover-a-missed-connect button.
// The ongoing stream starts only after that first operator key, so an unattended USB
// serial endpoint has no path to delay the CAN / recovery loop.
int fmtBootRow(char* out, size_t cap, const BootRec& r);   // defined with the routes

uint16_t g_bootPrinted = 0;
bool     g_bootFullSaid = false;
bool     g_serialNarrating = false;

void pumpSerial() {
  // Serial input is a two-key console: 'r' REARMS the black box (fresh recording from this
  // instant — for when the interesting power-on is about to happen and the box is already
  // full of silent-bus rows), anything else REPLAYS the record from row 0.
  bool replay = false, rearm = false;
  while (Serial.available() > 0) {
    const int c = Serial.read();
    if (c == 'r' || c == 'R') rearm = true; else replay = true;
  }
  if (rearm) {
    portENTER_CRITICAL(&g_bootMux);
    g_bootN = 0;
    portEXIT_CRITICAL(&g_bootMux);
    g_bootPrinted  = 0;
    g_bootFullSaid = false;
    g_serialNarrating = true;
    Serial.printf("\n=== BLACK BOX REARMED at %lu ms - recording starts fresh ===\n",
                  static_cast<unsigned long>(millis()));
  } else if (replay) {
    g_bootPrinted = 0;
    g_serialNarrating = true;
    Serial.printf("\n=== BLACK BOX REPLAY from t=0 (%u rows so far) ===\n",
                  static_cast<unsigned>(g_bootN));
  }

  // Never start a continuous USB trace until an operator explicitly requested it.
  // On the C3 a just-closed CDC host can still look connected; blindly printing in
  // that state used to starve later loop pumps. The RAM/LittleFS records remain complete.
  if (!g_serialNarrating) return;
  if (!Serial) {
    g_serialNarrating = false;
    return;
  }

  // Up to 40 rows per pass: ~8000 rows/s of drain capacity against a 1500/s storm, while
  // one pass stays ~2 ms worst case and the watchdog-fed delay(5) below still runs.
  const uint16_t upto = g_bootN;
  uint8_t printed = 0;
  char line[96];
  while (g_bootPrinted < upto && printed < 40) {
    BootRec rec;
    portENTER_CRITICAL(&g_bootMux);
    rec = g_boot[g_bootPrinted];
    portEXIT_CRITICAL(&g_bootMux);
    fmtBootRow(line, sizeof(line), rec);
    Serial.print(line);
    ++g_bootPrinted;
    ++printed;
  }
  if (g_bootPrinted >= kBoot && !g_bootFullSaid) {
    g_bootFullSaid = true;
    Serial.println("=== BLACK BOX FULL - frozen; the record above is the complete "
                   "history from power-on ===");
  }
}

// ---------------------------------------------------------------------------
// Controller state into the black box: once a second, and on every change
// ---------------------------------------------------------------------------
uint32_t g_nextStatMs = 0;
uint8_t  g_lastDrvState = 0xFF;

void pumpStat(uint32_t now) {
  twai_status_info_t st{};
  const bool valid = twai_get_status_info(&st) == ESP_OK;
  const uint8_t state = valid ? static_cast<uint8_t>(st.state) : 9;

  const bool due     = expired(now, g_nextStatMs);
  const bool changed = state != g_lastDrvState;
  if (!due && !changed) return;
  if (due) g_nextStatMs = now + 1000;
  g_lastDrvState = state;

  uint8_t d[8];
  d[0] = valid ? static_cast<uint8_t>(st.tx_error_counter > 255 ? 255 : st.tx_error_counter) : 0;
  d[1] = valid ? static_cast<uint8_t>(st.rx_error_counter > 255 ? 255 : st.rx_error_counter) : 0;
  const uint32_t be = valid ? st.bus_error_count : 0;
  d[2] = static_cast<uint8_t>(be);
  d[3] = static_cast<uint8_t>(be >> 8);
  d[4] = static_cast<uint8_t>(be >> 16);
  d[5] = static_cast<uint8_t>(be >> 24);
  d[6] = 0;
  d[7] = 0;
  bootPush(2, 0, state, d);
}

// ---------------------------------------------------------------------------
// The goal FSM
// ---------------------------------------------------------------------------
void pumpGoal(uint32_t now) {
  if (g_goalTxFailed) {
    g_goalTxFailed = false;
    if (g_goal == Goal::Register) {
      ++g_regFail;
      g_regIdx = 0;
      g_retryAt = now + kRetryMs;
      logmsg("registration TX- - retrying from index 0");
    } else if (g_goal == Goal::PowerOn) {
      ++g_powerFail;
      g_retryAt = now + kClockRetryMs;
      logmsg("display ON TX- - retrying in %lus",
             static_cast<unsigned long>(kClockRetryMs / 1000));
    } else if (g_goal == Goal::SetClock) {
      ++g_clockFail;
      g_retryAt = now + kClockRetryMs;
      logmsg("clock TX- - retrying in %lus",
             static_cast<unsigned long>(kClockRetryMs / 1000));
    }
    return;
  }

  if (g_wait.active) {
    // A reply may already be buffered (the RX task timestamps independently), but TX+
    // remains the CAN-layer proof that starts this command's ACK timeout.
    if (!g_wait.txConfirmed) return;
    if (g_wait.got) {
      const uint8_t code = g_wait.code;
      g_wait.active = false;

      if (g_goal == Goal::Register) {
        if (code == 0x74) {
          logmsg("func 0x%03X registered", kFuncs[g_regIdx]);
          if (++g_regIdx >= kFuncCount) {
            g_sync |= FUNCSREG;
            ++g_regOk;
            logmsg("FUNCSREG latched - authorization complete");
            goalReset(Goal::SettleBeforePower, "registration complete");
            g_retryAt = now + kPostRegisterSettleMs;
          }
        } else {
          ++g_regFail;
          if (startLegacyHelloFallback(now, "captured registration was explicitly rejected")) return;
          g_regIdx  = 0;                       // any failure aborts the pass (§3.5)
          g_retryAt = now + kRetryMs;
          logmsg("registration rejected (0x%02X) - retrying from index 0", code);
        }
      } else if (g_goal == Goal::PowerOn) {
        if (code == 0x74) {
          ++g_powerOkCnt;
          goalReset(Goal::WarmUp, "display ON acknowledged");
          g_retryAt = now + kPowerWarmUpMs;
        } else {
          ++g_powerFail;
          g_retryAt = now + kClockRetryMs;
          logmsg("display ON rejected (0x%02X) - retrying in %lus", code,
                 static_cast<unsigned long>(kClockRetryMs / 1000));
        }
      } else if (g_goal == Goal::SetClock) {
        // This is a one-frame command.  Only 74 is a terminal ACK; 30 01 00 is a
        // continuation request in the legacy sender and cannot prove the clock changed.
        if (code == 0x74) {
          ++g_clockOkCnt;
          g_goal = Goal::Done;
          g_doneAtMs = now ? now : 1;
          logmsg(">>> clock command acknowledged for %c%c:%c%c; verify glass <<<",
                 kClockHHMM[0], kClockHHMM[1], kClockHHMM[2], kClockHHMM[3]);
        } else {
          ++g_clockFail;
          g_retryAt = now + kClockRetryMs;
          logmsg("clock rejected (0x%02X) - retrying in %lus", code,
                 static_cast<unsigned long>(kClockRetryMs / 1000));
        }
      }
      return;
    }
    if (expired(now, g_wait.since + kAckMs)) {
      g_wait.active = false;
      if (g_goal == Goal::Register) {
        ++g_regFail;
        const uint16_t func = kFuncs[g_regIdx];
        if (startLegacyHelloFallback(now, "captured registration timed out")) return;
        g_regIdx  = 0;
        g_retryAt = now + kRetryMs;
        logmsg("no 74 within %lums for func 0x%03X - pass aborted",
               static_cast<unsigned long>(kAckMs), func);
      } else if (g_goal == Goal::PowerOn) {
        ++g_powerFail;
        // A command-channel timeout means the panel may have forgotten our function
        // registration. Restart it rather than blindly queuing display writes forever.
        g_sync = static_cast<uint8_t>(g_sync & ~FUNCSREG);
        goalReset(Goal::Register, "display ON unACKed - assuming registration lost");
      } else if (g_goal == Goal::SetClock) {
        ++g_clockFail;
        // A clock the panel never ACKs means the registration is probably gone even
        // though nobody said so. Re-run it — the factory recovery (§3.5).
        g_sync = static_cast<uint8_t>(g_sync & ~FUNCSREG);
        goalReset(Goal::Register, "clock unACKed - assuming registration lost");
      }
    }
    return;
  }

  // A registration/power/clock frame was accepted by the software outbox but has not yet
  // received TX+ or TX-. Its reply timeout deliberately has not started yet.
  if (g_goalTxPending) return;

  switch (g_goal) {
    case Goal::WaitSync:
      if (!appSessionReady()) break;
      goalReset((g_sync & FUNCSREG) ? Goal::SettleBeforePower : Goal::Register, "sync is up");
      break;

    case Goal::Register:
      if (!appSessionReady()) break;
      if (g_sync & FUNCSREG) {
        goalReset(Goal::SettleBeforePower, "already registered");
        g_retryAt = now + kPostRegisterSettleMs;
        break;
      }
      if (!expired(now, g_retryAt)) break;
      if (sendRawAfter(kFuncs[g_regIdx], kRegisterCommand, sizeof(kRegisterCommand),
                       TxAfter::GoalReply, g_goalToken, g_authEpoch)) {
        g_goalTxPending = true;
      } else {
        g_retryAt = now + kRetryMs;
      }
      break;

    case Goal::SettleBeforePower:
      if (!appSessionReady()) break;
      if (!(g_sync & FUNCSREG)) {
        goalReset(Goal::Register, "pre-power settle lost registration");
        break;
      }
      if (expired(now, g_retryAt)) goalReset(Goal::PowerOn, "400 ms post-registration settle complete");
      break;

    case Goal::PowerOn: {
      if (!appSessionReady()) break;
      if (!(g_sync & FUNCSREG)) { goalReset(Goal::Register, "display ON needs registration"); break; }
      if (!expired(now, g_retryAt)) break;
      // The live-radio monitor uses zero padding for the captured profile. The state byte
      // is the semantic part (09 = ON); legacy FF padding is reserved for the one fallback
      // path, never silently claimed to be the captured wire form.
      const uint8_t on[8] = {0x03, 0x52, 0x09, 0x00, 0x00, 0x00, 0x00, 0x00};
      if (sendRawAfter(kFuncs[0], on, 8, TxAfter::GoalReply, g_goalToken, g_authEpoch)) {
        g_goalTxPending = true;
      } else {
        g_retryAt = now + kClockRetryMs;
      }
      break;
    }

    case Goal::WarmUp:
      if (!appSessionReady()) break;
      if (!(g_sync & FUNCSREG)) { goalReset(Goal::Register, "warm-up lost registration"); break; }
      if (expired(now, g_retryAt)) goalReset(Goal::SetClock, "display ON warm-up complete");
      break;

    case Goal::SetClock: {
      if (!appSessionReady()) break;
      if (!(g_sync & FUNCSREG)) { goalReset(Goal::Register, "clock needs registration"); break; }
      if (!expired(now, g_retryAt)) break;
      const uint8_t clk[8] = {0x05, 0x56,
                              static_cast<uint8_t>(kClockHHMM[0]),
                              static_cast<uint8_t>(kClockHHMM[1]),
                              static_cast<uint8_t>(kClockHHMM[2]),
                              static_cast<uint8_t>(kClockHHMM[3]), 0x00, 0x00};
      if (sendRawAfter(kFuncs[0], clk, 8, TxAfter::GoalReply, g_goalToken, g_authEpoch)) {
        g_goalTxPending = true;
      } else {
        g_retryAt = now + kRetryMs;
      }
      break;
    }

    case Goal::Done:
      break;                                   // re-issue paths are event-driven
  }
}

// ---------------------------------------------------------------------------
// Direct-TWAI completion and recovery
// ---------------------------------------------------------------------------
// TXQ means the controller accepted a frame; TX+ / TX- are the controller's later result.
// They are intentionally separate records, so neither the black box nor the web UI can
// mistake an application scheduling attempt for a CAN transmission.
void dispatchTxResult(const TxItem& item, bool success, uint32_t now) {
  switch (item.after) {
    case TxAfter::Probe:
      if (item.completionToken != g_probeGeneration || !g_syncProbeAwaitingTx) break;
      g_syncProbeAwaitingTx = false;
      if (success) {
        g_syncProbeIssued = true;
        g_startCompatAtMs = now + kStartCompatMs;
        ++g_bootstraps;
        if (kEnable01Compatibility) {
          logmsg("BA probe TX+ once; awaiting 61 11 00 (01 compatibility after %lums)",
                 static_cast<unsigned long>(kStartCompatMs));
        } else {
          logmsg("BA probe TX+ once; awaiting 61 11 00 (01 remains bootstrap-only)");
        }
      } else if (++g_probeTxFailures <= kBootstrapMaxTxFailures) {
        // BOUNDED, AND SLOWLY. An unbounded 10 Hz retry of an undeliverable BA is a transmit
        // storm that makes the very collisions it is failing on more likely. The display
        // repeats its own request on its own timer; we do not have to shout.
        g_syncProbePending = true;
        g_nextBootstrapQueueMs = now + kBootstrapRetryBackoffMs;
        logmsg("BA probe TX- (%u/%u); retrying in %lums",
               static_cast<unsigned>(g_probeTxFailures),
               static_cast<unsigned>(kBootstrapMaxTxFailures),
               static_cast<unsigned long>(kBootstrapRetryBackoffMs));
      } else {
        logmsg("BA probe TX- x%u - giving up; the opening runs off the display's own 61 11",
               static_cast<unsigned>(g_probeTxFailures));
      }
      break;

    case TxAfter::ControlAck:
      if (success) {
        ++g_autoAcks;                         // count only a physical controller TX+
        break;
      }
      // A reset/re-auth makes a formerly useful 5C1 response dangerous. Do not revive it
      // after its epoch or inbound control-plane permission disappeared.
      if (item.completionToken != g_authEpoch || !g_syncRequestSeen) break;
      if (item.retryCount >= kControlAckMaxRetries) {
        ++g_autoAckFailures;
        logmsg("panel ACK %03X TX- after %u attempt(s); awaiting a fresh panel frame",
               static_cast<unsigned>(item.id), static_cast<unsigned>(item.retryCount + 1u));
        break;
      }
      if (sendControl1(item.id, item.d[0], static_cast<uint8_t>(item.retryCount + 1u))) {
        ++g_autoAckRetries;
        logmsg("panel ACK %03X TX-; retrying once", static_cast<unsigned>(item.id));
      } else {
        ++g_autoAckFailures;
        logmsg("panel ACK %03X TX-; retry could not enter control outbox",
               static_cast<unsigned>(item.id));
      }
      break;

    case TxAfter::HelloStep:
      if (item.completionToken != g_authEpoch || !g_authHelloPending) break;
      g_helloActive = false;
      if (success) {
        ++g_helloIndex;
        g_nextHelloMs = now + kHelloStepMs;
      } else {
        g_helloIndex = 0;
        g_helloPending = true;
        g_nextHelloMs = now + kHelloRetryMs;
        logmsg("opening B0 TX- - restarting %s in %lums", helloStyleName(g_helloStyle),
               static_cast<unsigned long>(kHelloRetryMs));
      }
      break;

    case TxAfter::HelloComplete:
      if (item.completionToken != g_authEpoch || !g_authHelloPending) break;
      g_helloActive = false;
      if (success) {
        g_helloPending = false;
        g_helloAuthEpochPending = 0;
        ++g_hellos;
        authorizeAfterHello(item.completionToken);
      } else {
        // One lost final fragment cannot leave the POC permanently half-open. Re-run the
        // three-frame captured/legacy greeting once the controller is available again.
        g_helloIndex = 0;
        g_helloPending = true;
        g_nextHelloMs = now + kHelloRetryMs;
        logmsg("final opening frame TX- - restarting %s in %lums", helloStyleName(g_helloStyle),
               static_cast<unsigned long>(kHelloRetryMs));
      }
      break;

    case TxAfter::GoalReply:
      if (item.completionToken != g_goalToken) break;  // obsolete queued application work
      g_goalTxPending = false;
      if (success && item.sessionEpoch == g_authEpoch && appSessionReady() &&
          g_wait.active && g_wait.replyId == (item.id | kReplyFlag)) {
        // The expected reply was reserved at TXQ so a fast panel reply cannot be dropped;
        // only now, at controller TX+, does its application timeout start.
        confirmWait(item.id, now);
      } else {
        g_wait = Wait{};
        g_goalTxFailed = true;
      }
      break;

    case TxAfter::None:
      break;
  }
}

void recordTxResult(const TxItem& item, bool success, bool queued, uint32_t now) {
  (void)now;
  if (success) {
    bootPush(4, item.id, 8, item.d);          // TX+
    capturePush(3, item.id, 8, item.d);       // TX+
  } else if (queued) {
    bootPush(5, item.id, 8, item.d);          // TX- after controller acceptance
    capturePush(4, item.id, 8, item.d);       // TX-
  } else {
    bootPush(6, item.id, 8, item.d);          // TX! local controller refusal
    capturePush(5, item.id, 8, item.d);       // TX!
  }
}

void finishInFlight(bool success, uint32_t now, bool controllerAlert = true) {
  if (!g_txInFlight) return;
  const TxItem item = g_txCurrent;
  g_txInFlight = false;
  g_txCurrentIsControl = false;
  g_txInFlightSince = 0;
  if (controllerAlert && success) {
    ++g_txSuccessAlerts;
  } else if (controllerAlert) {
    ++g_txFailedAlerts;
  }
  if (!success) {
    cancelSelfTx(g_txCurrentSelfSlot);
  }
  recordTxResult(item, success, /*queued=*/true, now);
  dispatchTxResult(item, success, now);
}

// A controller that accepted TXQ but produces neither TX+ nor TX- must not retain protocol
// ownership forever. Drop every queued frame from that session and demand a new panel request;
// this is intentionally more conservative than guessing whether an old clock write escaped.
void invalidateForTxTransportFault(const char* why) {
  g_txOutR = g_txOutW;
  g_ctrlOutR = g_ctrlOutW;
  // The capture retains every received row, but protocol work must not consume a response
  // that was queued before the controller restart and call it a fresh authorization.
  portENTER_CRITICAL(&g_fifoMux);
  g_fifoR = g_fifoW;
  portEXIT_CRITICAL(&g_fifoMux);
  ++g_authEpoch;
  if (g_authEpoch == 0) ++g_authEpoch;
  g_sync = FAILED;
  g_panelSeen = false;
  g_syncRequestSeen = false;
  g_authRequestSeen = false;
  g_authHelloPending = false;
  g_helloPending = false;
  g_helloActive = false;
  g_helloIndex = 0;
  g_helloAuthEpochPending = 0;
  g_start01Seen = false;
  g_syncProbePending = false;
  g_syncProbeAwaitingTx = false;
  g_syncProbeIssued = false;
  g_probeTxFailures = 0;
  ++g_probeGeneration;
  if (g_probeGeneration == 0) ++g_probeGeneration;
  g_nextBootstrapQueueMs = 0;
  g_startCompatAtMs = 0;
  g_lastPeerMs = 0;
  g_wait = Wait{};
  ++g_syncLosses;
  goalReset(Goal::WaitSync, why);
}

void handleBusOff(uint32_t now) {
  if (g_canRecovering) return;
  ++g_busOffs;
  g_canReady = false;
  g_canRecovering = true;
  g_txRestartPending = true;                 // also recovers if the BUS_RECOVERED alert is lost
  g_nextTxRestartMs = now + kTxRestartRetryMs;
  finishInFlight(false, now, /*controllerAlert=*/false);

  // Nothing queued before a bus-off may silently emerge after recovery. Start clean and
  // demand a brand-new full panel authorization request.
  g_txOutR = g_txOutW;
  g_ctrlOutR = g_ctrlOutW;
  portENTER_CRITICAL(&g_fifoMux);
  g_fifoR = g_fifoW;
  portEXIT_CRITICAL(&g_fifoMux);
  g_panelSeen = false;
  g_syncRequestSeen = false;
  g_helloPending = false;
  g_helloActive = false;
  g_helloIndex = 0;
  g_start01Seen = false;
  g_syncProbePending = false;
  g_syncProbeAwaitingTx = false;
  g_syncProbeIssued = false;
  g_probeTxFailures = 0;
  ++g_probeGeneration;
  if (g_probeGeneration == 0) ++g_probeGeneration;
  g_nextBootstrapQueueMs = 0;
  g_startCompatAtMs = 0;
  invalidateAuthorization("TWAI bus-off - fresh panel auth required after recovery");

  const esp_err_t err = twai_initiate_recovery();
  if (err != ESP_OK) {
    g_canRecovering = false;                  // retry the recovery request on the paced poll path
    ++g_busRecoveryFailures;
    logmsg("TWAI bus-off recovery request failed: %s", esp_err_to_name(err));
  } else {
    logmsg("TWAI BUS-OFF - recovery started; TX held until controller restarts");
  }
}

void handleBusRecovered() {
  if (!g_canRecovering) return;
  // Legacy ESP-IDF completes recovery in STOPPED state. Starting here has no blocking
  // receive/transmit call and keeps the existing RX task alive.
  const esp_err_t err = twai_start();
  if (err == ESP_OK) {
    g_canReady = true;
    g_canRecovering = false;
    g_txRestartPending = false;
    ++g_busRecoveries;
    logmsg("TWAI recovered and restarted - waiting for fresh 69 / 61 11 opening");
  } else {
    g_canReady = false;
    g_txRestartPending = true;
    g_nextTxRestartMs = millis() + kTxRestartRetryMs;
    ++g_busRecoveryFailures;
    logmsg("TWAI recovered but start failed: %s", esp_err_to_name(err));
  }
}

// BUS_RECOVERED should be delivered as an alert, but periodically checking a stopped/recovering
// controller prevents a lost alert from becoming a permanent offline state.
void pumpTxRestart(uint32_t now) {
  if (!g_txRestartPending || !expired(now, g_nextTxRestartMs)) return;

  twai_status_info_t st{};
  if (twai_get_status_info(&st) == ESP_OK) {
    if (st.state == TWAI_STATE_BUS_OFF) {
      g_canRecovering = false;
      g_txRestartPending = false;
      handleBusOff(now);
      return;
    }
    if (st.state == TWAI_STATE_RECOVERING) {
      g_canReady = false;
      g_canRecovering = true;
      g_nextTxRestartMs = now + kTxRestartRetryMs;
      return;
    }
    if (st.state == TWAI_STATE_RUNNING) {
      g_canReady = true;
      g_canRecovering = false;
      g_txRestartPending = false;
      ++g_txRestarts;
      logmsg("TWAI outcome watchdog found controller already running - fresh auth still required");
      return;
    }
  }

  const esp_err_t err = twai_start();
  if (err == ESP_OK) {
    g_canReady = true;
    g_canRecovering = false;
    g_txRestartPending = false;
    ++g_txRestarts;
    logmsg("TWAI outcome watchdog restarted controller - waiting for fresh 69 / 61 opening");
    return;
  }

  g_canReady = false;
  ++g_txRestartFailures;
  g_nextTxRestartMs = now + kTxRestartRetryMs;
  if (g_txRestartFailures <= 3 || (g_txRestartFailures % 10u) == 0) {
    logmsg("TWAI outcome watchdog restart failed: %s; retrying in %lums", esp_err_to_name(err),
           static_cast<unsigned long>(kTxRestartRetryMs));
  }
}

void pumpTxOutcomeWatchdog(uint32_t now) {
  pumpTxRestart(now);
  if (g_txRestartPending || !g_txInFlight ||
      !expired(now, g_txInFlightSince + kTxOutcomeMs)) {
    return;
  }

  // If the controller actually entered bus-off, use its canonical recovery sequence rather
  // than stop/start. The alert may simply have been delayed behind the high-rate RX workload.
  twai_status_info_t st{};
  if (twai_get_status_info(&st) == ESP_OK) {
    if (st.state == TWAI_STATE_BUS_OFF) {
      handleBusOff(now);
      return;
    }
    if (st.state == TWAI_STATE_RECOVERING) {
      ++g_txOutcomeTimeouts;
      finishInFlight(false, now, /*controllerAlert=*/false);
      invalidateForTxTransportFault("TX outcome stalled while TWAI was recovering");
      g_canReady = false;
      g_canRecovering = true;
      g_txRestartPending = true;
      g_nextTxRestartMs = now + kTxRestartRetryMs;
      return;
    }
  }

  ++g_txOutcomeTimeouts;
  const TxItem stalled = g_txCurrent;
  finishInFlight(false, now, /*controllerAlert=*/false);
  invalidateForTxTransportFault("TXQ had no TX outcome - fresh panel auth required");
  g_canReady = false;
  g_canRecovering = false;

  // A valid 500 kbit/s frame cannot still be on the wire after kTxOutcomeMs. Stop/start
  // clears the controller's ownership deterministically without uninstalling it while the
  // RX task is blocked. Any delayed old alert is ignored because no frame remains in flight.
  const esp_err_t stopped = twai_stop();
  if (stopped != ESP_OK && stopped != ESP_ERR_INVALID_STATE) {
    logmsg("TWAI outcome watchdog stop failed after %03X: %s", stalled.id,
           esp_err_to_name(stopped));
  } else {
    logmsg("TWAI TX outcome timeout on %03X - controller restart scheduled", stalled.id);
  }
  g_txRestartPending = true;
  g_nextTxRestartMs = now;
  pumpTxRestart(now);                         // normally restarts immediately; otherwise paced
}

void pumpCanAlerts() {
  uint32_t alerts = 0;
  // BUS_ERROR can arrive much faster than the application loop.  Draining until empty
  // turns a noisy bus into an accidental infinite loop: HTTP remains alive on its own
  // task, but capture flushing, the watchdog and the protocol FSM no longer run.  The
  // alert bits are diagnostic/coalescing signals, not a frame stream, so a bounded drain
  // preserves every state transition that matters while guaranteeing the rest of this
  // POC (and its recovery path) gets CPU time.
  constexpr uint8_t kAlertsPerPass = 32;
  for (uint8_t n = 0; n < kAlertsPerPass && twai_read_alerts(&alerts, 0) == ESP_OK; ++n) {
    g_twaiAlerts |= alerts;
    const uint32_t now = millis();

    // A completed TX may be reported in the same batch as a following BUS_OFF. Record the
    // frame outcome before resetting the session; it remains useful forensic evidence.
    if ((alerts & TWAI_ALERT_TX_SUCCESS) && g_txInFlight) finishInFlight(true, now);
    if ((alerts & TWAI_ALERT_TX_FAILED) && g_txInFlight) finishInFlight(false, now);
    if (alerts & TWAI_ALERT_BUS_OFF) handleBusOff(now);
    if (alerts & TWAI_ALERT_BUS_RECOVERED) handleBusRecovered();
  }
}

void pumpTx() {
  if (!g_txGate || !g_canReady || g_canRecovering || g_txRestartPending || g_txInFlight ||
      (g_ctrlOutR == g_ctrlOutW && g_txOutR == g_txOutW))
    return;

  // The control lane is deliberately examined first. It contains only generic panel ACKs,
  // so `1C1 70 -> 5C1 74` can land between B0#1 and B0#2 even if ordinary liveness work
  // is waiting in the normal lane.
  const bool control = g_ctrlOutR != g_ctrlOutW;
  const uint8_t cap = control ? kCtrlOutbox : kTxOutbox;
  uint8_t& read = control ? g_ctrlOutR : g_txOutR;
  TxItem& head = control ? g_ctrlOutbox[read] : g_txOutbox[read];
  const uint8_t next = static_cast<uint8_t>((read + 1) % cap);

  // Never let a reply/registration/time frame queued in an old session leak into a new
  // bootstrap phase. Opening frames have a special tag: they must transmit while output is
  // intentionally locked, but only for the auth epoch that scheduled them.
  const bool probeFrame = head.after == TxAfter::Probe;
  const bool openingFrame = head.after == TxAfter::HelloStep ||
                            head.after == TxAfter::HelloComplete;
  const bool stale = probeFrame
      ? (head.completionToken != g_probeGeneration || !g_syncProbeAwaitingTx)
      : openingFrame
      ? (head.sessionEpoch != g_authEpoch || !g_authHelloPending)
      : (head.sessionEpoch &&
         (head.sessionEpoch != g_authEpoch || !appSessionReady()));
  if (stale) {
    const TxItem stale = head;
    read = next;
    ++g_txRefused;
    recordTxResult(stale, false, /*queued=*/false, millis());
    dispatchTxResult(stale, false, millis());
    return;
  }

  twai_message_t frame{};
  frame.identifier = head.id;
  frame.extd = 0;
  frame.rtr = 0;
  // NOT single-shot. `ss = 1` turns every transient CAN error into a permanent failure: one
  // disturbed ACK bit, one lost arbitration, and the frame is gone with no hardware retry.
  // Measured on the bench at 0.5.0 against a live display: TX+ 43 vs TX- 334, an 89% failure
  // rate, while the display was plainly there (it opened 1C1 and we acked it six times).
  // Automatic retransmission is what CAN is FOR, and it is what the OEM radio's controller
  // does. The wedge this flag was defending against is already covered — kTxOutcomeMs (500
  // ms) restarts the controller if a frame never settles, so a genuinely stuck arbitration
  // is bounded by the watchdog rather than by throwing away every recoverable collision.
  frame.ss = 0;
  frame.self = 0;                            // never ask the controller to manufacture RX
  frame.data_length_code = 8;
  memcpy(frame.data, head.d, sizeof(frame.data));

  // Establish ownership before the nonblocking driver call. Alerts are consumed only by
  // loop(), but this order is still correct if an interrupt completes the frame instantly.
  g_txCurrent = head;
  g_txCurrentIsControl = control;
  g_txInFlight = true;
  g_txInFlightSince = millis();
  g_txCurrentSelfSlot = noteSelfTx(head.id, head.d, 8);
  const esp_err_t err = twai_transmit(&frame, 0);
  if (err == ESP_OK) {
    // A real display can answer before this loop pass records TX+. Reserve this precise
    // reply at controller acceptance, fenced by the RX-task timestamp, then let TX+
    // start the timeout in dispatchTxResult().
    if (head.after == TxAfter::GoalReply && head.completionToken == g_goalToken &&
        head.sessionEpoch == g_authEpoch && appSessionReady()) {
      reserveWait(head.id, esp_timer_get_time());
    }
    read = next;
    ++g_txCount;                            // TXQ: controller accepted it
    bootPush(1, head.id, 8, head.d);
    capturePush(1, head.id, 8, head.d);
    tap(head.id, head.d, 8, /*tx=*/true);
    return;
  }

  g_txInFlight = false;
  g_txCurrentIsControl = false;
  g_txInFlightSince = 0;
  cancelSelfTx(g_txCurrentSelfSlot);
  if (err == ESP_ERR_TIMEOUT || err == ESP_FAIL) {
    // The single hardware slot is briefly occupied. Keep the head in software and retry on
    // a later loop pass; there is no wait and no loss of ordering.
    ++g_txBusy;
    return;
  }

  // A real local refusal (for example a stopped driver) has no controller TX- alert.
  // Remove it, record TX!, and let the appropriate protocol layer choose its bounded retry.
  const TxItem refused = head;
  read = next;
  ++g_txRefused;
  recordTxResult(refused, false, /*queued=*/false, millis());
  dispatchTxResult(refused, false, millis());
  if (g_txRefused <= 5 || (g_txRefused % 50u) == 0)
    logmsg("TWAI refused TXQ %03X: %s", refused.id, esp_err_to_name(err));
}

// This pump is deliberately usable before setup() has completed Wi-Fi association. CAN
// starts first and the panel may have begun its `61 11` request already; joining a network
// must never create an RX-only auth gap. It has no blocking call and bounds every drain.
void pumpProtocol(uint32_t now) {
  pumpCanAlerts();
  pumpTxOutcomeWatchdog(now);
  pumpRx(now);
  pumpTick(now);
  pumpGoal(now);
  pumpTx();
  pumpCanAlerts();
  pumpCapture();
  pumpStat(now);
}

}  // namespace

// ===========================================================================
// DEPLOYMENT SCAFFOLDING — WiFi, console, OTA. The board must stay reachable.
// ===========================================================================

namespace {

constexpr const char* kWifiNamespace = "megaopen";   // read-only: ssid / pass
constexpr const char* kApSsid        = "AffaClock";
constexpr const char* kApPass        = "affaclock1";
constexpr const char* kMdnsName      = "affaclock";
constexpr uint32_t    kStaJoinMs     = 15000;

PsychicHttpServer g_server;
bool     g_otaRunning = false;
uint32_t g_rebootAt   = 0;

const char kPage[] PROGMEM = R"HTML(<!doctype html><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>AffaDisplay 06_authclock</title>
<style>
body{font:14px/1.5 system-ui,sans-serif;margin:0;padding:16px;background:#111;color:#ddd}
h1{font-size:16px;margin:0 0 12px}
fieldset{border:1px solid #333;border-radius:6px;margin:0 0 12px;padding:10px}
legend{color:#8ab;padding:0 6px}
button{font:13px system-ui;background:#243;color:#cfe;border:1px solid #465;border-radius:4px;
padding:5px 10px;margin:2px;cursor:pointer}
button:hover{background:#365}
pre{background:#000;border:1px solid #333;border-radius:6px;padding:8px;overflow:auto;
max-height:45vh;font:12px/1.4 ui-monospace,monospace;color:#9c9}
</style>
<h1>AffaDisplay <span>06_authclock</span> &middot; stock driver defaults &middot;
<a href=/update style="color:#8ab">OTA</a></h1>
<fieldset><legend>control</legend>
<button onclick=go('/api/restart')>redo sequence</button>
<button onclick=go('/api/reboot')>reboot board (black box restarts)</button>
<a href="/api/boot?from=0" style="color:#8ab;margin-left:12px">full black box (paged)</a>
<a href="/api/capture" style="color:#8ab;margin-left:12px">download raw CAN capture</a>
</fieldset>
<fieldset><legend>status</legend><pre id=s>...</pre></fieldset>
<fieldset><legend>black box &mdash; first 2400 events after power-on (tail)</legend>
<pre id=b>...</pre></fieldset>
<fieldset><legend>live wire (identical frames coalesced)</legend><pre id=f>...</pre></fieldset>
<fieldset><legend>log</legend><pre id=l>...</pre></fieldset>
<script>
async function go(u){ try{ await fetch(u) }catch(e){} }
async function tick(){
  try{
    s.textContent = await (await fetch('/api/status')).text()
    b.textContent = await (await fetch('/api/boot?tail=1')).text()
    f.textContent = await (await fetch('/api/frames')).text()
    l.textContent = await (await fetch('/api/log')).text()
  }catch(e){ s.textContent = 'offline' }
  setTimeout(tick, 2000)
}
tick()
</script>
)HTML";

const char* drvStateName(uint8_t s) {
  switch (s) {
    case 0: return "STOPPED";
    case 1: return "running";
    case 2: return "BUS-OFF";
    case 3: return "recovering";
    case 9: return "no-driver";
  }
  return "?";
}

const char* bootKindName(uint8_t kind) {
  switch (kind) {
    case 0: return "RX ";
    case 1: return "TXQ";
    case 3: return "RXE";
    case 4: return "TX+";
    case 5: return "TX-";
    case 6: return "TX!";
  }
  return "???";
}

int fmtBootRow(char* out, size_t cap, const BootRec& r) {
  if (r.kind == 2) {
    const uint32_t be = static_cast<uint32_t>(r.d[2]) | (static_cast<uint32_t>(r.d[3]) << 8) |
                        (static_cast<uint32_t>(r.d[4]) << 16) |
                        (static_cast<uint32_t>(r.d[5]) << 24);
    return snprintf(out, cap, "%8lu.%03lu  STAT %-9s txErr %3u rxErr %3u busErr %lu\n",
                    static_cast<unsigned long>(r.ms), static_cast<unsigned long>(r.us),
                    drvStateName(r.len),
                    r.d[0], r.d[1], static_cast<unsigned long>(be));
  }
  return snprintf(out, cap, "%8lu.%03lu  %s %03X  %02X %02X %02X %02X %02X %02X %02X %02X\n",
                  static_cast<unsigned long>(r.ms), static_cast<unsigned long>(r.us),
                  bootKindName(r.kind), r.id,
                  r.d[0], r.d[1], r.d[2], r.d[3], r.d[4], r.d[5], r.d[6], r.d[7]);
}

void routes() {
  g_server.on("/", HTTP_GET, [](PsychicRequest* r) {
    return r->reply(200, "text/html", kPage);
  });

  g_server.on("/api/status", HTTP_GET, [](PsychicRequest* r) {
    twai_status_info_t st{};
    const bool valid = twai_get_status_info(&st) == ESP_OK;
    const uint32_t now = millis();
    const uint16_t bootN = g_bootN;

    char out[2300];
    int n = 0;
    n += snprintf(out + n, sizeof(out) - n, "%s  up %lus\n", kVersion,
                  static_cast<unsigned long>(now / 1000));
    n += snprintf(out + n, sizeof(out) - n,
                  "driver  direct ESP-IDF TWAI: install+start 500k, accept-all, auto-retry; "
                  "mode %s\n",
                  kNoAckMode ? "NO_ACK experiment" : "normal");
    n += snprintf(out + n, sizeof(out) - n,
                  "session panel %s; full61 %s (enables panel 1C1->5C1); opening %s%s "
                  "(69->B9+one BA; 00 normal; %s)\n",
                  g_panelSeen ? "seen" : "not seen",
                  g_syncRequestSeen ? "seen" : "waiting",
                  g_authRequestSeen ? "OPEN" : "WAITING",
                  g_authHelloPending ? " (hello pending)" : "",
                  kEnable01Compatibility ? "persistent 01 compatibility ENABLED" :
                                            "01 bootstrap-only; waiting for 00");
    n += snprintf(out + n, sizeof(out) - n, "sync    0x%02X %s%s%s\n", g_sync,
                  (g_sync & FAILED)     ? "FAILED "     : "",
                  (g_sync & PEER_ALIVE) ? "PEER_ALIVE " : "",
                  (g_sync & FUNCSREG)   ? "FUNCSREG "   : "");
    n += snprintf(out + n, sizeof(out) - n, "goal    %s\n", goalName(g_goal));
    n += snprintf(out + n, sizeof(out) - n,
                  "panel   pings %lu (last %ld ms ago) openings %lu unknown61 %lu keys %lu\n",
                  static_cast<unsigned long>(g_pings),
                  g_lastPingMs ? static_cast<long>(now - g_lastPingMs) : -1,
                  static_cast<unsigned long>(g_syncReqs),
                  static_cast<unsigned long>(g_badAuths),
                  static_cast<unsigned long>(g_keys));
    n += snprintf(out + n, sizeof(out) - n,
                  "us      pongs-queued %lu hellos-TX+ %lu BA-TX+ %lu autoAcks-TX+ %lu "
                  "retry %lu fail %lu reg %lu/%lu power %lu/%lu clock %lu/%lu\n",
                  static_cast<unsigned long>(g_pongs),
                  static_cast<unsigned long>(g_hellos),
                  static_cast<unsigned long>(g_bootstraps),
                  static_cast<unsigned long>(g_autoAcks),
                  static_cast<unsigned long>(g_autoAckRetries),
                  static_cast<unsigned long>(g_autoAckFailures),
                  static_cast<unsigned long>(g_regOk),
                  static_cast<unsigned long>(g_regOk + g_regFail),
                  static_cast<unsigned long>(g_powerOkCnt),
                  static_cast<unsigned long>(g_powerOkCnt + g_powerFail),
                  static_cast<unsigned long>(g_clockOkCnt),
                  static_cast<unsigned long>(g_clockOkCnt + g_clockFail));
    n += snprintf(out + n, sizeof(out) - n,
                  "frames  rx %lu selfEcho %lu fifoDrop %lu syncLosses %lu\n",
                  static_cast<unsigned long>(g_rxCount),
                  static_cast<unsigned long>(g_selfEchoes),
                  static_cast<unsigned long>(g_fifoDrop),
                  static_cast<unsigned long>(g_syncLosses));
    n += snprintf(out + n, sizeof(out) - n,
                  "tx      scheduled %lu TXQ %lu TX+ %lu TX- %lu TX! %lu busy %lu outbox n%u/c%u "
                   "inflight %s%s age %lums\n",
                  static_cast<unsigned long>(g_txScheduled),
                  static_cast<unsigned long>(g_txCount),
                  static_cast<unsigned long>(g_txSuccessAlerts),
                  static_cast<unsigned long>(g_txFailedAlerts),
                  static_cast<unsigned long>(g_txRefused),
                  static_cast<unsigned long>(g_txBusy),
                   static_cast<unsigned>(txOutboxUsed()),
                   static_cast<unsigned>(ctrlOutboxUsed()),
                   g_txInFlight ? "yes" : "no",
                   g_txInFlight ? (g_txCurrentIsControl ? "(control)" : "(normal)") : "",
                   static_cast<unsigned long>(g_txInFlight ? now - g_txInFlightSince : 0));
    n += snprintf(out + n, sizeof(out) - n,
                  "txwatch outcomeTimeouts %lu restarts %lu restartFailures %lu%s\n",
                  static_cast<unsigned long>(g_txOutcomeTimeouts),
                  static_cast<unsigned long>(g_txRestarts),
                  static_cast<unsigned long>(g_txRestartFailures),
                  g_txRestartPending ? " (restart pending)" : "");
    n += snprintf(out + n, sizeof(out) - n,
                  "boot    black box %u/%u rows%s\n", bootN, kBoot,
                  bootN >= kBoot ? "  FROZEN (full history from t=0 preserved)" : ", recording");
    n += snprintf(out + n, sizeof(out) - n,
                  "capture %s%s rows %lu bytes %lu fifoDrop %lu  GET /api/capture\n",
                  g_captureOpen ? "recording" : "unavailable",
                  g_captureFull ? " (FROZEN)" : "",
                  static_cast<unsigned long>(g_captureRows),
                  static_cast<unsigned long>(g_captureBytes),
                  static_cast<unsigned long>(g_captureDrop));
    n += snprintf(out + n, sizeof(out) - n,
                  "drv     %s txErr %lu rxErr %lu busErr %lu arbLost %lu rxMissed %lu "
                  "driverTX %lu txFailed %lu\n",
                  valid ? drvStateName(static_cast<uint8_t>(st.state)) : "no-driver",
                  static_cast<unsigned long>(st.tx_error_counter),
                  static_cast<unsigned long>(st.rx_error_counter),
                  static_cast<unsigned long>(st.bus_error_count),
                  static_cast<unsigned long>(st.arb_lost_count),
                  static_cast<unsigned long>(st.rx_missed_count),
                  static_cast<unsigned long>(st.msgs_to_tx),
                  static_cast<unsigned long>(st.tx_failed_count));
    n += snprintf(out + n, sizeof(out) - n,
                  "recovery ready %s recovering %s busOff %lu recovered %lu failures %lu alerts 0x%08lX\n",
                  g_canReady ? "yes" : "no", g_canRecovering ? "yes" : "no",
                  static_cast<unsigned long>(g_busOffs),
                  static_cast<unsigned long>(g_busRecoveries),
                  static_cast<unsigned long>(g_busRecoveryFailures),
                  static_cast<unsigned long>(g_twaiAlerts));
    n += snprintf(out + n, sizeof(out) - n, "wifi    %s  rssi %d\n",
                  WiFi.isConnected() ? WiFi.localIP().toString().c_str() : "AP mode",
                  WiFi.RSSI());
    return r->reply(200, "text/plain", out);
  });

  // The black box, paged: ?from=N gives 120 rows starting at N; ?tail=1 gives the last 40.
  g_server.on("/api/boot", HTTP_GET, [](PsychicRequest* r) {
    const uint16_t total = g_bootN;
    uint16_t from = 0;
    uint16_t count = 120;
    if (r->getParam("tail")) {
      count = 40;
      from  = total > count ? static_cast<uint16_t>(total - count) : 0;
    } else if (r->getParam("from")) {
      const long v = r->getParam("from")->value().toInt();
      from = (v < 0) ? 0 : (v > total ? total : static_cast<uint16_t>(v));
    }
    uint16_t upto = static_cast<uint16_t>(from + count);
    if (upto > total) upto = total;

    char out[8600];
    int n = snprintf(out, sizeof(out), "rows %u..%u of %u%s\n", from,
                     upto ? upto - 1 : 0, total,
                     total >= kBoot ? " (FROZEN)" : "");
    for (uint16_t i = from; i < upto; ++i) {
      BootRec rec;
      portENTER_CRITICAL(&g_bootMux);
      rec = g_boot[i];
      portEXIT_CRITICAL(&g_bootMux);
      n += fmtBootRow(out + n, sizeof(out) - n, rec);
      if (n > static_cast<int>(sizeof(out)) - 100) break;
    }
    if (upto < total)
      n += snprintf(out + n, sizeof(out) - n, "next: /api/boot?from=%u\n", upto);
    return r->reply(200, "text/plain", out);
  });

  g_server.on("/api/frames", HTTP_GET, [](PsychicRequest* r) {
    char out[2600];
    int n = 0;
    portENTER_CRITICAL(&g_frameMux);
    for (uint8_t i = 0; i < kFrameRing; ++i) {
      const FrameRec& f = g_frames[(g_frameHead + i) % kFrameRing];
      if (!f.count) continue;
      n += snprintf(out + n, sizeof(out) - n,
                    "%s %03X  %02X %02X %02X %02X %02X %02X %02X %02X",
                    f.dir ? "TX" : "RX", f.id,
                    f.d[0], f.d[1], f.d[2], f.d[3], f.d[4], f.d[5], f.d[6], f.d[7]);
      if (f.count > 1)
        n += snprintf(out + n, sizeof(out) - n, "  x%lu (t=%lu..%lu)\n",
                      static_cast<unsigned long>(f.count),
                      static_cast<unsigned long>(f.firstMs),
                      static_cast<unsigned long>(f.lastMs));
      else
        n += snprintf(out + n, sizeof(out) - n, "  (t=%lu)\n",
                      static_cast<unsigned long>(f.firstMs));
      if (n > static_cast<int>(sizeof(out)) - 80) break;
    }
    portEXIT_CRITICAL(&g_frameMux);
    if (n == 0) n = snprintf(out, sizeof(out), "(nothing heard or sent yet)\n");
    return r->reply(200, "text/plain", out);
  });

  g_server.on("/api/log", HTTP_GET, [](PsychicRequest* r) {
    char out[3400];
    int n = 0;
    portENTER_CRITICAL(&g_logMux);
    for (size_t i = 0; i < kLogRing; ++i) {
      const LogRec& rec = g_log[(g_logHead + i) % kLogRing];
      if (!rec.ms && !rec.msg[0]) continue;
      n += snprintf(out + n, sizeof(out) - n, "%8lu  %s\n",
                    static_cast<unsigned long>(rec.ms), rec.msg);
      if (n > static_cast<int>(sizeof(out)) - 140) break;
    }
    portEXIT_CRITICAL(&g_logMux);
    if (n == 0) n = snprintf(out, sizeof(out), "(empty)\n");
    return r->reply(200, "text/plain", out);
  });

  g_server.on("/api/restart", HTTP_GET, [](PsychicRequest* r) {
    g_doneAtMs = 0;
    goalReset(Goal::WaitSync, "console asked for a full redo");
    return r->reply(200, "text/plain", "ok");
  });

  g_server.on("/api/reboot", HTTP_GET, [](PsychicRequest* r) {
    g_rebootAt = millis() + 300;
    return r->reply(200, "text/plain", "rebooting");
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
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < kStaJoinMs) {
      // CAN has already been armed. Keep the whole bounded protocol/recovery pump alive
      // while association proceeds so the display never waits for Wi-Fi before its hello.
      pumpProtocol(millis());
      delay(5);
    }
    sta = (WiFi.status() == WL_CONNECTED);
  }
  if (!sta) {
    pumpProtocol(millis());
    WiFi.mode(WIFI_AP);
    WiFi.softAP(kApSsid, kApPass);
    pumpProtocol(millis());
  }
  if (MDNS.begin(kMdnsName)) MDNS.addService("http", "tcp", 80);

  const String ip = sta ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  Serial.printf("\n[wifi] %s ip=%s  console http://%s/  OTA http://%s/update  mdns %s.local\n",
                sta ? "STA" : "AP (fallback)", ip.c_str(), ip.c_str(), ip.c_str(), kMdnsName);
}

void startHttp() {
  // The two lockouts that each cost a cable: full socket table (permanent without
  // lru_purge) and the fixed URI table (silent overrun). HTTP-server settings, not CAN.
  g_server.config.lru_purge_enable  = true;
  g_server.config.max_open_sockets  = 7;
  g_server.config.recv_wait_timeout = 3;
  g_server.config.send_wait_timeout = 3;
  g_server.config.max_uri_handlers  = 32;
  // 16k, not the usual 8k: /api/boot formats its page into an 8600-byte stack buffer.
  g_server.config.stack_size        = 16384;

  g_server.listen(80);

  // A binary, fixed-record capture.  It is intentionally served as a static file rather
  // than formatted through RAM, so downloading the evidence never has to reconstruct or
  // truncate it.  `tools/decode_authclock_capture.py` turns it into readable CAN rows.
  if (g_captureOpen) g_server.serveStatic("/api/capture", LittleFS, kCapturePath);

  // OTA FIRST, ALWAYS. An OTA write stalls the CAN ISR, so stop handing frames in.
  ElegantOTA.onStart([]() {
    g_otaRunning = true;
    g_txGate = false;
    logmsg("ota started - CAN TX gated");
  });
  ElegantOTA.onEnd([](bool ok) {
    if (!ok) { g_otaRunning = false; g_txGate = true; }
    logmsg("ota %s", ok ? "ok, rebooting" : "FAILED - TX ungated");
  });
  ElegantOTA.begin(&g_server);

  routes();
}

}  // namespace

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  delay(300);
  Serial.printf("\n%s\n", kVersion);

  // The capture file has to exist BEFORE the direct RX task can hand it its first frame.
  // The display is attached only after this image is armed, so filesystem setup cannot
  // cause us to miss its initial authorization request.
  startCapture();

  // CAN FIRST so the black box records from as close to power-on as this framework can
  // get; WiFi later (it blocks for seconds). This is ordering, not driver configuration.
  //
  // THE ENTIRE DRIVER SETUP — stock, nothing else, in the library's documented order:
  const bool ok = startCan();
  Serial.printf("[can] %s at %lu bit/s, %s (rx=GPIO%d tx=GPIO%d)\n",
                ok ? "RUNNING" : "NOT RUNNING", static_cast<unsigned long>(kBitrate),
                kNoAckMode ? "NO_ACK experiment" : "normal", static_cast<int>(kRxPin),
                static_cast<int>(kTxPin));

  startNetwork();
  startHttp();

  logmsg("%s: display liveness -> B9 + one BA; 00 primary; 01 compatibility %s", kVersion,
         kEnable01Compatibility ? "ENABLED" : "disabled (bootstrap-only)");
  logmsg("black box recording: first %u events (frames both ways + drv state 1/s)",
         static_cast<unsigned>(kBoot));
  logmsg("goal: 69/61->BA->B0x3 + panel ACK->register 151/1F1->power ON->clock %c%c:%c%c",
         kClockHHMM[0], kClockHHMM[1], kClockHHMM[2], kClockHHMM[3]);
}

void loop() {
  ElegantOTA.loop();

  const uint32_t now = millis();
  if (g_rebootAt && expired(now, g_rebootAt)) ESP.restart();
  if (g_otaRunning) { pumpCapture(); delay(10); return; }

  pumpProtocol(now);                          // same bounded FSM used during Wi-Fi association
  pumpSerial();                                // narrate the black box to USB

  delay(5);                                    // IDLE must eat or the single-core C3 panics
}
