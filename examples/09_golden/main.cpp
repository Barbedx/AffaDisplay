// 09_golden — the example to copy.
//
// Everything a real application wants, using nothing but the AffaDisplay API, over
// collin80's esp32_can. If you are starting a project against a Carminat display, start
// here and delete what you do not need.
//
// WHAT IT DOES
//   * three independent scrolling rows, each on its own speed, running for ever
//   * a web console: pause/resume, per-row speed, set the text of any row, set the clock
//   * a live wire log — every CAN frame in and out, newest last, identical frames coalesced
//   * status: handshake step, registration, driver counters, transmit queue
//   * WiFi manager: joins your network, or falls back to its own AP so you can configure it
//   * OTA, so the cable is only ever needed once
//
// AND THE POINT: THE LIBRARY OWNS THE PROTOCOL, NOT THIS FILE.
// There is no handshake here. No `61 11`, no `B0`, no ISO-TP, no flow control, no `74`. The
// library does all of it on its own task; this file calls setText/setTime/showFullscreenText
// and reads status. Compare examples/07_cantime and 08_rows3, which implement the same
// protocol by hand — they are ~600 lines each and every one of those lines is a place to get
// the wire wrong.
//
// NOTHING HERE BLOCKS. Every render call ENQUEUES and returns immediately. The value it
// returns is an ACCEPTANCE verdict ("did this go into the queue"), never a delivery verdict —
// delivery arrives later on the completion callback. That is the single most common mistake
// against this API, so it is worth stating twice: `setText()` returning Ok does NOT mean the
// panel drew it.
//
// Board: ESP32 DevKit V1 — CRX -> GPIO5, CTX -> GPIO4.
//   pio run -e ex09_golden -t upload --upload-port COM5
//   then http://<ip>/  and  http://<ip>/update

#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <PsychicHttp.h>
#include <ElegantOTA.h>

#include <AffaDisplay.h>

#include <cstdarg>
#include <cstdio>

#if !AFFA_PANEL_CARMINAT
#  error "09_golden is a Carminat example: build with -D AFFA_PANEL_CARMINAT=1"
#endif
#if !AFFA_ENABLE_TASK
#  error "09_golden uses the library-owned poll task: build with -D AFFA_ENABLE_TASK=1"
#endif

namespace {

constexpr const char* kVersion = "09_golden 1.0.0";

#ifndef AFFA_CAN_RX
#  define AFFA_CAN_RX 5
#endif
#ifndef AFFA_CAN_TX
#  define AFFA_CAN_TX 4
#endif
constexpr gpio_num_t kRxPin   = static_cast<gpio_num_t>(AFFA_CAN_RX);
constexpr gpio_num_t kTxPin   = static_cast<gpio_num_t>(AFFA_CAN_TX);
constexpr uint32_t   kBitrate = 500000;

constexpr size_t kRowWidth = 20;   // visible cells per row
constexpr size_t kRowGap   = 6;    // blank cells before the text wraps round

// ---------------------------------------------------------------------------
// The library. Three objects and a task — that is the whole CAN/protocol stack.
// ---------------------------------------------------------------------------
struct ArduinoClock final : affa::IClock {
  uint32_t millis() const override { return ::millis(); }
};

affa::CanCommonLink   g_link;     // ICanLink over esp32_can
ArduinoClock          g_clock;
affa::CarminatDisplay g_display(g_link, g_clock);
affa::rtos::AffaTask  g_task;     // the library polls itself; loop() never calls poll()

// ---------------------------------------------------------------------------
// Log ring
// ---------------------------------------------------------------------------
struct LogRec { uint32_t ms = 0; char msg[100] = {0}; };
constexpr size_t kLogRing = 64;
LogRec       g_log[kLogRing];
size_t       g_logHead = 0;
portMUX_TYPE g_logMux = portMUX_INITIALIZER_UNLOCKED;

void logmsg(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void logmsg(const char* fmt, ...) {
  char buf[100];
  va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
  portENTER_CRITICAL(&g_logMux);
  LogRec& r = g_log[g_logHead];
  r.ms = millis();
  snprintf(r.msg, sizeof(r.msg), "%s", buf);
  g_logHead = (g_logHead + 1) % kLogRing;
  portEXIT_CRITICAL(&g_logMux);
  if (Serial) Serial.printf("[%lu] %s\n", static_cast<unsigned long>(r.ms), buf);
}

// ---------------------------------------------------------------------------
// Wire log — the library's Layer-0 frame tap, both directions, in wire order
// ---------------------------------------------------------------------------
// onFrame() is on the path of EVERY frame, so this does nothing but fill a ring. Never
// render from a tap: a render inside the transmit path is how a queue eats itself.
struct WireRec {
  uint32_t firstMs = 0, lastMs = 0, count = 0;
  uint16_t id = 0;
  uint8_t  tx = 0, d[8] = {0};
};
constexpr size_t kWireRing = 120;
WireRec      g_wire[kWireRing];
size_t       g_wireHead = 0;
uint32_t     g_wireSeq = 0;
portMUX_TYPE g_wireMux = portMUX_INITIALIZER_UNLOCKED;

bool g_logHeartbeats = false;   // the 1 Hz B9/69 pair floods a ring you want for a handshake

void onWire(const affa::Frame& f, affa::Direction d, void*) {
  const bool tx = (d == affa::Direction::Tx);
  // HEARTBEATS ARE 4 FRAMES A SECOND AND THEY NEVER COALESCE, because B9 and 69 alternate.
  // Left in, they evict an entire 120-row ring in thirty seconds and every post-mortem shows
  // nothing but the heartbeat. Filtered by default; the toggle is on the console.
  if (!g_logHeartbeats && f.len >= 1 &&
      ((f.id == 0x3AF && f.data[0] == 0xB9) || (f.id == 0x3CF && f.data[0] == 0x69)))
    return;
  portENTER_CRITICAL(&g_wireMux);
  ++g_wireSeq;
  WireRec& last = g_wire[(g_wireHead + kWireRing - 1) % kWireRing];
  if (last.count && last.tx == (tx ? 1 : 0) && last.id == f.id &&
      memcmp(last.d, f.data, 8) == 0) {
    ++last.count;
    last.lastMs = millis();
  } else {
    WireRec& w = g_wire[g_wireHead];
    w.firstMs = w.lastMs = millis();
    w.count = 1;
    w.id = static_cast<uint16_t>(f.id);
    w.tx = tx ? 1 : 0;
    memcpy(w.d, f.data, 8);
    g_wireHead = (g_wireHead + 1) % kWireRing;
  }
  portEXIT_CRITICAL(&g_wireMux);
}

// ---------------------------------------------------------------------------
// Three marquees
// ---------------------------------------------------------------------------
struct Row {
  char     text[64] = {0};
  uint32_t periodMs = 250;    // 0 = static
  uint32_t nextAt   = 0;
  size_t   offset   = 0;
};
Row  g_row[3];
bool g_paused = false;
char g_shown[3][kRowWidth + 2] = {{0}, {0}, {0}};

void windowOf(const Row& r, char* out, size_t n) {
  const size_t len = strlen(r.text);
  if (!len) { out[0] = 0; return; }
  if (r.periodMs == 0 && len <= kRowWidth) { snprintf(out, n, "%s", r.text); return; }
  const size_t span = len + kRowGap;
  size_t w = 0;
  while (w < kRowWidth && w + 1 < n) {
    const size_t i = (r.offset + w) % span;
    out[w] = (i < len) ? r.text[i] : ' ';
    ++w;
  }
  out[w] = 0;
}

// ---------------------------------------------------------------------------
// Completion callback — delivery verdicts arrive here, on the library's task
// ---------------------------------------------------------------------------
volatile uint32_t g_okCount = 0, g_failCount = 0;
uint32_t g_refused = 0;
volatile uint8_t  g_lastResult = 0;
volatile bool     g_screenBusy = false;

// THE GLASS HAS TO BE TURNED ON BEFORE ANYTHING DRAWN ON IT IS VISIBLE, and the panel does
// not tell you which state it is in. `03 52 09` is the first application payload in all four
// OEM captures — never a screen, never a clock — and a fullscreen sent to a dark panel is
// ACKed exactly like one sent to a lit panel. Found on the bench: a flawless ring (announce,
// burst, registration, 400 ms settle, 14-frame transfer, terminal 74) and nothing on screen,
// because this example painted rows without ever sending power-on.
enum class Stage : uint8_t { NeedPower, WarmUp, Live };
Stage    g_stage      = Stage::NeedPower;
uint32_t g_warmUntil  = 0;
constexpr uint32_t kWarmUpMs = 750;   // the panel does not announce that its glass is lit

void onDone(affa::rtos::TxRequest, affa::Result r, void*) {
  g_lastResult = static_cast<uint8_t>(r);
  if (r == affa::Result::Ok) ++g_okCount; else ++g_failCount;
  g_screenBusy = false;
  // The power render completing is what starts the warm-up clock — NOT the moment it was
  // enqueued. On a busy bus the ACK can take longer than the warm-up itself, so a deadline
  // armed at enqueue time has already expired when the glass actually begins to light.
  if (g_stage == Stage::NeedPower && r == affa::Result::Ok) {
    g_stage     = Stage::WarmUp;
    g_warmUntil = millis() + kWarmUpMs;
  }
}

// ---------------------------------------------------------------------------
// The screen loop
// ---------------------------------------------------------------------------
// REPAINT ON CHANGE, WITH ONE SCREEN IN FLIGHT. A fullscreen is fourteen acknowledged
// frames — the wire carries roughly five to eight a second — so painting on a timer queues
// faster than it drains and the display falls further behind reality every minute. Painting
// only when the previous one COMPLETED and the visible text actually MOVED is self-limiting
// and cannot lag.
void pumpRows(uint32_t now) {
  // Power on, wait for its ACK, let the glass light, and only then draw. Same order as
  // 03_hello and as every OEM capture.
  if (g_stage == Stage::NeedPower) {
    if (g_screenBusy) return;
    if (g_task.setPower(true) != affa::rtos::kNoRequest) {
      g_screenBusy = true;
      logmsg("display ON queued - waiting for its ACK, then %lu ms warm-up",
             static_cast<unsigned long>(kWarmUpMs));
    }
    return;
  }
  if (g_stage == Stage::WarmUp) {
    if (static_cast<int32_t>(now - g_warmUntil) < 0) return;
    g_stage = Stage::Live;
    logmsg("glass warm - rows are live");
  }

  if (!g_paused) {
    for (auto& r : g_row) {
      if (r.periodMs == 0 || static_cast<int32_t>(now - r.nextAt) < 0) continue;
      r.nextAt = now + r.periodMs;
      const size_t span = strlen(r.text) + kRowGap;
      if (span) r.offset = (r.offset + 1) % span;
    }
  }

  if (g_screenBusy) return;

  char w[3][kRowWidth + 2];
  for (int i = 0; i < 3; ++i) windowOf(g_row[i], w[i], sizeof(w[i]));
  if (!strcmp(w[0], g_shown[0]) && !strcmp(w[1], g_shown[1]) && !strcmp(w[2], g_shown[2]))
    return;

  // ONE CALL. No ISO-TP, no flow control, no ACK handling — the library owns all of it.
  if (g_task.showFullscreenText(w[0], w[1], w[2]) != affa::rtos::kNoRequest) {
    g_screenBusy = true;
    for (int i = 0; i < 3; ++i) snprintf(g_shown[i], sizeof(g_shown[i]), "%s", w[i]);
    return;
  }
  // REFUSED, AND SAYING SO. A silent refusal here is a frozen screen with every counter
  // looking healthy — which is exactly how this presented on the bench: `screens ok 1` and
  // rows that never moved again. Rate-limited so a persistent refusal cannot itself flood.
  ++g_refused;
  if (g_refused == 1 || (g_refused % 200) == 0)
    logmsg("showFullscreenText REFUSED x%lu (task queue full?)",
           static_cast<unsigned long>(g_refused));
}

// ---------------------------------------------------------------------------
// Web console — plain text in a <pre>, meta refresh. No websockets, no JSON.
// ---------------------------------------------------------------------------
PsychicHttpServer g_server;
Preferences       g_prefs;
constexpr const char* kCfgNs = "golden";

void saveRows() {
  Preferences p;
  if (!p.begin(kCfgNs, false)) return;
  for (int i = 0; i < 3; ++i) {
    char k[8];
    snprintf(k, sizeof(k), "t%d", i); p.putString(k, g_row[i].text);
    snprintf(k, sizeof(k), "p%d", i); p.putUInt(k, g_row[i].periodMs);
  }
  p.putBool("paused", g_paused);
  p.end();
}

void loadRows() {
  Preferences p;
  if (!p.begin(kCfgNs, true)) return;
  for (int i = 0; i < 3; ++i) {
    char k[8];
    snprintf(k, sizeof(k), "t%d", i);
    const String t = p.getString(k, "");
    if (t.length()) snprintf(g_row[i].text, sizeof(g_row[i].text), "%s", t.c_str());
    snprintf(k, sizeof(k), "p%d", i);
    g_row[i].periodMs = p.getUInt(k, g_row[i].periodMs);
  }
  g_paused = p.getBool("paused", false);
  p.end();
}

String statusText() {
  const affa::rtos::Status st = g_task.status();
  const affa::CanCommonLink::Driver drv = g_link.driver();
  const char* dstate = !drv.valid ? "n/a"
                     : drv.state == 0 ? "STOPPED"
                     : drv.state == 1 ? "RUNNING"
                     : drv.state == 2 ? "BUS-OFF" : "RECOVERING";
  char b[720];
  snprintf(b, sizeof(b),
           "%s   %s %s   img %.8s\nup %lus\n\n"
           "sync    0x%02X  %s%s%s\n"
           "screens ok %lu  failed %lu  lastResult %u  inFlight %s\n"
           "rows    %s\n"
           "driver  %s  txErr %lu rxErr %lu busErr %lu arbLost %lu\n"
           "        rxMissed %lu  queued rx %lu tx %lu  ringOverflow %lu\n"
           "pins    rx=GPIO%d tx=GPIO%d @ %lu bit/s   (esp32_can via CanCommonLink)\n",
           kVersion, __DATE__, __TIME__, ESP.getSketchMD5().c_str(),
           static_cast<unsigned long>(millis() / 1000),
           static_cast<unsigned>(st.sync),
           affa::hasFlag(st.sync, affa::SyncState::Failed) ? "FAILED " : "",
           affa::hasFlag(st.sync, affa::SyncState::PeerAlive) ? "PEER_ALIVE " : "",
           affa::hasFlag(st.sync, affa::SyncState::FuncsReg) ? "REGISTERED" : "",
           static_cast<unsigned long>(g_okCount), static_cast<unsigned long>(g_failCount),
           static_cast<unsigned>(g_lastResult), g_screenBusy ? "yes" : "no",
           g_paused ? "PAUSED" : "running",
           dstate,
           static_cast<unsigned long>(drv.txErr), static_cast<unsigned long>(drv.rxErr),
           static_cast<unsigned long>(drv.busErr), static_cast<unsigned long>(drv.arbLost),
           static_cast<unsigned long>(drv.rxMissed),
           static_cast<unsigned long>(drv.queuedRx), static_cast<unsigned long>(drv.queuedTx),
           static_cast<unsigned long>(g_link.overflow()),
           static_cast<int>(kRxPin), static_cast<int>(kTxPin),
           static_cast<unsigned long>(kBitrate));
  return String(b);
}

String wireText() {
  WireRec snap[kWireRing];
  size_t head; uint32_t seq;
  portENTER_CRITICAL(&g_wireMux);
  memcpy(snap, g_wire, sizeof(snap));
  head = g_wireHead; seq = g_wireSeq;
  portEXIT_CRITICAL(&g_wireMux);

  // Only the newest rows. A response sized to the whole ring is a multi-kilobyte allocation
  // on the HTTP task, and when it fails the page goes BLANK — which looks exactly like a
  // dead bus, at the moment you most need to read it.
  constexpr size_t kShow = 34;
  size_t total = 0;
  for (size_t i = 0; i < kWireRing; ++i) if (snap[(head + i) % kWireRing].count) ++total;
  const size_t skip = (total > kShow) ? total - kShow : 0;

  String out;
  out.reserve(kShow * 64 + 128);
  char line[112];
  snprintf(line, sizeof(line), "%lu frames seen, showing last %u\n"
                               "   first    last   xN dir  id  data\n",
           static_cast<unsigned long>(seq), static_cast<unsigned>(total - skip));
  out += line;
  size_t seen = 0;
  for (size_t i = 0; i < kWireRing; ++i) {
    const WireRec& w = snap[(head + i) % kWireRing];
    if (!w.count) continue;
    if (seen++ < skip) continue;
    int n = snprintf(line, sizeof(line), "%8lu %7lu %4lu  %s %03X ",
                     static_cast<unsigned long>(w.firstMs),
                     static_cast<unsigned long>(w.lastMs),
                     static_cast<unsigned long>(w.count),
                     w.tx ? "TX" : "RX", static_cast<unsigned>(w.id));
    for (uint8_t b = 0; b < 8; ++b)
      n += snprintf(line + n, sizeof(line) - n, " %02X", static_cast<unsigned>(w.d[b]));
    out += line; out += '\n';
  }
  return out;
}

// ACTION ENDPOINTS MUST NOT 301. PsychicRequest::redirect() sends 301 Moved Permanently,
// which browsers cache FOR EVER: the first click works, and every click after it is served
// from cache without ever reaching the board. The button then looks broken while the
// endpoint behind it is perfectly fine. 303 See Other is the correct code for "I did the
// thing, now go look at this page", and it is explicitly not cacheable.
esp_err_t seeOther(PsychicRequest* r) {
  PsychicResponse res(r);
  res.setCode(303);
  res.addHeader("Location", "/");
  res.addHeader("Cache-Control", "no-store");
  res.setContent("");
  return res.send();
}

void routes() {
  g_server.on("/", HTTP_GET, [](PsychicRequest* r) {
    String out;
    out.reserve(6144);
    out += "<!doctype html><meta charset=utf-8><meta http-equiv=refresh content=2>"
           "<title>AffaDisplay golden</title>"
           "<body style='background:#111;color:#ddd;font:13px ui-monospace,monospace'>";
    out += "<pre>";
    out += statusText();
    out += "\nrows\n";
    char b[128];
    for (int i = 0; i < 3; ++i) {
      snprintf(b, sizeof(b), "  [%d] %-20s  %5lu ms   \"%s\"\n", i, g_shown[i],
               static_cast<unsigned long>(g_row[i].periodMs), g_row[i].text);
      out += b;
    }
    out += "</pre>";

    // Forms, not JavaScript: a GET form is the smallest thing that works everywhere.
    out += g_paused ? "<a href='/pause?on=0'>[ RESUME ]</a> " : "<a href='/pause?on=1'>[ PAUSE ]</a> ";
    out += "<a href=/wire.txt><b>[ DOWNLOAD WIRE LOG ]</b></a> ";
    out += g_logHeartbeats ? "<a href='/heartbeats?on=0'>[ hide heartbeats ]</a> "
                           : "<a href='/heartbeats?on=1'>[ show heartbeats ]</a> ";
    out += "<a href='/update'>[ OTA ]</a> <a href='/wifi'>[ WiFi ]</a><br><br>";
    for (int i = 0; i < 3; ++i) {
      snprintf(b, sizeof(b),
               "<form style='display:inline' action=/row><input type=hidden name=i value=%d>"
               "text <input name=t size=24> speed <input name=p size=5 value=%lu> ms "
               "<button>set row %d</button></form><br>",
               i, static_cast<unsigned long>(g_row[i].periodMs), i);
      out += b;
    }
    out += "<br><form style='display:inline' action=/clock>clock HHMM "
           "<input name=v size=6 value=1000><button>set</button></form>";
    out += "<pre>";
    out += wireText();
    out += "\n---- log ----\n";
    char line[128];
    portENTER_CRITICAL(&g_logMux);
    for (size_t i = 0; i < kLogRing; ++i) {
      const LogRec& rec = g_log[(g_logHead + i) % kLogRing];
      if (!rec.ms && !rec.msg[0]) continue;
      snprintf(line, sizeof(line), "%8lu  %s\n", static_cast<unsigned long>(rec.ms), rec.msg);
      out += line;
    }
    portEXIT_CRITICAL(&g_logMux);
    out += "</pre>";
    return r->reply(200, "text/html", out.c_str());
  });

  // DOWNLOAD THE WHOLE RING. The page shows only the newest rows — a response sized to the
  // full ring is a multi-kilobyte allocation on the HTTP task and blanks the page when it
  // fails — but a post-mortem wants all of it, and a one-shot download is not on the 2 s
  // refresh path. Content-Disposition makes the browser save it instead of rendering it.
  g_server.on("/wire.txt", HTTP_GET, [](PsychicRequest* r) {
    WireRec snap[kWireRing];
    size_t head; uint32_t seq;
    portENTER_CRITICAL(&g_wireMux);
    memcpy(snap, g_wire, sizeof(snap));
    head = g_wireHead; seq = g_wireSeq;
    portEXIT_CRITICAL(&g_wireMux);

    String out;
    out.reserve(kWireRing * 64 + 512);
    char line[128];
    snprintf(line, sizeof(line),
             "# AffaDisplay %s  img %.8s\n# up %lus, %lu frames seen, ring holds %u\n"
             "# heartbeats (3AF B9 / 3CF 69) are %s\n"
             "#   first    last   xN dir  id  data\n",
             kVersion, ESP.getSketchMD5().c_str(),
             static_cast<unsigned long>(millis() / 1000),
             static_cast<unsigned long>(seq), static_cast<unsigned>(kWireRing),
             g_logHeartbeats ? "INCLUDED" : "filtered out");
    out += line;
    for (size_t i = 0; i < kWireRing; ++i) {
      const WireRec& w = snap[(head + i) % kWireRing];
      if (!w.count) continue;
      int n = snprintf(line, sizeof(line), "%8lu %7lu %4lu  %s %03X ",
                       static_cast<unsigned long>(w.firstMs),
                       static_cast<unsigned long>(w.lastMs),
                       static_cast<unsigned long>(w.count),
                       w.tx ? "TX" : "RX", static_cast<unsigned>(w.id));
      for (uint8_t b = 0; b < 8; ++b)
        n += snprintf(line + n, sizeof(line) - n, " %02X", static_cast<unsigned>(w.d[b]));
      out += line; out += '\n';
    }

    PsychicResponse res(r);
    res.setCode(200);
    res.setContentType("text/plain");
    char fname[96];
    snprintf(fname, sizeof(fname), "attachment; filename=affa-wire-%lus.txt",
             static_cast<unsigned long>(millis() / 1000));
    res.addHeader("Content-Disposition", fname);
    res.setContent(out.c_str());
    return res.send();
  });

  // Heartbeats are filtered from the ring by default so a handshake is still readable
  // thirty seconds later. Turn them on when the question IS the heartbeat.
  g_server.on("/heartbeats", HTTP_GET, [](PsychicRequest* r) {
    if (r->getParam("on")) g_logHeartbeats = r->getParam("on")->value().toInt() != 0;
    logmsg("wire log heartbeats %s", g_logHeartbeats ? "INCLUDED" : "filtered");
    return seeOther(r);
  });

  g_server.on("/pause", HTTP_GET, [](PsychicRequest* r) {
    if (r->getParam("on")) g_paused = r->getParam("on")->value().toInt() != 0;
    saveRows();
    logmsg("rows %s", g_paused ? "PAUSED" : "resumed");
    return seeOther(r);
  });

  g_server.on("/row", HTTP_GET, [](PsychicRequest* r) {
    const int i = r->getParam("i") ? r->getParam("i")->value().toInt() : -1;
    if (i < 0 || i > 2) return r->reply(400, "text/plain", "i must be 0..2\n");
    if (r->getParam("t") && r->getParam("t")->value().length())
      snprintf(g_row[i].text, sizeof(g_row[i].text), "%s", r->getParam("t")->value().c_str());
    if (r->getParam("p")) g_row[i].periodMs = r->getParam("p")->value().toInt();
    g_row[i].offset = 0;
    saveRows();
    logmsg("row %d = \"%s\" @ %lu ms", i, g_row[i].text,
           static_cast<unsigned long>(g_row[i].periodMs));
    return seeOther(r);
  });

  g_server.on("/clock", HTTP_GET, [](PsychicRequest* r) {
    const String v = r->getParam("v") ? r->getParam("v")->value() : String();
    if (v.length() != 4) return r->reply(400, "text/plain", "HHMM, four digits\n");
    // Another one-liner: the library builds `05 56 'H''H''M''M'` and owns the ACK.
    const bool ok = g_task.setTime(v.c_str()) != affa::rtos::kNoRequest;
    logmsg("clock %s -> %s", v.c_str(), ok ? "queued" : "REFUSED");
    return seeOther(r);
  });

  // Minimal WiFi manager: show the saved SSID and let it be changed. Credentials live in
  // the same NVS namespace the other examples read, so one setup serves them all.
  g_server.on("/wifi", HTTP_GET, [](PsychicRequest* r) {
    if (r->getParam("ssid")) {
      Preferences p;
      if (p.begin("megaopen", false)) {
        p.putString("ssid", r->getParam("ssid")->value());
        if (r->getParam("pass")) p.putString("pass", r->getParam("pass")->value());
        p.end();
      }
      logmsg("wifi credentials saved - rebooting");
      r->reply(200, "text/html",
               "<meta http-equiv=refresh content='3;url=/'>saved, rebooting...");
      delay(400);
      ESP.restart();
      return ESP_OK;
    }
    Preferences p;
    String ssid;
    if (p.begin("megaopen", true)) { ssid = p.getString("ssid", ""); p.end(); }
    String out = "<!doctype html><meta charset=utf-8><body style='background:#111;color:#ddd;"
                 "font:14px system-ui'><h3>WiFi</h3>current SSID: <b>";
    out += ssid.length() ? ssid : String("(none - running as AP)");
    out += "</b><form action=/wifi>SSID <input name=ssid><br>pass <input name=pass "
           "type=password><br><button>save &amp; reboot</button></form>"
           "<p><a href=/>back</a>";
    return r->reply(200, "text/html", out.c_str());
  });
}

void startWifi() {
  Preferences p;
  String ssid, pass;
  if (p.begin("megaopen", true)) {
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
    const uint32_t until = millis() + 15000;
    while (millis() < until && WiFi.status() != WL_CONNECTED) delay(100);
    sta = WiFi.status() == WL_CONNECTED;
  }
  if (!sta) { WiFi.mode(WIFI_AP); WiFi.softAP("AffaGolden", "affagolden"); }
  const String ip = sta ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  Serial.printf("[wifi] %s  http://%s/   OTA http://%s/update\n",
                sta ? "STA" : "AP (connect and open /wifi)", ip.c_str(), ip.c_str());
}

}  // namespace

void setup() {
  pinMode(kTxPin, OUTPUT);
  digitalWrite(kTxPin, HIGH);      // release the bus: recessive is HIGH
  delay(2000);                     // the SuperMini/DevKit settle; MeganeCAN opens the same way

  Serial.begin(115200);
  delay(300);
  Serial.printf("\n%s  %s %s\n", kVersion, __DATE__, __TIME__);

  snprintf(g_row[0].text, sizeof(g_row[0].text), "AFFA DISPLAY - ROW ONE");
  snprintf(g_row[1].text, sizeof(g_row[1].text), "SECOND ROW MOVES SLOWER");
  snprintf(g_row[2].text, sizeof(g_row[2].text), "THIRD ROW SLOWEST OF ALL");
  g_row[0].periodMs = 220;
  g_row[1].periodMs = 380;
  g_row[2].periodMs = 550;
  loadRows();

  if (!g_link.begin(kRxPin, kTxPin, kBitrate))
    Serial.println("[can] link did not come up");

  // ORDER IS THE CONTRACT: taps and callbacks, then begin(), then start(). start() refuses a
  // display that was never begun, and a callback installed after the task is running would
  // miss whatever it had already delivered.
  g_display.onFrame(&onWire, nullptr);
  g_task.onComplete(&onDone, nullptr);
  g_display.begin();
  if (!g_task.start(g_display))
    Serial.println("[task] start() FAILED - nothing will be polled");

  startWifi();
  g_server.config.max_uri_handlers = 48;
  g_server.config.stack_size       = 10240;
  g_server.config.lru_purge_enable = true;
  g_server.listen(80);
  ElegantOTA.begin(&g_server);     // OTA FIRST, always
  routes();

  logmsg("%s ready - the library owns the protocol", kVersion);
}

void loop() {
  // NO poll() HERE, AND THAT IS THE POINT. The library polls itself on its own task, so
  // loop() may block as long as it likes without costing a timed-out ACK or a lost render.
  const uint32_t now = millis();

  static bool s_wasRegistered = false;
  const bool registered = affa::hasFlag(g_task.status().sync, affa::SyncState::FuncsReg);
  if (registered != s_wasRegistered) {
    s_wasRegistered = registered;
    logmsg(registered ? "panel REGISTERED - rows are live" : "registration lost");
    if (registered) {
      for (auto& s : g_shown) s[0] = 0;
      g_screenBusy = false;
      g_stage = Stage::NeedPower;   // a new session starts with a dark panel
    }
  }

  if (registered) pumpRows(now);

  static uint32_t s_nextStatus = 0;
  if (static_cast<int32_t>(now - s_nextStatus) >= 0) {
    s_nextStatus = now + 5000;
    const affa::CanCommonLink::Driver d = g_link.driver();
    Serial.printf("[%lu] sync 0x%02X reg %d | screens ok %lu fail %lu busy %d refused %lu q %u | drv s=%u txErr %lu "
                  "rxErr %lu busErr %lu qTx %lu\n",
                  static_cast<unsigned long>(now),
                  static_cast<unsigned>(g_task.status().sync), registered ? 1 : 0,
                  static_cast<unsigned long>(g_okCount),
                  static_cast<unsigned long>(g_failCount),
                  g_screenBusy ? 1 : 0, static_cast<unsigned long>(g_refused),
                  static_cast<unsigned>(g_display.queued()),
                  static_cast<unsigned>(d.state), static_cast<unsigned long>(d.txErr),
                  static_cast<unsigned long>(d.rxErr), static_cast<unsigned long>(d.busErr),
                  static_cast<unsigned long>(d.queuedTx));
  }

  // FULL WIRE DUMP ON DEMAND. Send any character on the serial port and the ENTIRE ring is
  // printed, oldest first, uncoalesced counts included. The web page shows only the newest
  // rows on purpose — a response sized to the whole ring is a multi-kilobyte allocation on
  // the HTTP task and goes BLANK when it fails — but a handshake post-mortem needs all of it,
  // and serial has no such limit.
  if (Serial.available()) {
    while (Serial.available()) Serial.read();
    WireRec snap[kWireRing];
    size_t head; uint32_t seq;
    portENTER_CRITICAL(&g_wireMux);
    memcpy(snap, g_wire, sizeof(snap));
    head = g_wireHead; seq = g_wireSeq;
    portEXIT_CRITICAL(&g_wireMux);

    Serial.printf("\n===== WIRE DUMP: %lu frames seen, ring holds %u =====\n",
                  static_cast<unsigned long>(seq), static_cast<unsigned>(kWireRing));
    Serial.println("   first    last   xN dir  id  data");
    for (size_t i = 0; i < kWireRing; ++i) {
      const WireRec& w = snap[(head + i) % kWireRing];
      if (!w.count) continue;
      Serial.printf("%8lu %7lu %4lu  %s %03X  %02X %02X %02X %02X %02X %02X %02X %02X\n",
                    static_cast<unsigned long>(w.firstMs),
                    static_cast<unsigned long>(w.lastMs),
                    static_cast<unsigned long>(w.count),
                    w.tx ? "TX" : "RX", static_cast<unsigned>(w.id),
                    w.d[0], w.d[1], w.d[2], w.d[3], w.d[4], w.d[5], w.d[6], w.d[7]);
    }
    Serial.println("===== END WIRE DUMP =====\n");
  }

  ElegantOTA.loop();
  delay(5);
}
