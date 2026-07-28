// 01_bringup — prove the link in the order it has to be proved, and never lose the way in.
//
// This is the example to flash on a board you cannot reach with a cable. It does three
// things and deliberately nothing else:
//
//   1. Brings up WiFi and ElegantOTA FIRST, and makes them survivable (see THE LOCKOUT
//      below). Everything after this point may fail without costing you the board.
//   2. Proves we HEAR the panel — /api/frames is the raw Layer-0 tap, RX and TX in wire
//      order. Shut our transmitter with /api/txgate?on=0 (or boot that way with
//      /api/quiet?on=1) and everything left on that ring came from the other end of the
//      wire. Note the controller still ACKs in hardware while the gate is shut, which is
//      what makes the test meaningful on a two-node bus: if we could decode a frame we
//      would acknowledge it, so busErr still climbing with rxFrames at 0 means we genuinely
//      cannot decode. True listen-only is NOT available — Esp32CanLink::begin() refuses
//      LinkMode::ListenOnly, because this driver has no safe place to enter it.
//   3. Proves we can WRITE — the bring-up sequence: power on, WAIT for the panel to
//      actually light up, then "SUCCESS", then the clock at 10:00.
//
// THE LOCKOUT, AND WHY THIS EXAMPLE EXISTS.
// PsychicHttp only sets `lru_purge_enable` under ENABLE_ASYNC, which these builds do not
// define — so the server ran with esp_http_server's defaults: 7 sockets, LRU purge OFF.
// When all 7 are held by lingering connections, httpd stops calling accept() and never
// resumes. The board still answers ICMP and still answers mDNS, so it looks alive from
// every angle except the one that matters: TCP connect hangs for ever, and OTA is gone
// with it. The previous console polled /api/status from the browser once a second, for
// ever, which is exactly how you get there. Three lines in startHttp() and a page that
// does not poll by default are the fix; the self-probe below is the belt to that brace.
//
// NO ANIMATION LIVES HERE AND NONE LIVES IN THE LIBRARY. Scrolling, marquees, blinking and
// every other bit of motion are application policy or a shared widget under src/widget/,
// opted into by the application. This example builds with the menu and the marquee OFF, so
// what you are looking at is the protocol and nothing else.
//
//   pio run -e ex01_bringup                build
//   pio run -e ex01_bringup -t upload      first flash, over USB
//   thereafter: http://<ip>/update         ElegantOTA

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <PsychicHttp.h>
#include <ElegantOTA.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include <AffaDisplay.h>

#if !AFFA_PANEL_CARMINAT
#  error "01_bringup is a Carminat example: build with -D AFFA_PANEL_CARMINAT=1"
#endif
#if !AFFA_ENABLE_TASK
#  error "01_bringup relies on the library owning the poll task: -D AFFA_ENABLE_TASK=1"
#endif

namespace {

// ---------------------------------------------------------------------------
// Board
// ---------------------------------------------------------------------------
// ESP32-C3 SuperMini on the bench rig: rx = GPIO4, tx = GPIO3. The named struct is what
// stops the swap from becoming a silent bus with no error anywhere. -D AFFA_PINS_MIRRORED=1
// tries the other orientation, which is how the same module is soldered on some boards.
#ifndef AFFA_PINS_MIRRORED
#  define AFFA_PINS_MIRRORED 0
#endif
#if AFFA_PINS_MIRRORED
constexpr affa::CanPins kPins{ .rx = GPIO_NUM_3, .tx = GPIO_NUM_4 };
#else
constexpr affa::CanPins kPins{ .rx = GPIO_NUM_4, .tx = GPIO_NUM_3 };
#endif
constexpr uint32_t kBitrate = 500000;

// 250 ms, MEASURED — not 0 and not 2000. 0 leaves a bus-off in TWAI_STATE_STOPPED for ever
// (rxFrames 0, nothing climbing, reads exactly like a dead transceiver); 2000 accumulates
// inside setup() and the board answers its first HTTP request three minutes after boot.
constexpr uint32_t kForceRecoveryMs = 250;

constexpr const char* kWifiNamespace = "megaopen";  // read-only: ssid / pass
constexpr const char* kOwnNamespace  = "affa1";     // ours: the boot mode, and only that
constexpr const char* kApSsid        = "AffaBringup";
constexpr const char* kApPass        = "affabringup";
constexpr const char* kMdnsName      = "affabringup";
constexpr uint32_t    kStaJoinMs     = 15000;

struct ArduinoClock final : affa::IClock {
  uint32_t millis() const override { return ::millis(); }
};

affa::Esp32CanLink    g_link;
ArduinoClock          g_clock;
affa::CarminatDisplay g_display(g_link, g_clock);
affa::rtos::AffaTask  g_task;
PsychicHttpServer     g_server;

bool     g_canUp       = false;
bool     g_bootQuiet   = false;   // booted with our transmitter gated shut
bool     g_otaRunning  = false;
uint32_t g_otaSince    = 0;      // when onStart fired; 0 when no update is in progress
uint32_t g_rebootAt    = 0;

// ---------------------------------------------------------------------------
// The frame ring — the whole point of step 2
// ---------------------------------------------------------------------------
// Pushed from the Layer-0 tap, which runs on the library's owned task; read from the HTTP
// task. A spinlock rather than "it is only diagnostics": a torn record here is exactly the
// kind of thing that sends you looking for a protocol bug that does not exist.
struct FrameRec {
  uint32_t ms;
  uint16_t id;
  uint8_t  dir;          // 0 = RX (heard), 1 = TX (ours)
  uint8_t  len;
  uint8_t  d[8];
};
constexpr size_t kFrameRing = 96;
FrameRec     g_frames[kFrameRing];
size_t       g_frameHead = 0;      // next slot to write
uint32_t     g_frameSeq  = 0;      // total pushed, so the ring can report what it dropped
portMUX_TYPE g_frameMux  = portMUX_INITIALIZER_UNLOCKED;

// Counted separately from the driver's own counters: these are frames that reached the
// LIBRARY, which is the question "do we hear the panel" actually asks.
uint32_t g_rxSeen = 0;
uint32_t g_txSeen = 0;
uint32_t g_lastRxMs = 0;

void onTap(const affa::Frame& f, affa::Direction d, void*) {
  const bool rx = (d == affa::Direction::Rx);
  portENTER_CRITICAL(&g_frameMux);
  FrameRec& r = g_frames[g_frameHead];
  r.ms  = ::millis();
  r.id  = static_cast<uint16_t>(f.id);
  r.dir = rx ? 0 : 1;
  r.len = f.len;
  memcpy(r.d, f.data, 8);
  g_frameHead = (g_frameHead + 1) % kFrameRing;
  ++g_frameSeq;
  if (rx) { ++g_rxSeen; g_lastRxMs = r.ms; } else { ++g_txSeen; }
  portEXIT_CRITICAL(&g_frameMux);
}

// ---------------------------------------------------------------------------
// A small log ring, so a failure explains itself over HTTP
// ---------------------------------------------------------------------------
struct LogRec { uint32_t ms; char msg[96]; };
constexpr size_t kLogRing = 32;
LogRec       g_log[kLogRing];
size_t       g_logHead = 0;
portMUX_TYPE g_logMux = portMUX_INITIALIZER_UNLOCKED;

void logmsg(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void logmsg(const char* fmt, ...) {
  char buf[96];
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

const char* resultName(affa::Result r) {
  switch (r) {
    case affa::Result::Ok:           return "Ok";
    case affa::Result::NoSync:       return "NoSync";
    case affa::Result::UnknownFunc:  return "UnknownFunc";
    case affa::Result::SendFailed:   return "SendFailed";
    case affa::Result::Timeout:      return "Timeout";
    case affa::Result::TooLong:      return "TooLong";
    case affa::Result::QueueFull:    return "QueueFull";
    case affa::Result::NotSupported: return "NotSupported";
    case affa::Result::BadArgument:  return "BadArgument";
    case affa::Result::LinkDown:     return "LinkDown";
    case affa::Result::Cancelled:    return "Cancelled";
    case affa::Result::Aborted:      return "Aborted";
  }
  return "?";
}

// ---------------------------------------------------------------------------
// The bring-up sequence — ONE render in flight at a time, and every step waits for the
// PANEL's verdict rather than for a guess about how long the panel takes
// ---------------------------------------------------------------------------
// WHY IT IS A STATE MACHINE AND NOT FOUR CALLS IN A ROW. Every render call ENQUEUES and
// returns immediately; the Result is an acceptance verdict, never a delivery one. Firing
// setPower/setText/setTime back to back therefore puts three messages in the queue and the
// panel gets the text microseconds after the power command — before the glass is lit, so
// the text is drawn into a display that is still coming up and you see nothing. It also
// wastes the queue: setText and setTime carry different RenderSlots, but a second setText
// behind the first would COALESCE and silently replace it.
//
// So: issue one, wait for onComplete, then move. The only hard-coded delay in the whole
// file is kWarmUpMs, and it is there because "the panel is lit" is not something the panel
// tells us.
constexpr uint32_t kWarmUpMs   = 750;    // 0.5-1 s; the panel needs a moment after power-on
constexpr uint32_t kAckWaitMs  = 4000;   // > AFFA_ACK_TIMEOUT_MS, so the library reports first
constexpr uint8_t  kMaxTries   = 5;
constexpr uint32_t kRetryMs    = 1500;

constexpr const char* kText = "SUCCESS";
constexpr const char* kTime = "1000";    // HHMM — four ASCII digits, so 10:00

enum class Step : uint8_t {
  WaitLink,    // no sync: the panel is not answering, nothing else is worth trying
  SendPower, AckPower,
  WarmUp,
  SendText,  AckText,
  SendTime,  AckTime,
  Done,
  GaveUp,
};

Step     g_step      = Step::WaitLink;
bool     g_autoRun   = true;
uint32_t g_req       = 0;      // the request we are waiting on
uint32_t g_deadline  = 0;
uint8_t  g_tries     = 0;
uint32_t g_runs      = 0;      // completed sequences since boot

// Written by onDone on the owned task, read by the FSM in loop(). 32-bit aligned scalars,
// so each read is atomic; the FSM's own timeout makes a missed hand-off self-healing rather
// than a hang, which is why this needs no lock.
volatile uint32_t g_ackReq = 0;
volatile uint8_t  g_ackRes = 0;
volatile uint32_t g_ackSeq = 0;
uint32_t          g_ackSeen = 0;

const char* stepName(Step s) {
  switch (s) {
    case Step::WaitLink:  return "wait-link";
    case Step::SendPower: case Step::AckPower: return "power";
    case Step::WarmUp:    return "warm-up";
    case Step::SendText:  case Step::AckText:  return "text";
    case Step::SendTime:  case Step::AckTime:  return "time";
    case Step::Done:      return "done";
    case Step::GaveUp:    return "gave-up";
  }
  return "?";
}

void onDone(affa::rtos::TxRequest req, affa::Result r, void*) {
  g_ackReq = static_cast<uint32_t>(req);
  g_ackRes = static_cast<uint8_t>(r);
  ++g_ackSeq;
}

void restart(const char* why) {
  logmsg("sequence restart: %s", why);
  g_step = Step::WaitLink;
  g_req = 0;
  g_tries = 0;
}

// true when the panel is answering. FuncsReg is deliberately NOT required: registration is
// lazy and happens ON the first render, so waiting for it here would wait for ever.
bool linked(const affa::rtos::Status& st) {
  return !affa::hasFlag(st.sync, affa::SyncState::Failed);
}

// Issue one render and arm the wait. Centralised so every step retries identically.
void issue(affa::rtos::TxRequest req, Step next, uint32_t now, const char* what) {
  if (req == affa::rtos::kNoRequest) {
    // The command queue refused us — it is full, or the task is not running. Neither is
    // worth spending an attempt on; come back shortly.
    logmsg("%s: command queue refused", what);
    g_deadline = now + kRetryMs;
    return;
  }
  g_req      = static_cast<uint32_t>(req);
  g_ackSeen  = g_ackSeq;          // ignore anything that completed before we asked
  g_deadline = now + kAckWaitMs;
  g_step     = next;
}

// 0 = still waiting, 1 = ours completed Ok, 2 = ours failed, 3 = timed out
uint8_t pollAck(uint32_t now, affa::Result& out) {
  while (g_ackSeen != g_ackSeq) {
    ++g_ackSeen;
    if (g_ackReq == g_req) {
      out = static_cast<affa::Result>(g_ackRes);
      return out == affa::Result::Ok ? 1 : 2;
    }
    // Somebody else's completion — the library makes renders of its own. Keep looking.
  }
  return affa::expired(now, g_deadline) ? 3 : 0;
}

// A failed step: spend an attempt, or give up and say so.
void stepFailed(const char* what, const char* why, Step retryAt, uint32_t now) {
  if (++g_tries >= kMaxTries) {
    logmsg("%s FAILED (%s) after %u tries - giving up", what, why,
           static_cast<unsigned>(g_tries));
    g_step = Step::GaveUp;
    return;
  }
  logmsg("%s failed (%s), retry %u/%u", what, why,
         static_cast<unsigned>(g_tries), static_cast<unsigned>(kMaxTries));
  g_deadline = now + kRetryMs;
  g_step     = retryAt;
}

void sequenceTick(uint32_t now) {
  const affa::rtos::Status st = g_task.status();

  // Losing sync invalidates everything behind us: the panel forgets its registration, and
  // it may well have been powered down. Start again from the top rather than carrying on
  // into a panel that is no longer the one we were talking to.
  if (g_step != Step::WaitLink && g_step != Step::GaveUp && !linked(st)) {
    restart("sync lost");
    return;
  }

  affa::Result r = affa::Result::Ok;

  switch (g_step) {
    case Step::WaitLink:
      if (!g_autoRun || !linked(st)) return;
      logmsg("link up (sync=0x%02X) - starting bring-up",
             static_cast<unsigned>(static_cast<uint8_t>(st.sync)));
      g_tries    = 0;
      g_deadline = 0;
      g_step     = Step::SendPower;
      return;

    // -- power on ----------------------------------------------------------
    case Step::SendPower:
      if (!affa::expired(now, g_deadline)) return;
      issue(g_task.setPower(true), Step::AckPower, now, "power");
      return;

    case Step::AckPower:
      switch (pollAck(now, r)) {
        case 0: return;
        case 1:
          // THE WAIT THE PANEL NEEDS. It has acknowledged the command; the glass is not lit
          // yet. Text drawn inside this window is drawn into a display that is still coming
          // up, and the usual conclusion is "setText does not work".
          logmsg("power on acknowledged - warming up %lu ms",
                 static_cast<unsigned long>(kWarmUpMs));
          g_deadline = now + kWarmUpMs;
          g_step     = Step::WarmUp;
          return;
        case 2: stepFailed("power", resultName(r), Step::SendPower, now); return;
        default: stepFailed("power", "no completion", Step::SendPower, now); return;
      }

    case Step::WarmUp:
      if (!affa::expired(now, g_deadline)) return;
      g_tries = 0;
      g_step  = Step::SendText;
      return;

    // -- the text ----------------------------------------------------------
    case Step::SendText:
      if (!affa::expired(now, g_deadline)) return;
      issue(g_task.setText(kText), Step::AckText, now, "text");
      return;

    case Step::AckText:
      switch (pollAck(now, r)) {
        case 0: return;
        case 1:
          logmsg("\"%s\" delivered", kText);
          g_tries    = 0;
          g_deadline = 0;
          g_step     = Step::SendTime;
          return;
        case 2: stepFailed("text", resultName(r), Step::SendText, now); return;
        default: stepFailed("text", "no completion", Step::SendText, now); return;
      }

    // -- the clock ---------------------------------------------------------
    case Step::SendTime:
      if (!affa::expired(now, g_deadline)) return;
      issue(g_task.setTime(kTime), Step::AckTime, now, "time");
      return;

    case Step::AckTime:
      switch (pollAck(now, r)) {
        case 0: return;
        case 1:
          ++g_runs;
          logmsg("clock set to %c%c:%c%c - bring-up COMPLETE (run %lu)",
                 kTime[0], kTime[1], kTime[2], kTime[3],
                 static_cast<unsigned long>(g_runs));
          g_step = Step::Done;
          return;
        case 2: stepFailed("time", resultName(r), Step::SendTime, now); return;
        default: stepFailed("time", "no completion", Step::SendTime, now); return;
      }

    case Step::Done:
    case Step::GaveUp:
      return;
  }
}

// ---------------------------------------------------------------------------
// The HTTP self-probe — the dead-man switch for the lockout described at the top
// ---------------------------------------------------------------------------
// lru_purge_enable is the fix; this is the proof that the fix is holding, and the recovery
// if something else ever takes the server down. The board opens a connection TO ITSELF and
// asks for /api/ping. If that fails three times running — three minutes of a server that is
// not accepting — it reboots, because a reboot always restores OTA and there is no cable.
//
// It cannot become a boot loop: it does nothing in the first two minutes of uptime, nothing
// while an OTA is in progress, and nothing when WiFi is not associated in the first place.
// Generous: a slow upload over a weak link is normal, and cutting one short is worse than
// waiting. Nothing depends on this being tight — it only has to be finite.
constexpr uint32_t kOtaAbandonMs = 180000;

constexpr uint32_t kProbeEveryMs = 60000;
constexpr uint32_t kProbeGraceMs = 120000;
constexpr uint8_t  kProbeFails   = 3;

uint32_t g_probeAt    = kProbeGraceMs;
uint8_t  g_probeFail  = 0;
uint32_t g_probeOk    = 0;
uint32_t g_probeBad   = 0;

bool probeSelf() {
  WiFiClient c;
  c.setTimeout(2);                       // seconds, for the Stream half of the API
  if (!c.connect(WiFi.localIP(), 80)) return false;
  c.print("GET /api/ping HTTP/1.1\r\nHost: self\r\nConnection: close\r\n\r\n");

  char     buf[192];
  size_t   n  = 0;
  const uint32_t t0 = millis();
  while (millis() - t0 < 2500 && n < sizeof(buf) - 1) {
    while (c.available() && n < sizeof(buf) - 1) buf[n++] = static_cast<char>(c.read());
    buf[n] = 0;
    if (strstr(buf, "pong")) { c.stop(); return true; }
    if (!c.connected() && !c.available()) break;
    delay(10);
  }
  c.stop();
  return false;
}

void httpWatchdogTick(uint32_t now) {
  if (g_otaRunning || g_rebootAt) return;
  if (!WiFi.isConnected()) return;              // SoftAP fallback: nothing to probe to
  if (!affa::expired(now, g_probeAt)) return;
  g_probeAt = now + kProbeEveryMs;

  if (probeSelf()) {
    ++g_probeOk;
    if (g_probeFail) logmsg("http probe recovered after %u failures",
                            static_cast<unsigned>(g_probeFail));
    g_probeFail = 0;
    return;
  }

  ++g_probeBad;
  ++g_probeFail;
  logmsg("http probe FAILED (%u/%u) - server is not accepting",
         static_cast<unsigned>(g_probeFail), static_cast<unsigned>(kProbeFails));
  if (g_probeFail >= kProbeFails) {
    logmsg("http dead - rebooting to restore OTA");
    g_rebootAt = now + 300;
  }
}

// ---------------------------------------------------------------------------
// JSON — fixed buffer, no heap, bounds-checked. esp_http_server runs every handler on one
// task, so one shared buffer is safe and costs no per-task stack.
// ---------------------------------------------------------------------------
char   g_out[6144];
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
  const affa::rtos::Status st = g_task.status();
  const auto drv  = g_link.driverState();
  const auto hl   = g_display.linkHealth();

  jclear();
  jf("{");

  jf("\"uptimeMs\":%lu,", static_cast<unsigned long>(millis()));
  jf("\"heapFree\":%lu,", static_cast<unsigned long>(ESP.getFreeHeap()));
  jf("\"bootQuiet\":%s,", g_bootQuiet ? "true" : "false");
  jf("\"canUp\":%s,", g_canUp ? "true" : "false");
  jf("\"txGate\":%s,", g_link.txEnabled() ? "true" : "false");

  // The sequence: what the board is actually doing about SUCCESS / 10:00.
  jf("\"seq\":{\"step\":"); jstr(stepName(g_step));
  jf(",\"auto\":%s,\"tries\":%u,\"runs\":%lu,", g_autoRun ? "true" : "false",
     static_cast<unsigned>(g_tries), static_cast<unsigned long>(g_runs));
  jf("\"text\":"); jstr(kText);
  jf(",\"time\":"); jstr(kTime);
  jf("},");

  // Do we HEAR anything? These count frames that reached the library, which is the
  // question — the driver's rxFrames can move while nothing ever decodes.
  jf("\"heard\":{\"rx\":%lu,\"tx\":%lu,\"lastRxMs\":%lu,\"sinceRxMs\":%ld},",
     static_cast<unsigned long>(g_rxSeen), static_cast<unsigned long>(g_txSeen),
     static_cast<unsigned long>(g_lastRxMs),
     g_lastRxMs ? static_cast<long>(millis() - g_lastRxMs) : -1L);

  jf("\"sync\":{\"state\":%u,\"synced\":%s,\"registered\":%s,\"busy\":%s,"
     "\"queued\":%u,\"cmdQueued\":%u,\"lastResult\":",
     static_cast<unsigned>(static_cast<uint8_t>(st.sync)),
     linked(st) ? "true" : "false",
     st.registered ? "true" : "false",
     st.busy ? "true" : "false",
     static_cast<unsigned>(st.queued), static_cast<unsigned>(st.cmdQueued));
  jstr(resultName(st.lastResult));
  jf("},");

  // The owned task's own health. queueDropped > 0 or foreignPolls > 0 means the
  // application is misusing the library; pollLateMaxUs means something is blocking it.
  jf("\"task\":{\"running\":%s,\"iterations\":%lu,\"pollLateMaxUs\":%lu,"
     "\"pollLateAtMs\":%lu,\"queueDropped\":%lu,\"foreignPolls\":%lu,\"stackFree\":%lu},",
     st.running ? "true" : "false",
     static_cast<unsigned long>(st.iterations),
     static_cast<unsigned long>(st.pollLateMaxUs),
     static_cast<unsigned long>(st.pollLateAtMs),
     static_cast<unsigned long>(st.queueDropped),
     static_cast<unsigned long>(st.foreignPolls),
     static_cast<unsigned long>(st.stackFreeBytes));

  // The controller. A SINGLE SAMPLE OF THIS IS NOT THE TRUTH — with forceRecovery armed, a
  // continuously failing bus cycles counters-zero -> rxErr 129 -> busErr climbing -> bus-off
  // -> reinstall -> counters-zero. Read `flaps` and `downMs` for the shape, busErr for the
  // cause, and sample several times over ~40 s before concluding anything.
  jf("\"drv\":{\"valid\":%s,\"state\":%u,\"txErr\":%lu,\"rxErr\":%lu,\"busErr\":%lu,"
     "\"arbLost\":%lu,\"rxMissed\":%lu,\"restarts\":%lu},",
     drv.valid ? "true" : "false", static_cast<unsigned>(drv.state),
     static_cast<unsigned long>(drv.txErr), static_cast<unsigned long>(drv.rxErr),
     static_cast<unsigned long>(drv.busErr), static_cast<unsigned long>(drv.arbLost),
     static_cast<unsigned long>(drv.rxMissed),
     static_cast<unsigned long>(g_link.restarts()));

  jf("\"health\":{\"recoveries\":%lu,\"failures\":%lu,\"flaps\":%lu,\"downMs\":%lu},",
     static_cast<unsigned long>(hl.recoveries), static_cast<unsigned long>(hl.failures),
     static_cast<unsigned long>(hl.flaps), static_cast<unsigned long>(hl.downMs));

  jf("\"http\":{\"probeOk\":%lu,\"probeBad\":%lu,\"probeFail\":%u},",
     static_cast<unsigned long>(g_probeOk), static_cast<unsigned long>(g_probeBad),
     static_cast<unsigned>(g_probeFail));

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
  if (want <= 0) want = 32;
  if (want > static_cast<long>(kFrameRing)) want = kFrameRing;

  // STATIC, not a local: this is 1.5 kB and esp_http_server's task stack is 4 kB. The same
  // argument that makes g_out safe makes this safe — every handler runs on that one task,
  // so two snapshots are never taken at once. jLog()'s copy is 3.2 kB and overflowed the
  // stack outright, which shows up as a handler that returns an empty body and no error.
  static FrameRec snap[kFrameRing];
  size_t   head;
  uint32_t seq;
  portENTER_CRITICAL(&g_frameMux);
  memcpy(snap, g_frames, sizeof(snap));
  head = g_frameHead;
  seq  = g_frameSeq;
  portEXIT_CRITICAL(&g_frameMux);

  const size_t have = (seq < kFrameRing) ? static_cast<size_t>(seq) : kFrameRing;
  const size_t n    = (static_cast<size_t>(want) < have) ? static_cast<size_t>(want) : have;

  jclear();
  jf("{\"total\":%lu,\"f\":[", static_cast<unsigned long>(seq));
  // Oldest first, so the ring reads like a trace.
  for (size_t i = 0; i < n; ++i) {
    const size_t idx = (head + kFrameRing - n + i) % kFrameRing;
    const FrameRec& r = snap[idx];
    if (i) jf(",");
    jf("[%lu,\"%s\",\"%03X\",%u,\"", static_cast<unsigned long>(r.ms),
       r.dir ? "TX" : "RX", static_cast<unsigned>(r.id), static_cast<unsigned>(r.len));
    for (uint8_t b = 0; b < r.len && b < 8; ++b)
      jf("%s%02X", b ? " " : "", static_cast<unsigned>(r.d[b]));
    jf("\"]");
  }
  jf("]}");
}

void jLog() {
  static LogRec snap[kLogRing];         // static for the reason spelled out in jFrames()
  size_t  head;
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

// ---------------------------------------------------------------------------
// The page. IT DOES NOT POLL ON A TIMER, and that is deliberate — see THE LOCKOUT at the
// top of this file. Refresh is a button; auto-refresh is a checkbox, off by default, and
// it runs at 3 s rather than 1 s when you do turn it on.
// ---------------------------------------------------------------------------
const char kPage[] PROGMEM = R"HTML(<!doctype html><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>AffaDisplay bring-up</title>
<style>
body{font:13px ui-monospace,Consolas,monospace;margin:0;padding:12px;background:#111;color:#ddd}
h1{font:600 15px system-ui;margin:0 0 10px}
button{font:inherit;padding:5px 9px;margin:2px;background:#243;color:#cfc;border:1px solid #486;border-radius:4px;cursor:pointer}
button.w{background:#432;color:#fda;border-color:#754}
a{color:#8cf}
pre{background:#000;padding:8px;border-radius:4px;overflow:auto;max-height:38vh;margin:8px 0}
label{margin-left:10px}
</style>
<h1>AffaDisplay — bring-up</h1>
<div>
<button onclick=go('/api/run')>run sequence</button>
<button onclick=go('/api/power?on=1')>power on</button>
<button onclick=go('/api/power?on=0')>power off</button>
<button onclick=go('/api/text?t=SUCCESS')>SUCCESS</button>
<button onclick=go('/api/time?t=1000')>10:00</button>
</div>
<div>
<button class=w onclick=go('/api/txgate?on=0')>tx gate SHUT</button>
<button class=w onclick=go('/api/txgate?on=1')>tx gate open</button>
<button class=w onclick=go('/api/quiet?on=1')>reboot QUIET</button>
<button class=w onclick=go('/api/quiet?on=0')>reboot normal</button>
<button class=w onclick=go('/api/reboot')>reboot</button>
<a href=/update>OTA</a>
</div>
<div>
<button onclick=tick()>refresh</button>
<label><input type=checkbox id=a> auto (3s)</label>
</div>
<pre id=s>press refresh</pre>
<pre id=f></pre>
<pre id=l></pre>
<script>
const $=i=>document.getElementById(i)
async function go(u){try{await fetch(u)}catch(e){};setTimeout(tick,250)}
async function tick(){
 try{
  $('s').textContent=JSON.stringify(await (await fetch('/api/status')).json(),null,1)
  const f=await (await fetch('/api/frames?n=40')).json()
  $('f').textContent='frames total '+f.total+'\n'+f.f.map(r=>r[0]+'  '+r[1]+' '+r[2]+'  '+r[4]).join('\n')
  const g=await (await fetch('/api/log')).json()
  $('l').textContent=g.l.map(r=>r[0]+'  '+r[1]).join('\n')
 }catch(e){$('s').textContent='fetch failed: '+e}
}
setInterval(()=>{if($('a').checked)tick()},3000)
</script>)HTML";

long pnum(PsychicRequest* r, const char* k, long dflt) {
  if (!r->hasParam(k)) return dflt;
  const String v = r->getParam(k)->value();
  if (!v.length()) return dflt;
  return strtol(v.c_str(), nullptr, 0);
}
void pstr(PsychicRequest* r, const char* k, char* out, size_t n, const char* dflt = "") {
  const char* src = dflt;
  String v;
  if (r->hasParam(k)) { v = r->getParam(k)->value(); src = v.c_str(); }
  snprintf(out, n, "%s", src);
}

esp_err_t replyJson(PsychicRequest* r) { return r->reply(200, "application/json", g_out); }

esp_err_t replyReq(PsychicRequest* r, affa::rtos::TxRequest req) {
  jclear();
  jf("{\"req\":%lu,\"accepted\":%s}", static_cast<unsigned long>(req),
     req == affa::rtos::kNoRequest ? "false" : "true");
  return replyJson(r);
}

// Persist "come up with the transmitter shut". The gate itself is a runtime switch, but
// having it shut FROM THE FIRST POLL is what you want on a bus you do not trust yet: no
// heartbeat, no sync request, nothing of ours on the wire until you open it.
void setBootQuiet(bool on) {
  Preferences p;
  if (p.begin(kOwnNamespace, /*readOnly=*/false)) {
    p.putUChar("quiet", on ? 1 : 0);
    p.end();
  }
}

void routes() {
  g_server.on("/", HTTP_GET, [](PsychicRequest* r) {
    return r->reply(200, "text/html", kPage);
  });

  // The self-probe's target. Deliberately the cheapest possible handler: if THIS cannot be
  // served, the server is not serving anything.
  g_server.on("/api/ping", HTTP_GET, [](PsychicRequest* r) {
    return r->reply(200, "text/plain", "pong");
  });

  g_server.on("/api/status", HTTP_GET, [](PsychicRequest* r) {
    jStatus();
    return replyJson(r);
  });

  g_server.on("/api/frames", HTTP_GET, [](PsychicRequest* r) {
    jFrames(pnum(r, "n", 32));
    return replyJson(r);
  });

  g_server.on("/api/log", HTTP_GET, [](PsychicRequest* r) {
    jLog();
    return replyJson(r);
  });

  // Re-arm the whole sequence, from power-on. `auto=0` leaves it parked instead.
  g_server.on("/api/run", HTTP_GET, [](PsychicRequest* r) {
    g_autoRun = pnum(r, "auto", 1) != 0;
    restart("requested over http");
    jclear();
    jf("{\"auto\":%s}", g_autoRun ? "true" : "false");
    return replyJson(r);
  });

  g_server.on("/api/power", HTTP_GET, [](PsychicRequest* r) {
    return replyReq(r, g_task.setPower(pnum(r, "on", 1) != 0));
  });

  g_server.on("/api/text", HTTP_GET, [](PsychicRequest* r) {
    char t[32];
    pstr(r, "t", t, sizeof(t), kText);
    return replyReq(r, g_task.setText(t));
  });

  g_server.on("/api/time", HTTP_GET, [](PsychicRequest* r) {
    char t[8];
    pstr(r, "t", t, sizeof(t), kTime);
    return replyReq(r, g_task.setTime(t));
  });

  // THE TEST THAT SETTLES WHOSE FAULT A BAD BUS IS. Shut our own transmitter and keep
  // watching: if busErr still climbs and rxFrames stays 0 while we are silent, we cannot be
  // causing it and the fault is on the wire. A software gate, not a driver mode change —
  // the controller still ACKs the other node, which a two-node bus requires.
  g_server.on("/api/txgate", HTTP_GET, [](PsychicRequest* r) {
    const bool on = pnum(r, "on", 1) != 0;
    g_link.setTxEnabled(on);
    logmsg("tx gate %s", on ? "open" : "SHUT");
    jclear();
    jf("{\"txGate\":%s}", on ? "true" : "false");
    return replyJson(r);
  });

  // The same gate, but latched from the very first poll — so not even the initial heartbeat
  // and sync request go out. Persisted, hence the reboot.
  g_server.on("/api/quiet", HTTP_GET, [](PsychicRequest* r) {
    const bool on = pnum(r, "on", 1) != 0;
    setBootQuiet(on);
    jclear();
    jf("{\"quietNextBoot\":%s,\"rebooting\":true}", on ? "true" : "false");
    g_rebootAt = millis() + 400;
    return replyJson(r);
  });

  g_server.on("/api/reboot", HTTP_GET, [](PsychicRequest* r) {
    jclear();
    jf("{\"rebooting\":true}");
    g_rebootAt = millis() + 300;
    return replyJson(r);
  });
}

// ---------------------------------------------------------------------------
void startNetwork() {
  // Read-only open of the namespace the bench board already has credentials in. Read-only
  // matters: an NVS WRITE stalls CAN reception outright for the duration of the flash write
  // (the TWAI ISR is not in IRAM), and this example must never be why they change.
  Preferences p;
  String ssid, pass;
  if (p.begin(kWifiNamespace, /*readOnly=*/true)) {
    ssid = p.getString("ssid", "");
    pass = p.getString("pass", "");
    p.end();
  }

  WiFi.persistent(false);
  WiFi.setSleep(true);          // the C3's single radio interleaves WiFi with CAN timing

  bool sta = false;
  if (ssid.length()) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());
    const uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < kStaJoinMs) delay(100);
    sta = (WiFi.status() == WL_CONNECTED);
  }
  if (!sta) {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(kApSsid, kApPass);
  }
  if (MDNS.begin(kMdnsName)) MDNS.addService("http", "tcp", 80);

  const String ip = sta ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  Serial.printf("\n[wifi] %s ip=%s  console http://%s/  OTA http://%s/update  mdns %s.local\n",
                sta ? "STA" : "AP (fallback)", ip.c_str(), ip.c_str(), ip.c_str(), kMdnsName);
}

void startHttp() {
  // ------------------------------------------------------------------------
  // THE THREE LINES THAT KEEP THIS BOARD REACHABLE. See THE LOCKOUT at the top.
  // ------------------------------------------------------------------------
  // With lru_purge_enable false — esp_http_server's default, and what PsychicHttp leaves it
  // at unless ENABLE_ASYNC is defined — a full socket table is PERMANENT: httpd stops
  // accepting and never resumes. With it true, a new connection always evicts the
  // least-recently-used one, so the worst a misbehaving client can do is get dropped.
  g_server.config.lru_purge_enable = true;
  g_server.config.max_open_sockets = 7;
  // And do not let a half-open connection hold its slot for the default 5 s each way.
  g_server.config.recv_wait_timeout = 3;
  g_server.config.send_wait_timeout = 3;
  g_server.config.max_uri_handlers  = 24;
  // The default 4 kB leaves very little room once a handler builds a body. Headroom here is
  // cheap; a handler that overflows it dies silently, returning an empty response.
  g_server.config.stack_size        = 8192;

  g_server.listen(80);
  routes();

  // An OTA write stalls CAN reception outright: the TWAI ISR is not in IRAM, so a flash
  // write looks exactly like a dead panel. Gate our transmitter for the duration — we are
  // not shouting at a bus we cannot hear — and expect PeerLost plus a resync after reboot.
  //
  // DELIBERATELY NOT g_task.stop() HERE: stop() joins, and joining waits for a message
  // already on the wire, i.e. up to two ACK timeouts of a stalled upload handler on the
  // only route back into this board.
  ElegantOTA.onStart([]() {
    g_otaRunning = true;
    g_otaSince   = millis();
    g_autoRun    = false;
    if (g_canUp) g_link.setTxEnabled(false);
    logmsg("ota started - CAN TX gated, RX stalls on flash writes");
  });
  ElegantOTA.onEnd([](bool ok) { logmsg("ota %s", ok ? "ok, rebooting" : "FAILED"); });
  ElegantOTA.begin(&g_server);
}

}  // namespace

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);      // a detached USB host must not be able to stall a write
  delay(300);                    // the application may sleep; the library may not
  Serial.println("\nAffaDisplay 01_bringup");

  // 1. NETWORK AND OTA FIRST, ALWAYS. This board has no cable and no buttons: everything
  //    after this line is allowed to fail without costing us the way back in.
  startNetwork();
  startHttp();

  // 2. The boot mode, read before the controller is touched.
  {
    Preferences p;
    if (p.begin(kOwnNamespace, /*readOnly=*/true)) {
      g_bootQuiet = p.getUChar("quiet", 0) != 0;
      p.end();
    }
  }

  // 3. CAN. A failure is reported, never fatal — the console stays up to say so.
  //    ALWAYS LinkMode::Normal: begin() refuses ListenOnly outright and would return false,
  //    leaving the board with no link at all. The software gate below is the substitute.
  g_canUp = g_link.begin(kPins, kBitrate, kForceRecoveryMs);
  logmsg("can %s", g_canUp ? "up" : "DID NOT COME UP");

  // 4. THE ORDER IS THE CONTRACT: callbacks, then begin(), then start(). start() refuses a
  //    display that was never begun, and callbacks installed after the task is running
  //    would miss whatever it had already delivered.
  g_display.onFrame(&onTap, nullptr);
  g_task.onComplete(&onDone, nullptr);
  g_display.begin();

  // Shut the gate BEFORE the task starts polling, so a quiet boot is quiet from the very
  // first iteration rather than after one heartbeat has already gone out.
  if (g_canUp && g_bootQuiet) g_link.setTxEnabled(false);

  if (!g_task.start(g_display))
    logmsg("AffaTask::start() FAILED - nothing will be polled");

  // Gated, the sequence can never succeed: nothing of ours reaches the wire. Park it rather
  // than letting it burn its five attempts and report a failure that is expected.
  if (g_bootQuiet) g_autoRun = false;

  logmsg("up: can=%d task=%d quiet=%d", g_canUp ? 1 : 0,
         g_task.running() ? 1 : 0, g_bootQuiet ? 1 : 0);
}

void loop() {
  // THERE IS NO poll() IN THIS FUNCTION, and that is the point. The library polls itself on
  // its own task at AFFA_TASK_PERIOD_MS. Anything added here — a web write, an NVS commit,
  // a blocking probe — costs a slow screen and nothing else: no timed-out ACK, no lost
  // registration, no missed key. That is the property that makes this hard to break.
  const uint32_t now = millis();

  // AN UPDATE THAT NEVER ARRIVES MUST NOT LEAVE THE BOARD MUTED.
  // GET /ota/start gates the transmitter and parks the sequence, and ElegantOTA's onEnd only
  // fires if an upload actually happens. A start with no upload — an aborted flash, a
  // dropped connection — therefore left the board with txGate shut and auto off for ever,
  // with nothing to undo it but a reboot. Observed on the bench 2026-07-29. One timeout
  // fixes it; there is no state to unwind.
  if (g_otaRunning && affa::expired(now, g_otaSince + kOtaAbandonMs)) {
    g_otaRunning = false;
    g_otaSince   = 0;
    if (g_canUp) g_link.setTxEnabled(true);
    g_autoRun = true;
    restart("ota started but never completed");
    logmsg("ota abandoned after %lu s - TX gate reopened",
           static_cast<unsigned long>(kOtaAbandonMs / 1000));
  }

  sequenceTick(now);
  httpWatchdogTick(now);

  ElegantOTA.loop();
  if (g_rebootAt && affa::expired(now, g_rebootAt)) ESP.restart();

  // loopTask runs at priority 1 and IDLE at 0, so a loop() that never blocks starves IDLE
  // and the single-core C3 panics with "Task watchdog got triggered (IDLE)".
  delay(10);
}
