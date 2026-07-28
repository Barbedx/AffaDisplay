// 19_owned_task — the library owns the poll task. WiFi, OTA, and a screen that never stops.
//
// THIS IS THE REFERENCE FOR THE 0.3.0 THREADING MODEL, and it is deliberately the simplest
// file in examples/ that still runs unattended on a real bus:
//
//   * `loop()` NEVER calls poll(). It renders, and it is allowed to be slow.
//   * HTTP handlers call the render API DIRECTLY, from the web server's task, with no
//     mailbox, no mutex and no hand-off. That mailbox is what this release deletes.
//   * The panel shows three rows moving at three different rates, one of which is a clock
//     drawn by hand, and a popup counter every 20 s that proves the overlay layers.
//
// WHAT TO WATCH ON /api/status. `foreignPolls` is 0 (nobody but the owned task polls),
// `pollLateMaxUs` stays near the 2 ms period (nothing blocks it), and `queueDropped` stays
// 0 (the wire keeps up with what we ask of it). Those three numbers are the whole design.
//
// THE THREE ROWS, AND WHY THE FULLSCREEN PRIMITIVE:
//   10:04:33                  the clock, drawn by hand, 1 Hz
//   AFFA DISPLAY 0.3.0 …      marquee, 300 ms per symbol
//   THE LIBRARY OWNS …        marquee, 700 ms per symbol
// showFullscreenText is 0x21 mode 0x05 — three equal lines, no row tags. Measured on a real
// Carminat 2026-07-28: any other full-screen render REPLACES it, so it needs no teardown;
// the POPUP is the true overlay, survives a redraw underneath, and only hidePopup() clears
// it. This example does both at once and you can watch the base tick under the popup.
//
//   pio run -e ex19_owned_task            build
//   pio run -e ex19_owned_task -t upload  first flash over USB
//   thereafter: http://<ip>/update        ElegantOTA

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <PsychicHttp.h>
#include <ElegantOTA.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include <AffaDisplay.h>

#if !AFFA_PANEL_CARMINAT
#  error "19_owned_task needs -D AFFA_PANEL_CARMINAT=1"
#endif
#if !AFFA_ENABLE_TASK
#  error "19_owned_task IS the owned-task example; it needs -D AFFA_ENABLE_TASK=1"
#endif
#if !AFFA_ENABLE_MARQUEE
#  error "19_owned_task scrolls two of its three rows; it needs AFFA_ENABLE_MARQUEE=1"
#endif

namespace {

// ---------------------------------------------------------------------------
// Board and network
// ---------------------------------------------------------------------------
// ESP32-C3 SuperMini on the bench rig: rx = GPIO4, tx = GPIO3. The named struct is what
// stops the swap from becoming a silent bus with no error anywhere.
constexpr affa::CanPins kPins{ .rx = GPIO_NUM_4, .tx = GPIO_NUM_3 };
constexpr uint32_t      kBitrate = 500000;

// 250 ms, MEASURED — not 0 and not 2000. 0 leaves a bus-off in TWAI_STATE_STOPPED for ever
// (rxFrames 0, no errors climbing, looks exactly like a dead transceiver); 2000 accumulates
// inside setup() and the board answers its first HTTP request three minutes after boot.
constexpr uint32_t kForceRecoveryMs = 250;

constexpr const char* kNvsNamespace = "megaopen";   // read-only: ssid / pass
constexpr const char* kApSsid       = "AffaOwned";
constexpr const char* kApPass       = "affaowned";
constexpr const char* kMdnsName     = "affaowned";
constexpr uint32_t    kStaJoinMs    = 15000;

struct ArduinoClock final : affa::IClock {
  uint32_t millis() const override { return ::millis(); }
};

affa::Esp32CanLink    g_link;
ArduinoClock          g_clock;
affa::CarminatDisplay g_display(g_link, g_clock);
affa::rtos::AffaTask  g_task;          // <- the library's own poll task
PsychicHttpServer     g_server;

bool     g_canUp    = false;
uint32_t g_rebootAt = 0;

// ---------------------------------------------------------------------------
// The log ring
// ---------------------------------------------------------------------------
// WRITTEN FROM MORE THAN ONE TASK now — the library logs from the owned task, the HTTP
// handlers log from theirs — so it takes a spinlock. That is the one new obligation the
// owned task creates for an application, and it is three lines.
//
// A portMUX critical section, not a mutex: it is a memcpy of 96 bytes, it must never block
// the owned task, and a spinlock held for microseconds cannot.
constexpr uint16_t kLogRing = 64;
struct LogRec {
  uint32_t ms;
  uint8_t  level;
  char     tag[10];
  char     msg[86];
};
LogRec   g_logs[kLogRing];
uint32_t g_logHead = 0;
portMUX_TYPE g_logMux = portMUX_INITIALIZER_UNLOCKED;

void pushLog(uint8_t level, const char* tag, const char* msg) {
  portENTER_CRITICAL(&g_logMux);
  LogRec& r = g_logs[g_logHead % kLogRing];
  r.ms    = millis();
  r.level = level;
  snprintf(r.tag, sizeof(r.tag), "%s", tag ? tag : "");
  snprintf(r.msg, sizeof(r.msg), "%s", msg ? msg : "");
  ++g_logHead;
  portEXIT_CRITICAL(&g_logMux);

  // `if (Serial)` IS LOAD-BEARING. HWCDC::write queues into a ring the USB host drains;
  // with no host attached the ring fills and the write blocks for Serial's TX timeout —
  // and this can be called from the owned task, which must not stall. The bench rig has no
  // serial cable, so on it this branch is never taken.
  if (Serial)
    Serial.printf("[%lu] %u %s: %s\n", static_cast<unsigned long>(r.ms),
                  static_cast<unsigned>(level), r.tag, r.msg);
}

void logmsg(uint8_t level, const char* tag, const char* fmt, ...)
     __attribute__((format(printf, 3, 4)));
void logmsg(uint8_t level, const char* tag, const char* fmt, ...) {
  char buf[86];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  pushLog(level, tag, buf);
}

// A sink is a LEAF: it never calls back into the library. This one appends and prints.
struct RingSink final : affa::ILogSink {
  void write(uint8_t level, const char* tag, const char* msg) override {
    pushLog(level, tag, msg);
  }
};
RingSink g_sink;

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
// The screen
// ---------------------------------------------------------------------------
// Three rows, three clocks. The two marquees derive their position from the wall clock
// rather than accumulating steps (widget::Marquee::windowAt(now)), so a skipped render
// costs nothing: we sample later and get the position the clock says, not a backlog.
constexpr uint8_t  kCols   = 24;     // the Carminat line is wider; leave margin
constexpr uint8_t  kGap    = 4;
constexpr uint32_t kRowAMs = 300;    // row 2 — one symbol per 300 ms
constexpr uint32_t kRowBMs = 700;    // row 3 — one symbol per 700 ms

affa::widget::Marquee g_rowA{ affa::widget::MarqueeGeometry{kCols, kGap, kRowAMs} };
affa::widget::Marquee g_rowB{ affa::widget::MarqueeGeometry{kCols, kGap, kRowBMs} };

// THE CLOCK IS DRAWN BY HAND, as a line of text, and it is NOT setTime(). setTime() writes
// the panel's own clock widget (the one thing a Carminat draws with the display powered
// off); this is a row of our screen that happens to contain the time. Both are in this
// example on purpose — they are different mechanisms and they are both worth seeing.
//
// 10:00:00 at boot, so the hand-drawn row and the panel's own clock (setTime("1000"), see
// below) agree on the glass.
constexpr uint32_t kClockOriginSec = 10u * 3600u;

void clockLine(char* out, size_t n, uint32_t nowMs) {
  const uint32_t s = (kClockOriginSec + nowMs / 1000u) % 86400u;
  snprintf(out, n, "%02lu:%02lu:%02lu", static_cast<unsigned long>(s / 3600u),
           static_cast<unsigned long>((s / 60u) % 60u),
           static_cast<unsigned long>(s % 60u));
}

// THE THREE COUNTERS ARE NOT "OK AND NOT OK", and conflating them cost an hour of a soak
// run. A completion that is not Ok has two entirely different meanings:
//
//   Aborted / Cancelled — WE did that. A newer render of the same RenderSlot superseded a
//     queued one (coalescing, working as designed), or a resync cancelled the queue. It is
//     the library reporting our own decision back to us, and on a screen that repaints
//     continuously it is NORMAL and expected.
//   anything else — the PANEL or the LINK did that: Timeout, SendFailed, NoSync, LinkDown.
//     One of these is a real fault and worth waking someone for.
//
// Counting both as "failed" makes a healthy repaint loop look broken, which is exactly the
// kind of number that gets ignored and then hides the real one.
struct Screen {
  bool     run       = true;
  uint32_t renders   = 0;
  uint32_t refused   = 0;      // kNoRequest at post: the command queue was full
  uint32_t delivered = 0;      // completed Ok
  uint32_t stale     = 0;      // completed Aborted/Cancelled — self-inflicted, expected
  uint32_t failed    = 0;      // completed anything else — the panel or the link
  affa::Result lastFail = affa::Result::Ok;
  uint32_t lastFailMs = 0;
  char     lastL1[32] = {0};
  char     lastL2[32] = {0};
  char     lastL3[32] = {0};
} g_screen;

// The popup cycle: up for 3 s once every 20 s, with a counter that ticks while it is up.
//
// THE BASE KEEPS REDRAWING UNDERNEATH, deliberately. That is the confirmed overlay
// behaviour (WIRE-SPEC §8.6) and this is what demonstrates it: the seconds in row 1 keep
// moving at the line ends the popup does not cover.
constexpr uint32_t kPopupPeriodMs = 20000;
constexpr uint32_t kPopupHoldMs   = 3000;

struct Popup {
  bool     up       = false;
  uint32_t nextMs   = kPopupPeriodMs;   // first popup 20 s after boot
  uint32_t downMs   = 0;
  uint32_t tickMs   = 0;
  uint32_t counter  = 0;
  uint32_t shows    = 0;
  uint32_t hides    = 0;
} g_pop;

// ---------------------------------------------------------------------------
// Callbacks — EVERY ONE OF THESE RUNS ON THE LIBRARY'S OWN TASK
// ---------------------------------------------------------------------------
// Which is the entire point, and the entire obligation: none of them may block. They
// append to a ring, stamp a number, and return.
uint32_t g_keyFrameUs = 0;    // key frame seen on the wire (Layer-0 tap)
uint32_t g_keyToCbUs  = 0;    // ... to KeyCb. THE number this release exists to protect.
uint32_t g_keyCount   = 0;

void onTap(const affa::Frame& f, affa::Direction d, void*) {
  if (d != affa::Direction::Rx) return;
  // The same `03 89` guard the library uses, so this cannot stamp on `70 A3..`.
  if (f.id == affa::carminat::kIdKeyPressed && f.len >= 4 && f.data[0] == 0x03 &&
      f.data[1] == 0x89)
    g_keyFrameUs = micros();
}

void onKey(affa::Key k, affa::KeyEdge e, void*) {
  if (g_keyFrameUs) { g_keyToCbUs = micros() - g_keyFrameUs; g_keyFrameUs = 0; }
  ++g_keyCount;
  logmsg(3, "key", "0x%04X %s -> cb in %lu us", static_cast<unsigned>(k),
         e == affa::KeyEdge::Hold ? "hold" : "click",
         static_cast<unsigned long>(g_keyToCbUs));
}

// The link clock. THE ONE THING A CARMINAT DRAWS WITH THE DISPLAY POWERED OFF IS THE CLOCK,
// so setting it on every fresh sync turns the glass itself into a link light: 10:00 on the
// panel means the handshake completed and registration latched.
//
// Re-armed on sync LOSS and on a FAILED DELIVERY, not cleared at enqueue: an acceptance
// verdict is not a delivery verdict, and a render that completed Timeout because the panel
// was not acknowledging yet would otherwise leave the clock frozen while the link came good
// underneath it.
constexpr const char* kLinkClock = "1000";
bool                  g_clockPending = true;
affa::rtos::TxRequest g_clockReq     = affa::rtos::kNoRequest;

void onSyncChanged(affa::SyncState s, void*) {
  logmsg(3, "sync", "state 0x%02X synced=%d registered=%d", static_cast<unsigned>(s),
         g_display.synced() ? 1 : 0, g_display.registered() ? 1 : 0);
  if (!g_display.synced()) {
    g_clockPending = true;
    // Anything we thought was on the glass is gone with the registration.
    g_screen.lastL1[0] = g_screen.lastL2[0] = g_screen.lastL3[0] = '\0';
  }
}

void onDone(affa::rtos::TxRequest req, affa::Result r, void*) {
  if (r == affa::Result::Ok) {
    ++g_screen.delivered;
  } else if (r == affa::Result::Aborted || r == affa::Result::Cancelled) {
    ++g_screen.stale;                       // ours: coalescing or a resync. Not a fault.
  } else {
    ++g_screen.failed;
    g_screen.lastFail   = r;
    g_screen.lastFailMs = millis();
    logmsg(2, "render", "completed %s", resultName(r));
  }

  if (req != affa::rtos::kNoRequest && req == g_clockReq) {
    g_clockReq = affa::rtos::kNoRequest;
    if (r != affa::Result::Ok) {
      g_clockPending = true;
      logmsg(2, "clock", "link clock not delivered (%s) — will retry", resultName(r));
    } else {
      logmsg(3, "clock", "link clock delivered");
    }
  }
}

// ---------------------------------------------------------------------------
// The application — runs in loop(), which is now allowed to be slow
// ---------------------------------------------------------------------------
void clockTick() {
  if (!g_clockPending) return;
  const affa::rtos::Status s = g_task.status();
  if (hasFlag(s.sync, affa::SyncState::Failed)) return;
  const affa::rtos::TxRequest req = g_task.setTime(kLinkClock);
  if (req == affa::rtos::kNoRequest) return;      // retry next pass
  g_clockReq     = req;
  g_clockPending = false;
}

void screenTick(uint32_t now) {
  if (!g_screen.run) return;

  // RENDER AT THE RATE THE WIRE SUSTAINS, NOT THE RATE THE TEXT MOVES. A fullscreen is 14
  // frames, each waiting on a panel ACK — about 190 ms — so enqueueing on every step would
  // not make the panel faster: coalescing would supersede the queued screen, completions
  // would arrive Aborted, and the glass would look frozen while the counters said "busy".
  const affa::rtos::Status s = g_task.status();
  if (s.busy || hasFlag(s.sync, affa::SyncState::Failed)) return;

  char l1[32], l2[32], l3[32];
  clockLine(l1, sizeof(l1), now);
  g_rowA.window(g_rowA.windowAt(now), l2, sizeof(l2));
  g_rowB.window(g_rowB.windowAt(now), l3, sizeof(l3));

  // Say nothing when nothing moved. This is what keeps a 500 kbit/s bus mostly idle.
  if (strcmp(l1, g_screen.lastL1) == 0 && strcmp(l2, g_screen.lastL2) == 0 &&
      strcmp(l3, g_screen.lastL3) == 0)
    return;

  // No hideFullscreenText() between screens: each one replaces the last. Confirmed on a
  // real panel, and the reason this loop is three calls and not six.
  if (g_task.showFullscreenText(l1, l2, l3) == affa::rtos::kNoRequest) {
    ++g_screen.refused;
    return;
  }
  ++g_screen.renders;
  strcpy(g_screen.lastL1, l1);
  strcpy(g_screen.lastL2, l2);
  strcpy(g_screen.lastL3, l3);
}

void popupTick(uint32_t now) {
  if (!g_screen.run) return;

  if (!g_pop.up && affa::expired(now, g_pop.nextMs)) {
    g_pop.up     = true;
    g_pop.downMs = now + kPopupHoldMs;
    g_pop.tickMs = now;                       // render immediately
    g_pop.nextMs = now + kPopupPeriodMs;
    ++g_pop.shows;
  }

  if (g_pop.up && affa::expired(now, g_pop.tickMs)) {
    g_pop.tickMs = now + 1000;
    ++g_pop.counter;
    char buf[16];
    snprintf(buf, sizeof(buf), "CNT %lu", static_cast<unsigned long>(g_pop.counter));
    // Priority is not needed and not used: the popup is its own RenderSlot, so it never
    // coalesces against the fullscreen underneath and never has to overtake it either.
    if (g_task.showPopupText(buf) == affa::rtos::kNoRequest) ++g_screen.refused;
  }

  if (g_pop.up && affa::expired(now, g_pop.downMs)) {
    g_pop.up = false;
    ++g_pop.hides;
    // THE POPUP IS THE ONE THING HERE THAT NEEDS AN EXPLICIT CLOSE. No auto-revert has been
    // observed on this panel, so its lifetime is the application's.
    if (g_task.hidePopup() == affa::rtos::kNoRequest) ++g_screen.refused;
  }
}

// ---------------------------------------------------------------------------
// JSON — fixed buffer, no heap, bounds-checked
// ---------------------------------------------------------------------------
// ONE SHARED BUFFER, no lock: esp_http_server runs every handler on ONE task, so two
// bodies are never built at once. On the handler's stack it would be 4 kB of stack per
// HTTP task instead.
char   g_out[4096];
size_t g_outN = 0;

size_t jroom() { return sizeof(g_out) - 1 - g_outN; }
void   jclear() { g_outN = 0; g_out[0] = 0; }
void jf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void jf(const char* fmt, ...) {
  const size_t room = jroom();
  if (room == 0) return;
  va_list ap;
  va_start(ap, fmt);
  const int w = vsnprintf(g_out + g_outN, room + 1, fmt, ap);
  va_end(ap);
  if (w <= 0) return;
  g_outN += (static_cast<size_t>(w) > room) ? room : static_cast<size_t>(w);
  g_out[g_outN] = 0;
}
void jstr(const char* s) {
  jf("\"");
  for (; s && *s; ++s) {
    const unsigned char c = static_cast<unsigned char>(*s);
    if (c == '"' || c == '\\')      jf("\\%c", c);
    else if (c < 0x20 || c >= 0x7F) jf("\\u%04X", static_cast<unsigned>(c));
    else                            jf("%c", c);
  }
  jf("\"");
}
void jkv(const char* k, const char* v) { jf("\"%s\":", k); jstr(v); }

void jStatus() {
  // ONE SNAPSHOT, taken once. status() is published by the owned task under a seqlock, so
  // every field below is from the same moment — which the direct accessors could not
  // promise to a reader on this task.
  const affa::rtos::Status s = g_task.status();

  jclear();
  jf("{\"uptimeMs\":%lu,\"heap\":%lu,\"minHeap\":%lu,",
     static_cast<unsigned long>(millis()),
     static_cast<unsigned long>(ESP.getFreeHeap()),
     static_cast<unsigned long>(ESP.getMinFreeHeap()));

  jf("\"wifi\":{");
  jkv("mode", WiFi.isConnected() ? "sta" : "ap"); jf(",");
  {
    const String ip = WiFi.isConnected() ? WiFi.localIP().toString()
                                         : WiFi.softAPIP().toString();
    jkv("ip", ip.c_str()); jf(",");
  }
  jf("\"rssi\":%d},", WiFi.isConnected() ? static_cast<int>(WiFi.RSSI()) : 0);

  jf("\"canUp\":%s,", g_canUp ? "true" : "false");

  // The three numbers that ARE the owned-task design.
  jf("\"task\":{\"running\":%s,\"iterations\":%lu,\"pollLateMaxUs\":%lu,"
     "\"pollLateAtMs\":%lu,"
     "\"queueDropped\":%lu,\"foreignPolls\":%lu,\"cmdQueued\":%u,\"stackFree\":%lu,",
     s.running ? "true" : "false",
     static_cast<unsigned long>(s.iterations),
     static_cast<unsigned long>(s.pollLateMaxUs),
     static_cast<unsigned long>(s.pollLateAtMs),
     static_cast<unsigned long>(s.queueDropped),
     static_cast<unsigned long>(s.foreignPolls),
     static_cast<unsigned>(s.cmdQueued),
     static_cast<unsigned long>(s.stackFreeBytes));
  jf("\"periodMs\":%u,", static_cast<unsigned>(AFFA_TASK_PERIOD_MS));
  jkv("lastResult", resultName(s.lastResult)); jf("},");

  jf("\"sync\":{\"state\":%u,\"synced\":%s,\"registered\":%s,\"busy\":%s,\"queued\":%u},",
     static_cast<unsigned>(s.sync),
     hasFlag(s.sync, affa::SyncState::Failed) ? "false" : "true",
     s.registered ? "true" : "false",
     s.busy ? "true" : "false",
     static_cast<unsigned>(s.queued));

  jf("\"link\":{\"rxFrames\":%lu,\"txFrames\":%lu,\"txDropped\":%lu,\"ringOverflow\":%lu,"
     "\"txErr\":%lu,\"rxErr\":%lu,\"txFailed\":%lu},",
     static_cast<unsigned long>(s.stats.rxFrames),
     static_cast<unsigned long>(s.stats.txFrames),
     static_cast<unsigned long>(s.stats.txDropped),
     static_cast<unsigned long>(s.stats.ringOverflow),
     static_cast<unsigned long>(s.stats.txErr),
     static_cast<unsigned long>(s.stats.rxErr),
     static_cast<unsigned long>(s.stats.txFailed));

  jf("\"screen\":{\"run\":%s,\"renders\":%lu,\"delivered\":%lu,\"stale\":%lu,"
     "\"failed\":%lu,\"refused\":%lu,\"lastFailMs\":%lu,",
     g_screen.run ? "true" : "false",
     static_cast<unsigned long>(g_screen.renders),
     static_cast<unsigned long>(g_screen.delivered),
     static_cast<unsigned long>(g_screen.stale),
     static_cast<unsigned long>(g_screen.failed),
     static_cast<unsigned long>(g_screen.refused),
     static_cast<unsigned long>(g_screen.lastFailMs));
  jkv("lastFail", resultName(g_screen.lastFail)); jf(",");
  jkv("l1", g_screen.lastL1); jf(",");
  jkv("l2", g_screen.lastL2); jf(",");
  jkv("l3", g_screen.lastL3); jf("},");

  jf("\"popup\":{\"up\":%s,\"counter\":%lu,\"shows\":%lu,\"hides\":%lu},",
     g_pop.up ? "true" : "false",
     static_cast<unsigned long>(g_pop.counter),
     static_cast<unsigned long>(g_pop.shows),
     static_cast<unsigned long>(g_pop.hides));

  jf("\"keys\":{\"count\":%lu,\"lastKeyToCbUs\":%lu}}",
     static_cast<unsigned long>(g_keyCount),
     static_cast<unsigned long>(g_keyToCbUs));
}

void jLog(long n) {
  if (n <= 0 || n > kLogRing) n = 24;
  jclear();
  portENTER_CRITICAL(&g_logMux);
  const uint32_t head  = g_logHead;
  const uint32_t avail = head < kLogRing ? head : kLogRing;
  const uint32_t take  = (static_cast<uint32_t>(n) < avail) ? static_cast<uint32_t>(n)
                                                            : avail;
  static LogRec snap[kLogRing];
  for (uint32_t i = 0; i < take; ++i) snap[i] = g_logs[(head - take + i) % kLogRing];
  portEXIT_CRITICAL(&g_logMux);

  jf("{\"head\":%lu,\"l\":[", static_cast<unsigned long>(head));
  for (uint32_t i = 0; i < take; ++i) {
    if (jroom() < 200) break;
    jf("%s[%lu,%u,", i ? "," : "", static_cast<unsigned long>(snap[i].ms),
       static_cast<unsigned>(snap[i].level));
    jstr(snap[i].tag); jf(","); jstr(snap[i].msg); jf("]");
  }
  jf("]}");
}

// ---------------------------------------------------------------------------
// HTTP — the handlers call the library DIRECTLY. That is the headline.
// ---------------------------------------------------------------------------
// In 0.2.x every one of these had to post into a one-slot mailbox and block until loop()
// picked it up, because the library was unlocked and loop() was the only task allowed to
// touch it. All of that is deleted: AffaTask::setText() copies the call into the command
// queue and returns a handle, from whatever task happens to be running.
const char kPage[] PROGMEM = R"HTML(<!doctype html><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>AffaDisplay 0.3.0 — owned task</title>
<style>
body{font:14px system-ui,sans-serif;margin:0;padding:12px;background:#111;color:#ddd}
h1{font-size:16px;margin:0 0 8px}button,input{font:inherit;padding:6px 10px;margin:2px}
button{background:#264;color:#dfd;border:1px solid #385;border-radius:4px}
pre{background:#000;padding:8px;border-radius:4px;overflow:auto;max-height:40vh}
.g{color:#8f8}.r{color:#f88}
</style>
<h1>AffaDisplay 0.3.0 — the library owns the poll task</h1>
<div>
<button onclick=go('/api/demo?run=1')>demo on</button>
<button onclick=go('/api/demo?run=0')>demo off</button>
<button onclick=go('/api/state?on=1')>display on</button>
<button onclick=go('/api/state?on=0')>display off</button>
<button onclick=go('/api/resync')>resync</button>
<button onclick=go('/api/reboot')>reboot</button>
<a href=/update style=color:#8cf>OTA</a>
</div>
<div><input id=t value="HELLO"><button onclick=go('/api/text?t='+encodeURIComponent(t.value))>setText</button>
<button onclick=go('/api/popup?t='+encodeURIComponent(t.value))>popup</button></div>
<pre id=s>…</pre><pre id=l></pre>
<script>
async function go(u){await fetch(u);tick()}
async function tick(){
 try{
  const s=await (await fetch('/api/status')).json();
  document.getElementById('s').textContent=JSON.stringify(s,null,1);
  const g=await (await fetch('/api/log?n=20')).json();
  document.getElementById('l').textContent=g.l.map(r=>r[0]+' '+r[2]+': '+r[3]).join('\n');
 }catch(e){}
}
setInterval(tick,1000);tick();
</script>)HTML";

long pnum(PsychicRequest* r, const char* k, long dflt) {
  if (!r->hasParam(k)) return dflt;
  const String v = r->getParam(k)->value();
  if (!v.length()) return dflt;
  return strtol(v.c_str(), nullptr, 0);        // base 0: "0x60" and "96" both work
}
// The String must OUTLIVE the c_str() it is copied from, which is why it is a named local
// and not a temporary inside the snprintf argument list.
void pstr(PsychicRequest* r, const char* k, char* out, size_t n, const char* dflt = "") {
  const char* src = dflt;
  String v;
  if (r->hasParam(k)) { v = r->getParam(k)->value(); src = v.c_str(); }
  snprintf(out, n, "%s", src);
}

esp_err_t replyJson(PsychicRequest* r) { return r->reply(200, "application/json", g_out); }

esp_err_t replyRequest(PsychicRequest* r, affa::rtos::TxRequest req) {
  jclear();
  jf("{\"req\":%u,\"accepted\":%s}", static_cast<unsigned>(req),
     req == affa::rtos::kNoRequest ? "false" : "true");
  return replyJson(r);
}

void routes() {
  g_server.on("/", HTTP_GET, [](PsychicRequest* r) {
    return r->reply(200, "text/html", kPage);
  });

  g_server.on("/api/status", HTTP_GET, [](PsychicRequest* r) {
    jStatus();
    return replyJson(r);
  });

  g_server.on("/api/log", HTTP_GET, [](PsychicRequest* r) {
    const long n = pnum(r, "n", 24);
    jLog(n);
    return replyJson(r);
  });

  // setText REPLACES the three-row screen — a full-screen-class render, and the panel keeps
  // whatever was drawn last. Leaving the demo running puts the rows straight back, which is
  // exactly the interruption-and-recovery this endpoint is for.
  g_server.on("/api/text", HTTP_GET, [](PsychicRequest* r) {
    char t[40];
    pstr(r, "t", t, sizeof(t), "AFFA");
    logmsg(3, "http", "setText %s", t);
    return replyRequest(r, g_task.setText(t));
  });

  g_server.on("/api/popup", HTTP_GET, [](PsychicRequest* r) {
    char t[40];
    pstr(r, "t", t, sizeof(t), "POPUP");
    return replyRequest(r, g_task.showPopupText(t));
  });

  g_server.on("/api/popup/hide", HTTP_GET, [](PsychicRequest* r) {
    return replyRequest(r, g_task.hidePopup());
  });

  // Display power. WITH THE PANEL OFF A RENDER STILL COMPLETES Ok and shows nothing
  // (WIRE-SPEC §8.3) — which is why this endpoint exists in a test rig: it is the cheapest
  // way to produce a delivered-but-invisible render on purpose.
  g_server.on("/api/state", HTTP_GET, [](PsychicRequest* r) {
    const bool on = pnum(r, "on", 1) != 0;
    logmsg(3, "http", "setPower %d", on ? 1 : 0);
    return replyRequest(r, g_task.setPower(on));
  });

  g_server.on("/api/demo", HTTP_GET, [](PsychicRequest* r) {
    g_screen.run = pnum(r, "run", 1) != 0;
    if (g_screen.run) {
      g_screen.lastL1[0] = g_screen.lastL2[0] = g_screen.lastL3[0] = '\0';
    }
    logmsg(3, "http", "demo %s", g_screen.run ? "on" : "off");
    jclear();
    jf("{\"run\":%s}", g_screen.run ? "true" : "false");
    return replyJson(r);
  });

  // Tears the link down and builds it again from begin(), ON THE OWNED TASK. The panel
  // forgets us, the 0x70 registration burst goes out again with the next render, and the
  // clock re-arms — the whole recovery path, on demand.
  g_server.on("/api/resync", HTTP_GET, [](PsychicRequest* r) {
    logmsg(2, "http", "resync requested");
    g_clockPending = true;
    return replyRequest(r, g_task.resync());
  });

  g_server.on("/api/reboot", HTTP_GET, [](PsychicRequest* r) {
    g_rebootAt = millis() + 400;
    jclear();
    jf("{\"reboot\":true}");
    return replyJson(r);
  });
}

void startNetwork() {
  // Read-only open of the namespace the bench board already has credentials in. Read-only
  // matters: an NVS WRITE stops CAN reception outright for the duration of the flash write
  // (the TWAI ISR is not in IRAM), and this example must never be the reason they change.
  Preferences p;
  String ssid, pass;
  if (p.begin(kNvsNamespace, /*readOnly=*/true)) {
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

  Serial.printf("\n[wifi] %s ip=%s  console http://%s/  OTA http://%s/update\n",
                sta ? "STA" : "AP",
                sta ? WiFi.localIP().toString().c_str() : WiFi.softAPIP().toString().c_str(),
                sta ? WiFi.localIP().toString().c_str() : WiFi.softAPIP().toString().c_str(),
                sta ? WiFi.localIP().toString().c_str() : WiFi.softAPIP().toString().c_str());
}

void startHttp() {
  g_server.config.max_uri_handlers = 24;
  g_server.listen(80);
  routes();

  // An OTA write stops CAN reception outright: the TWAI ISR is not in IRAM, so a flash
  // write looks exactly like a dead panel. Gate our transmitter for the duration — we are
  // not shouting at a bus we cannot hear — and expect PeerLost plus a resync after the
  // reboot. This is also why AFFA_PEER_TIMEOUT_MS must stay above the longest flash write.
  //
  // DELIBERATELY NOT g_task.stop() HERE. stop() joins, and joining waits for a message
  // already on the wire to finish — up to two ACK timeouts. Four seconds of a stalled
  // upload handler on the only route back into this board is a worse trade than leaving a
  // 2 ms task polling a gated link that the reboot is about to reset anyway.
  ElegantOTA.onStart([]() {
    pushLog(2, "ota", "update started — CAN TX gated off, RX stalls on flash writes");
    g_screen.run = false;
    if (g_canUp) g_link.setTxEnabled(false);
  });
  ElegantOTA.onEnd([](bool ok) {
    pushLog(ok ? 3 : 1, "ota", ok ? "update ok, rebooting" : "update FAILED");
  });
  ElegantOTA.begin(&g_server);
}

}  // namespace

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);      // a detached USB host must not be able to stall a write
  delay(300);                    // the application may sleep; the library may not
  Serial.println("\nAffaDisplay 19_owned_task");

  // 1. NETWORK FIRST, ALWAYS. The bench rig has no serial cable and no buttons: everything
  //    after this can fail and still leave a way back in.
  startNetwork();
  startHttp();

  // 2. CAN. A failure is reported, never fatal.
  g_canUp = g_link.begin(kPins, kBitrate, kForceRecoveryMs);
  if (!g_canUp)
    Serial.println("[can] controller did not come up — check pins, transceiver, "
                   "termination");

  // 3. THE ORDER HERE IS THE CONTRACT: log sink and callbacks, then begin(), then start().
  //    start() refuses a display that was never begun, and callbacks installed after the
  //    task is running would miss whatever it already delivered.
  g_display.setLogSink(&g_sink);
  g_display.onFrame(&onTap, nullptr);
  g_display.onKey(&onKey, nullptr);
  g_display.onSync(&onSyncChanged, nullptr);
  g_task.onComplete(&onDone, nullptr);

  g_display.begin();
  if (g_canUp) g_link.setTxEnabled(true);

  if (!g_task.start(g_display)) {
    // Reported, not fatal: the console stays up so the failure can be read off /api/status
    // instead of being guessed at from a board that does nothing.
    logmsg(1, "boot", "AffaTask::start() FAILED — nothing will be polled");
  }

  // setText takes `now`: the marquee position is DERIVED from the clock, never accumulated,
  // so a render we skip costs nothing — we sample later and get where the wall clock says
  // the text should be, not a backlog of steps.
  const uint32_t now = millis();
  g_rowA.setText("AFFA 0.3.0 - THE LIBRARY OWNS THE POLL TASK", now);
  g_rowB.setText("THREE ROWS, THREE CLOCKS, ONE TASK", now);

  logmsg(3, "boot", "up: can=%d task=%d", g_canUp ? 1 : 0, g_task.running() ? 1 : 0);
}

void loop() {
  // THERE IS NO poll() IN THIS FUNCTION, and that is the entire example. loop() renders;
  // the library polls itself on its own task at AFFA_TASK_PERIOD_MS. A blocking call added
  // here — a web write, a BLE service call, an NVS commit — costs a slow screen and nothing
  // else: no timed-out ACK, no lost registration, no missed key.
  const uint32_t now = millis();

  clockTick();
  screenTick(now);
  popupTick(now);

  ElegantOTA.loop();
  if (g_rebootAt && affa::expired(now, g_rebootAt)) ESP.restart();

  // loopTask runs at priority 1 and IDLE at 0, so a loop() that never blocks starves IDLE
  // and the single-core C3 panics with "Task watchdog got triggered (IDLE)". Ten
  // milliseconds, not one: this loop only renders now, and the library's own task is at
  // priority 2 and does not care how long we sleep.
  delay(10);
}
