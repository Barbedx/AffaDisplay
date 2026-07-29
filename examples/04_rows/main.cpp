// 04_rows — a live three-row screen, a periodic popup, and a console to poke them with.
//
// This is the example to read AFTER 03_hello. 03_hello puts one string on the glass and
// stops; this one keeps a screen ALIVE for hours, which is a different problem and has
// exactly three parts:
//
//   1. WHAT THE ROWS SAY is a widget, not a protocol call. affa::widget::RowScreen holds
//      three marquees, tells you when the visible window has actually changed, and knows
//      nothing about CAN. It transmits nothing. THIS FILE does the transmitting.
//   2. WHEN TO REPAINT is a function of the wire, not of the loop. A Carminat fullscreen is
//      fourteen ACKed frames — about 190 ms — so the wire carries roughly five screens a
//      second. Painting on a timer would queue faster than that and the screen would lag
//      further behind reality every minute. Painting when the PREVIOUS ONE COMPLETED and
//      the content has moved is self-limiting and cannot fall behind.
//   3. THE POPUP IS AN OVERLAY, and it is the one screen here that must be closed. It
//      survives a redraw of the screen underneath; a fullscreen does not need a teardown
//      at all, because the next fullscreen simply replaces it.
//
// THE ONE-LINE VERSION OF THE API, and the thing people get wrong:
//
//     every render call ENQUEUES and returns immediately. The value it hands back is an
//     ACCEPTANCE verdict, never a delivery verdict. Delivery arrives later, on onComplete.
//
// So `showFullscreenText(a, b, c)` twice in a row does not draw two screens: they share one
// render slot and the second replaces the first. That is a FEATURE for a live screen — it
// is what "coalescing" means, and it is why this file can be sloppy about timing — but it
// makes "call it in a loop and watch it animate" produce a frozen screen and no error.
//
// WHY IT HAS WIFI when 03_hello deliberately does not: this runs for an hour on a board
// with no cable attached. The console is how you change a speed without a rebuild, and
// ElegantOTA is the only way back in. Both are at the BOTTOM of this file, fenced off —
// delete everything below THE DEPLOYMENT SCAFFOLDING line and the example still works, it
// just becomes a cable-flashed appliance with fixed speeds.
//
// Build:
//   pio run -e ex04_rows -t upload     first flash, over USB
//   thereafter                         http://<ip>/       console
//                                      http://<ip>/update ElegantOTA

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
#  error "04_rows is a Carminat example: build with -D AFFA_PANEL_CARMINAT=1"
#endif
#if !AFFA_ENABLE_TASK
#  error "04_rows relies on the library owning the poll task: -D AFFA_ENABLE_TASK=1"
#endif
#if !AFFA_ENABLE_MARQUEE
#  error "04_rows is built on the RowScreen widget: -D AFFA_ENABLE_MARQUEE=1"
#endif
#if !AFFA_ENABLE_FULLSCREEN || !AFFA_ENABLE_POPUP
#  error "04_rows draws a fullscreen and a popup: -D AFFA_ENABLE_FULLSCREEN=1 -D AFFA_ENABLE_POPUP=1"
#endif

namespace {

using affa::widget::Marquee;
using affa::widget::RowScreen;

// ---------------------------------------------------------------------------
// Board
// ---------------------------------------------------------------------------
// ESP32-C3 SuperMini on the bench rig. RX FIRST — the named fields are what stop the swap
// from becoming a silent bus with no error reported anywhere.
constexpr affa::CanPins kPins{ .rx = GPIO_NUM_4, .tx = GPIO_NUM_3 };
constexpr uint32_t      kBitrate = 500000;

// 250 ms, MEASURED. 0 leaves a bus-off parked in TWAI_STATE_STOPPED for ever and reads
// exactly like a dead transceiver; 2000 accumulates inside setup().
constexpr uint32_t kForceRecoveryMs = 250;

// The panel does not announce that its glass is lit, so this is a hard-coded wait and there
// is no honest way around it.
constexpr uint32_t kWarmUpMs = 750;

// ---------------------------------------------------------------------------
// Screen geometry and cadence — the one set of numbers worth understanding
// ---------------------------------------------------------------------------
// Visible cells per row on a Carminat fullscreen. The wire carries far more (96 content
// bytes for the three lines together); this is how much of it the glass actually shows, so
// it is the width the marquee window must be.
constexpr uint8_t kRowWidth = 20;

// Blank cells appended before the text wraps, so the wrap reads as a pause and not as a
// collision between the end and the start.
constexpr uint8_t kRowGap = 6;

// THREE SPEEDS ON ONE GRID, and the harmonising is the point rather than the values.
//
// A repaint costs ~190 ms of wire, so about five screens a second is all there is. These
// three cadences ask for 1/0.4 + 1/0.8 + 1/1.2 = 4.2 steps a second in the WORST case — but
// because 800 and 1200 are multiples of 400, the step instants COINCIDE, and the actual
// repaint rate is 2.5/s. Pick 250/400/700 instead and the same visible motion costs eight
// repaints a second: 60 % over what the wire can carry, so the rows step at whatever rate
// the bus allows rather than the rate you configured, and the scroll goes uneven.
//
// Oversubscribing is SAFE — nothing breaks, the library coalesces what it cannot send — it
// just stops looking like what you asked for. The console lets you try it.
constexpr uint32_t kSpeedFast   = 400;    // row 0
constexpr uint32_t kSpeedMedium = 800;    // row 1
constexpr uint32_t kSpeedSlow   = 1200;   // row 2

// The popup: every 30 s, visible for 3 s, carrying a counter so you can tell one from the
// next from across the room.
constexpr uint32_t kPopupEveryMs = 30000;
constexpr uint32_t kPopupHoldMs  = 3000;

// A render that never completes must not wedge the sequence for ever. Two full ISO-TP ACK
// timeouts is comfortably longer than any healthy screen takes.
constexpr uint32_t kRenderTimeoutMs = 4000;

// How often the soak line goes into the log ring. Once a minute is enough to reconstruct an
// hour afterwards and cheap enough to leave running for a day.
constexpr uint32_t kSoakReportMs = 60000;

// ---------------------------------------------------------------------------
// The library objects. Four, and that is the whole surface.
// ---------------------------------------------------------------------------
struct ArduinoClock final : affa::IClock {
  uint32_t millis() const override { return ::millis(); }
};

affa::Esp32CanLink    g_link;
ArduinoClock          g_clock;
affa::CarminatDisplay g_display(g_link, g_clock);
affa::rtos::AffaTask  g_task;      // the library polls itself; loop() may block freely

// The widget. Holds content and phase; holds no link, no display and no task.
affa::widget::RowScreenGeometry rowGeometry() {
  affa::widget::RowScreenGeometry g;
  g.width = kRowWidth;
  g.gap   = kRowGap;
  return g;
}
RowScreen g_rows{rowGeometry()};

// ---------------------------------------------------------------------------
// The log ring — this board is watched over HTTP, not over a cable
// ---------------------------------------------------------------------------
// Written from loop() and from the library's owned task (the sync callback), read from the
// HTTP task. A spinlock rather than "it is only diagnostics": a torn record here is exactly
// the kind of thing that sends you hunting a protocol bug that does not exist.
struct LogRec { uint32_t ms = 0; char msg[96] = {0}; };
constexpr uint8_t kLogRing = 48;
LogRec            g_log[kLogRing];
uint8_t           g_logHead = 0;
portMUX_TYPE      g_logMux  = portMUX_INITIALIZER_UNLOCKED;

void logmsg(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void logmsg(const char* fmt, ...) {
  char buf[96];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  const uint32_t ms = ::millis();
  portENTER_CRITICAL(&g_logMux);
  LogRec& r = g_log[g_logHead];
  r.ms = ms;
  snprintf(r.msg, sizeof(r.msg), "%s", buf);
  g_logHead = (g_logHead + 1) % kLogRing;
  portEXIT_CRITICAL(&g_logMux);

  if (Serial) Serial.printf("[%lu] %s\n", static_cast<unsigned long>(ms), buf);
}

// ---------------------------------------------------------------------------
// The frame ring — WHAT THE PANEL ACTUALLY SENDS US
// ---------------------------------------------------------------------------
// AffaDisplayBase::onFrame() is the Layer-0 tap: every frame, both directions, before any
// interpretation. It is the only view that can answer "is the other end talking at all",
// and without it a screen that does not appear is indistinguishable from a panel that is
// switched off.
//
// Pushed from the library's owned task, read from the HTTP task, so it takes a spinlock. Not
// because it is precious, but because a torn record here sends you hunting a protocol bug
// that does not exist.
struct FrameRec {
  uint32_t ms  = 0;
  uint16_t id  = 0;
  uint8_t  dir = 0;          // 0 = RX (heard), 1 = TX (ours)
  uint8_t  len = 0;
  uint8_t  d[8] = {0};
};
constexpr uint8_t kFrameRing = 64;
FrameRec     g_frames[kFrameRing];
uint8_t      g_frameHead = 0;
uint32_t     g_frameSeq  = 0;
portMUX_TYPE g_frameMux  = portMUX_INITIALIZER_UNLOCKED;

// Counted separately from the driver's counters: these are frames that reached the LIBRARY,
// which is what "do we hear the panel" actually asks.
uint32_t g_rxSeen   = 0;
uint32_t g_txSeen   = 0;
uint32_t g_lastRxMs = 0;

void onTap(const affa::Frame& f, affa::Direction d, void*) {
  const bool rx = (d == affa::Direction::Rx);
  const uint32_t ms = ::millis();
  portENTER_CRITICAL(&g_frameMux);
  FrameRec& r = g_frames[g_frameHead];
  r.ms  = ms;
  r.id  = static_cast<uint16_t>(f.id);
  r.dir = rx ? 0 : 1;
  r.len = f.len;
  memcpy(r.d, f.data, 8);
  g_frameHead = static_cast<uint8_t>((g_frameHead + 1) % kFrameRing);
  ++g_frameSeq;
  if (rx) { ++g_rxSeen; g_lastRxMs = ms; } else { ++g_txSeen; }
  portEXIT_CRITICAL(&g_frameMux);
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
    // Aborted is the EXPECTED one here and not a fault: a live screen supersedes its own
    // render whenever the content moves again before the last one reached the wire.
    case affa::Result::Aborted:      return "Aborted";
    default:                         return "?";
  }
}

// ---------------------------------------------------------------------------
// Render completion — published by the library's task, consumed by loop()
// ---------------------------------------------------------------------------
// THE SEQUENCE NUMBER IS PUBLISHED LAST, and everything else is written before it. A reader
// that sees the bump therefore sees a consistent record, with no lock on the path the
// library actually cares about.
volatile uint32_t g_ackReq = 0;
volatile uint8_t  g_ackRes = 0;
volatile uint32_t g_ackSeq = 0;
uint32_t          g_ackSeen = 0;    // loop()'s cursor into that stream

uint32_t g_inFlight    = 0;         // the request we are waiting on; 0 when idle
uint32_t g_flightSince = 0;

void onRenderComplete(affa::rtos::TxRequest req, affa::Result r, void*) {
  g_ackReq = static_cast<uint32_t>(req);
  g_ackRes = static_cast<uint8_t>(r);
  ++g_ackSeq;
}

// 0 = still waiting, 1 = ours completed Ok, 2 = ours failed, 3 = nothing in flight.
uint8_t pollRender(affa::Result& out) {
  if (g_inFlight == 0) { g_ackSeen = g_ackSeq; return 3; }
  while (g_ackSeen != g_ackSeq) {
    ++g_ackSeen;
    if (g_ackReq == g_inFlight) {
      out = static_cast<affa::Result>(g_ackRes);
      g_inFlight = 0;
      return out == affa::Result::Ok ? 1 : 2;
    }
    // Somebody else's completion. The library makes renders of its own — a menu redraw, a
    // close banner — and they come through this same callback. Keep looking.
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Application state
// ---------------------------------------------------------------------------
enum class Step : uint8_t { WaitPanel, PowerOn, WarmUp, Live };

// Spelled out rather than numbered, because "step 0" tells you nothing and this is the first
// question anyone asks a screen that is blank.
const char* stepName(Step s) {
  switch (s) {
    case Step::WaitPanel: return "wait-panel  nothing heard from the panel yet";
    case Step::PowerOn:   return "power-on    setPower enqueued, waiting for its ACK";
    case Step::WarmUp:    return "warm-up     ACKed; letting the glass light up";
    case Step::Live:      return "live        drawing rows";
  }
  return "?";
}

Step     g_step     = Step::WaitPanel;
uint32_t g_deadline = 0;

bool     g_powerWanted = true;     // what the console last asked for
bool     g_powerIsOn   = false;    // what the panel was last told

bool     g_popupUp     = false;
uint32_t g_popupNextMs = 0;
uint32_t g_popupUntil  = 0;
uint32_t g_popupCount  = 0;

// Soak counters. Read from the HTTP task without a lock: aligned 32-bit reads do not tear on
// this core, and a diagnostic that is one repaint stale is still a correct diagnostic.
uint32_t g_repaints    = 0;
uint32_t g_renderFails = 0;
uint32_t g_timeouts    = 0;
uint32_t g_syncLosses  = 0;
uint32_t g_nextSoakMs  = kSoakReportMs;

// The two scrolling rows' content. Row 2 is generated, because it carries the clock.
char g_text0[AFFA_TEXT_MAX] = "AFFADISPLAY 0.3.1 - THREE ROWS, THREE SPEEDS, ONE WIRE";
char g_text1[AFFA_TEXT_MAX] = "ROW 1 RUNS AT HALF THE SPEED OF ROW 0";

// ---------------------------------------------------------------------------
// Row 2: the clock row, and the reason RowScreen exists at all
// ---------------------------------------------------------------------------
// Its text changes EVERY SECOND. Under a marquee's normal policy a changed string restarts
// at character 0, so this row would be pinned to the start for ever and would flicker once a
// second instead of scrolling. RowScreen publishes with Marquee::Phase::Keep, which makes
// the scroll phase a property of the ROW rather than of the string — so a row can scroll and
// tell the time at once. Nothing here has to know that; it just works.
void buildClockRow(uint32_t upMs, char* out, size_t n) {
  const uint32_t s = upMs / 1000;
  snprintf(out, n, "UP %02lu:%02lu:%02lu  PAINTS %lu  POPUPS %lu",
           static_cast<unsigned long>(s / 3600),
           static_cast<unsigned long>((s / 60) % 60),
           static_cast<unsigned long>(s % 60),
           static_cast<unsigned long>(g_repaints),
           static_cast<unsigned long>(g_popupCount));
}

void restart(const char* why) {
  logmsg("sequence restart: %s", why);
  g_step       = Step::WaitPanel;
  g_inFlight   = 0;
  g_powerIsOn  = false;
  g_popupUp    = false;
  g_rows.invalidate();          // whatever was on the glass, assume it is gone
}

// Enqueue one render and arm the wait. Every issue path goes through here so the timeout and
// the counters cannot disagree.
bool issue(affa::rtos::TxRequest req, uint32_t now, const char* what) {
  if (req == affa::rtos::kNoRequest) {
    // The command queue refused: it is full, or the task is not running. Not worth a retry
    // this iteration — the next one is 10 ms away.
    logmsg("%s: command queue refused", what);
    return false;
  }
  g_inFlight    = static_cast<uint32_t>(req);
  g_flightSince = now;
  return true;
}

// ---------------------------------------------------------------------------
// THE LIVE SCREEN — this is the whole example, and it is nineteen lines
// ---------------------------------------------------------------------------
void pumpScreen(uint32_t now) {
  // ---- publish the content ------------------------------------------------
  // Every iteration, unconditionally, with no change detection of our own. setText() is a
  // no-op when the string is identical and does not disturb the scroll, so this costs a
  // strcmp and is the intended way to use the widget: publish what is true, let it work out
  // whether anything moved.
  char clock[AFFA_TEXT_MAX];
  buildClockRow(now, clock, sizeof(clock));
  g_rows.setRows(g_text0, g_text1, clock, now);

  // ---- the popup, which owns the glass while it is up ---------------------
  if (g_popupUp) {
    if (!affa::expired(now, g_popupUntil)) return;   // still showing; do not repaint under it
    if (g_inFlight) return;                          // wait for our own last render first
    if (issue(g_task.hidePopup(), now, "hidePopup")) {
      g_popupUp = false;
      g_popupNextMs = now + kPopupEveryMs;
      // The rows have not moved as far as RowScreen knows, but the GLASS has changed under
      // them. invalidate() is exactly this case: force one repaint whatever the content says.
      g_rows.invalidate();
    }
    return;
  }

  if (affa::expired(now, g_popupNextMs)) {
    if (g_inFlight) return;
    char msg[AFFA_TEXT_MAX];
    snprintf(msg, sizeof(msg), "POPUP #%lu", static_cast<unsigned long>(g_popupCount + 1));
    if (issue(g_task.showPopupText(msg), now, "showPopupText")) {
      ++g_popupCount;
      g_popupUp    = true;
      g_popupUntil = now + kPopupHoldMs;
    }
    return;
  }

  // ---- the rows -----------------------------------------------------------
  // ONE RENDER IN FLIGHT AT A TIME. This is the self-limiting part: the next screen is not
  // even built until the panel has acknowledged the last one, so the repaint rate is
  // whatever the wire actually sustains and the screen can never queue itself into the past.
  if (g_inFlight) return;
  if (!g_rows.dirty(now)) return;

  char win[RowScreen::kRows][RowScreen::kCell];
  g_rows.render(now, win);

  if (!issue(g_task.showFullscreenText(win[0], win[1], win[2]), now, "showFullscreenText"))
    return;

  // LATCH ON ACCEPTANCE, NOT ON COMPLETION. A render that is superseded before it reaches
  // the wire is still the newest value for its slot, so latching later would repaint a
  // window the panel has already been given.
  g_rows.painted(now);
  ++g_repaints;
}

void soakReport(uint32_t now) {
  if (!affa::expired(now, g_nextSoakMs)) return;
  g_nextSoakMs = now + kSoakReportMs;

  const affa::rtos::Status st  = g_task.status();
  const affa::Stats        ls  = g_link.stats();
  const auto               lh  = g_display.linkHealth();
  const auto               drv = g_link.driverState();

  logmsg("soak up=%lus paints=%lu popups=%lu fail=%lu tmo=%lu synclost=%lu "
         "rx=%lu tx=%lu rxerr=%lu txerr=%lu buserr=%lu st=%u ovf=%lu flaps=%lu rec=%lu/%lu "
         "late=%luus stack=%lu",
         static_cast<unsigned long>(now / 1000),
         static_cast<unsigned long>(g_repaints),
         static_cast<unsigned long>(g_popupCount),
         static_cast<unsigned long>(g_renderFails),
         static_cast<unsigned long>(g_timeouts),
         static_cast<unsigned long>(g_syncLosses),
         static_cast<unsigned long>(ls.rxFrames),
         static_cast<unsigned long>(ls.txFrames),
         static_cast<unsigned long>(ls.rxErr),
         static_cast<unsigned long>(ls.txErr),
         static_cast<unsigned long>(drv.busErr),
         static_cast<unsigned>(drv.valid ? drv.state : 9),
         static_cast<unsigned long>(ls.ringOverflow),
         static_cast<unsigned long>(lh.flaps),
         static_cast<unsigned long>(lh.recoveries),
         static_cast<unsigned long>(lh.failures),
         static_cast<unsigned long>(st.pollLateMaxUs),
         static_cast<unsigned long>(st.stackFreeBytes));
}

}  // namespace

// ===========================================================================
// THE DEPLOYMENT SCAFFOLDING — WiFi, the console and OTA
// ===========================================================================
// Everything below this line is about reaching a board with no cable on it. None of it is
// AffaDisplay: delete it, drop the WiFi includes, and the example above still runs exactly
// as it does now with the speeds compiled in.

namespace {

constexpr const char* kWifiNamespace = "megaopen";   // read-only: ssid / pass
constexpr const char* kApSsid        = "AffaRows";
constexpr const char* kApPass        = "affarows1";
constexpr const char* kMdnsName      = "affarows";
constexpr uint32_t    kStaJoinMs     = 15000;

PsychicHttpServer g_server;
bool     g_otaRunning = false;
bool     g_txGate     = true;    // mirrors Esp32CanLink::setTxEnabled, for the console
uint32_t g_rebootAt   = 0;

const char kPage[] PROGMEM = R"HTML(<!doctype html><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>AffaDisplay 04_rows</title>
<style>
body{font:14px/1.5 system-ui,sans-serif;margin:0;padding:16px;background:#111;color:#ddd}
h1{font-size:16px;margin:0 0 12px}
fieldset{border:1px solid #333;border-radius:6px;margin:0 0 12px;padding:10px}
legend{color:#8ab;padding:0 6px}
button{font:13px system-ui;background:#243;color:#cfe;border:1px solid #465;border-radius:4px;
padding:5px 10px;margin:2px;cursor:pointer}
button:hover{background:#365}
pre{background:#000;border:1px solid #333;border-radius:6px;padding:8px;overflow:auto;
max-height:40vh;font:12px/1.4 ui-monospace,monospace;color:#9c9}
input{background:#000;color:#ddd;border:1px solid #444;border-radius:4px;padding:4px;width:60%}
.row{margin:6px 0}
a{color:#8ab}
</style>
<h1>AffaDisplay <span id=v>04_rows</span> &middot; <a href=/update>OTA</a></h1>

<fieldset><legend>display</legend>
<button onclick=go('/api/power?on=1')>power on</button>
<button onclick=go('/api/power?on=0')>power off</button>
<button onclick=go('/api/popup?now=1')>popup now</button>
<button onclick=go('/api/reboot')>reboot</button>
</fieldset>

<fieldset><legend>wire &mdash; is the panel talking?</legend>
<button onclick=go('/api/txgate?on=0')>TX gate SHUT (go silent)</button>
<button onclick=go('/api/txgate?on=1')>TX gate open</button>
<div class=row>Shut the gate and watch <b>rx</b> above. Still 0 &rarr; we are silent and the
fault is on the wire or at the panel. Climbing &rarr; we were trampling the bus.</div>
<pre id=f>...</pre>
</fieldset>

<fieldset><legend>rows</legend>
<div class=row>row 0
<button onclick=go('/api/speed?row=0&ms=200')>200</button>
<button onclick=go('/api/speed?row=0&ms=400')>400</button>
<button onclick=go('/api/speed?row=0&ms=800')>800</button>
<button onclick=go('/api/speed?row=0&ms=0')>stop</button>
<button onclick=go('/api/dir?row=0&rev=0')>&rarr;</button>
<button onclick=go('/api/dir?row=0&rev=1')>&larr;</button></div>
<div class=row>row 1
<button onclick=go('/api/speed?row=1&ms=400')>400</button>
<button onclick=go('/api/speed?row=1&ms=800')>800</button>
<button onclick=go('/api/speed?row=1&ms=1600')>1600</button>
<button onclick=go('/api/speed?row=1&ms=0')>stop</button>
<button onclick=go('/api/dir?row=1&rev=0')>&rarr;</button>
<button onclick=go('/api/dir?row=1&rev=1')>&larr;</button></div>
<div class=row>row 2
<button onclick=go('/api/speed?row=2&ms=600')>600</button>
<button onclick=go('/api/speed?row=2&ms=1200')>1200</button>
<button onclick=go('/api/speed?row=2&ms=2400')>2400</button>
<button onclick=go('/api/speed?row=2&ms=0')>stop</button>
<button onclick=go('/api/dir?row=2&rev=0')>&rarr;</button>
<button onclick=go('/api/dir?row=2&rev=1')>&larr;</button></div>
<div class=row><input id=t0 placeholder="text for row 0">
<button onclick="go('/api/text?row=0&t='+encodeURIComponent(t0.value))">set</button></div>
<div class=row><input id=t1 placeholder="text for row 1">
<button onclick="go('/api/text?row=1&t='+encodeURIComponent(t1.value))">set</button></div>
</fieldset>

<fieldset><legend>status</legend><pre id=s>...</pre></fieldset>
<fieldset><legend>log</legend><pre id=l>...</pre></fieldset>

<script>
// ONE TIMER, AND IT IS CHAINED RATHER THAN setInterval. A board whose HTTP task is busy will
// answer slowly; setInterval would keep stacking requests on top of it and hold every socket
// in the table, which is how this board used to become permanently unreachable.
async function go(u){ try{ await fetch(u) }catch(e){} tick() }
async function tick(){
  try{
    s.textContent = await (await fetch('/api/status')).text()
    f.textContent = await (await fetch('/api/frames')).text()
    l.textContent = await (await fetch('/api/log')).text()
  }catch(e){ s.textContent = 'offline' }
  setTimeout(tick, 2000)
}
tick()
</script>
)HTML";

// twai_state_t, spelled. `valid == false` means twai_get_status_info() itself failed, which
// on this driver means it is mid-reinstall — a snapshot, not a verdict.
const char* drvStateName(bool valid, uint8_t s) {
  if (!valid) return "reinstalling";
  switch (s) {
    case 0: return "STOPPED";
    case 1: return "running";
    case 2: return "BUS-OFF";
    case 3: return "recovering";
    default: return "?";
  }
}

uint8_t rowArg(PsychicRequest* r) {
  const int v = r->getParam("row") ? r->getParam("row")->value().toInt() : 0;
  return static_cast<uint8_t>(v < 0 ? 0 : (v > 2 ? 2 : v));
}

void routes() {
  g_server.on("/", HTTP_GET, [](PsychicRequest* r) {
    return r->reply(200, "text/html", kPage);
  });

  g_server.on("/api/status", HTTP_GET, [](PsychicRequest* r) {
    const affa::rtos::Status st  = g_task.status();
    const affa::Stats        ls  = g_link.stats();
    const auto               lh  = g_display.linkHealth();
    const auto               drv = g_link.driverState();
    uint32_t rxSeen, txSeen, lastRx;
    portENTER_CRITICAL(&g_frameMux);
    rxSeen = g_rxSeen; txSeen = g_txSeen; lastRx = g_lastRxMs;
    portEXIT_CRITICAL(&g_frameMux);
    const uint32_t nowMs = millis();

    char b[900];
    snprintf(b, sizeof(b),
             "uptimeMs   %lu\n"
             "step       %s\n"
             "           power %s   popup %s (#%lu)   inFlight %lu   txGate %s\n"
             "heard      rx %lu   tx %lu   lastRx %lu ms ago%s\n"
             "sync       0x%02X   registered %u   busy %u\n"
             "speeds     %lu / %lu / %lu ms   dir %c%c%c\n"
             "paints     %lu   fails %lu   timeouts %lu   syncLost %lu\n"
             "link       rx %lu  tx %lu  rxErr %lu  txErr %lu  txFailed %lu  drop %lu  ovf %lu\n"
             // THE LINE THAT SEPARATES A SILENT BUS FROM AN ERROR STORM, and it comes from
             // the driver rather than from us. busErr FROZEN with rxErr 0 is a bus with
             // nothing on it; busErr climbing by hundreds a second is a bus full of traffic
             // we cannot decode. Both look identical from rxFrames alone, which is 0 either
             // way, and confusing the two costs hours.
             "driver     %s state %u (%s)  busErr %lu  arbLost %lu  rxMissed %lu  restarts %lu\n"
             "linkHealth flaps %lu  recovered %lu  failed %lu  downMs %lu\n"
             "task       iters %lu  lateMax %luus @%lu  dropped %lu  stackFree %lu\n",
             static_cast<unsigned long>(nowMs),
             stepName(g_step),
             g_powerIsOn ? "on" : "off",
             g_popupUp ? "up" : "down", static_cast<unsigned long>(g_popupCount),
             static_cast<unsigned long>(g_inFlight), g_txGate ? "open" : "SHUT",
             static_cast<unsigned long>(rxSeen), static_cast<unsigned long>(txSeen),
             static_cast<unsigned long>(rxSeen ? nowMs - lastRx : 0),
             rxSeen ? "" : "   <-- WE HAVE NEVER HEARD THE PANEL",
             static_cast<unsigned>(st.sync), st.registered ? 1u : 0u, st.busy ? 1u : 0u,
             static_cast<unsigned long>(g_rows.scroll(0)),
             static_cast<unsigned long>(g_rows.scroll(1)),
             static_cast<unsigned long>(g_rows.scroll(2)),
             g_rows.direction(0) == Marquee::Direction::Forward ? '>' : '<',
             g_rows.direction(1) == Marquee::Direction::Forward ? '>' : '<',
             g_rows.direction(2) == Marquee::Direction::Forward ? '>' : '<',
             static_cast<unsigned long>(g_repaints),
             static_cast<unsigned long>(g_renderFails),
             static_cast<unsigned long>(g_timeouts),
             static_cast<unsigned long>(g_syncLosses),
             static_cast<unsigned long>(ls.rxFrames), static_cast<unsigned long>(ls.txFrames),
             static_cast<unsigned long>(ls.rxErr), static_cast<unsigned long>(ls.txErr),
             static_cast<unsigned long>(ls.txFailed),
             static_cast<unsigned long>(ls.txDropped),
             static_cast<unsigned long>(ls.ringOverflow),
             drv.valid ? "ok" : "UNREADABLE", static_cast<unsigned>(drv.state),
             drvStateName(drv.valid, drv.state),
             static_cast<unsigned long>(drv.busErr),
             static_cast<unsigned long>(drv.arbLost),
             static_cast<unsigned long>(drv.rxMissed),
             static_cast<unsigned long>(g_link.restarts()),
             static_cast<unsigned long>(lh.flaps),
             static_cast<unsigned long>(lh.recoveries),
             static_cast<unsigned long>(lh.failures),
             static_cast<unsigned long>(lh.downMs),
             static_cast<unsigned long>(st.iterations),
             static_cast<unsigned long>(st.pollLateMaxUs),
             static_cast<unsigned long>(st.pollLateAtMs),
             static_cast<unsigned long>(st.queueDropped),
             static_cast<unsigned long>(st.stackFreeBytes));
    return r->reply(200, "text/plain", b);
  });

  g_server.on("/api/log", HTTP_GET, [](PsychicRequest* r) {
    // Copy under the lock, format outside it. Formatting inside a critical section on the
    // HTTP task is how you stall the CAN poll task from a browser refresh.
    LogRec snap[kLogRing];
    uint8_t head;
    portENTER_CRITICAL(&g_logMux);
    memcpy(snap, g_log, sizeof(snap));
    head = g_logHead;
    portEXIT_CRITICAL(&g_logMux);

    String out;
    out.reserve(kLogRing * 72);
    for (uint8_t i = 0; i < kLogRing; ++i) {
      const LogRec& e = snap[(head + i) % kLogRing];
      if (!e.msg[0]) continue;
      char line[128];
      snprintf(line, sizeof(line), "%8lu  %s\n", static_cast<unsigned long>(e.ms), e.msg);
      out += line;
    }
    return r->reply(200, "text/plain", out.c_str());
  });

  // THE RAW WIRE, oldest first. This is the answer to "what does the display send us": if
  // this shows only TX lines, the other end is not talking and no amount of render debugging
  // will help.
  g_server.on("/api/frames", HTTP_GET, [](PsychicRequest* r) {
    // STATIC, not a local. This snapshot is ~900 B and esp_http_server's task stack is what
    // the config above sets; a handler that overflows it dies silently and returns an empty
    // body. Every handler runs on that one task, so two snapshots are never taken at once.
    static FrameRec snap[kFrameRing];
    uint8_t  head;
    uint32_t seq;
    portENTER_CRITICAL(&g_frameMux);
    memcpy(snap, g_frames, sizeof(snap));
    head = g_frameHead;
    seq  = g_frameSeq;
    portEXIT_CRITICAL(&g_frameMux);

    const uint8_t have = (seq < kFrameRing) ? static_cast<uint8_t>(seq) : kFrameRing;

    String out;
    out.reserve(kFrameRing * 48 + 64);
    char hdr[64];
    snprintf(hdr, sizeof(hdr), "total %lu, showing last %u\n",
             static_cast<unsigned long>(seq), static_cast<unsigned>(have));
    out += hdr;
    for (uint8_t i = 0; i < have; ++i) {
      const FrameRec& f = snap[(head + kFrameRing - have + i) % kFrameRing];
      char line[80];
      int n = snprintf(line, sizeof(line), "%8lu %s %03X [%u]",
                       static_cast<unsigned long>(f.ms), f.dir ? "TX" : "RX",
                       static_cast<unsigned>(f.id), static_cast<unsigned>(f.len));
      for (uint8_t b = 0; b < f.len && b < 8 && n < static_cast<int>(sizeof(line)) - 4; ++b)
        n += snprintf(line + n, sizeof(line) - n, " %02X", static_cast<unsigned>(f.d[b]));
      out += line;
      out += '\n';
    }
    return r->reply(200, "text/plain", out.c_str());
  });

  // THE IS-IT-US TEST, and it is the first thing to reach for when the bus looks dead.
  //
  // Shutting the gate stops every frame WE would send, in software. The controller keeps
  // ACKing other nodes in hardware, which a two-node bus requires — so if rx STILL does not
  // climb with the gate shut, we are silent and cannot be the cause, and the fault is on the
  // wire or at the other end. If rx starts climbing the moment we go quiet, we were
  // trampling the bus.
  //
  // True listen-only is NOT available: Esp32CanLink::begin() refuses LinkMode::ListenOnly,
  // because this driver has no safe place to enter it.
  g_server.on("/api/txgate", HTTP_GET, [](PsychicRequest* r) {
    g_txGate = !(r->getParam("on") && r->getParam("on")->value() == "0");
    g_link.setTxEnabled(g_txGate);
    logmsg("console: TX gate %s", g_txGate ? "OPEN" : "SHUT - we are silent now");
    return r->reply(200, "text/plain", "ok");
  });

  g_server.on("/api/power", HTTP_GET, [](PsychicRequest* r) {
    g_powerWanted = !(r->getParam("on") && r->getParam("on")->value() == "0");
    logmsg("console: power %s", g_powerWanted ? "on" : "off");
    return r->reply(200, "text/plain", "ok");
  });

  g_server.on("/api/speed", HTTP_GET, [](PsychicRequest* r) {
    const uint8_t  row = rowArg(r);
    const uint32_t ms  = r->getParam("ms")
                             ? static_cast<uint32_t>(r->getParam("ms")->value().toInt()) : 0;
    // 0 makes the row STATIC rather than fast: it shows its first `width` characters and
    // never moves, which costs no repaints at all.
    g_rows.setScroll(row, ms);
    logmsg("console: row %u speed %lu ms", row, static_cast<unsigned long>(ms));
    return r->reply(200, "text/plain", "ok");
  });

  g_server.on("/api/dir", HTTP_GET, [](PsychicRequest* r) {
    const uint8_t row = rowArg(r);
    const bool    rev = r->getParam("rev") && r->getParam("rev")->value() == "1";
    g_rows.setDirection(row, rev ? Marquee::Direction::Reverse : Marquee::Direction::Forward,
                        millis());
    logmsg("console: row %u %s", row, rev ? "reverse" : "forward");
    return r->reply(200, "text/plain", "ok");
  });

  g_server.on("/api/text", HTTP_GET, [](PsychicRequest* r) {
    const uint8_t row = rowArg(r);
    const String  t   = r->getParam("t") ? r->getParam("t")->value() : String();
    // Row 2 is generated every iteration, so writing to it would be overwritten immediately.
    // Say so rather than accepting it and appearing to do nothing.
    if (row == 2) return r->reply(200, "text/plain", "row 2 carries the clock; it is generated");
    snprintf(row == 0 ? g_text0 : g_text1, AFFA_TEXT_MAX, "%s", t.c_str());
    logmsg("console: row %u text '%s'", row, t.c_str());
    return r->reply(200, "text/plain", "ok");
  });

  g_server.on("/api/popup", HTTP_GET, [](PsychicRequest* r) {
    g_popupNextMs = millis();          // due now; loop() picks it up on its next pass
    return r->reply(200, "text/plain", "ok");
  });

  g_server.on("/api/reboot", HTTP_GET, [](PsychicRequest* r) {
    g_rebootAt = millis() + 300;       // after the response has actually gone out
    return r->reply(200, "text/plain", "rebooting");
  });
}

void startNetwork() {
  // Read-only open of the namespace this board already holds credentials in. READ-ONLY
  // matters: an NVS write stalls CAN reception outright for the duration of the flash write,
  // because the TWAI ISR is not in IRAM. This example must never be why they change.
  Preferences p;
  String ssid, pass;
  if (p.begin(kWifiNamespace, /*readOnly=*/true)) {
    ssid = p.getString("ssid", "");
    pass = p.getString("pass", "");
    p.end();
  }

  WiFi.persistent(false);
  WiFi.setSleep(true);          // the C3 has one radio and interleaves WiFi with CAN timing

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
  // THE FOUR LINES THAT KEEP THIS BOARD REACHABLE.
  //
  // With lru_purge_enable false — esp_http_server's default, and what PsychicHttp leaves it
  // at unless ENABLE_ASYNC is defined — a full socket table is PERMANENT: httpd stops
  // calling accept() and never resumes. The board still answers ping and still answers mDNS,
  // so it looks alive from every angle except the one that matters, and OTA is gone with it.
  g_server.config.lru_purge_enable  = true;
  g_server.config.max_open_sockets  = 7;
  g_server.config.recv_wait_timeout = 3;
  g_server.config.send_wait_timeout = 3;
  // The URI table is a FIXED ARRAY and registering past its end fails SILENTLY — PsychicHttp
  // does not check the return, so the route is simply absent. This example has nine routes
  // plus ElegantOTA's three; the headroom is deliberate, because the failure mode is a board
  // that needs a cable.
  g_server.config.max_uri_handlers  = 32;
  g_server.config.stack_size        = 8192;

  g_server.listen(80);

  // OTA FIRST, ALWAYS — before any route of ours can crowd its three registrations off the
  // end of that table. It is the only way back into a board with no cable.
  //
  // An OTA write stalls CAN reception outright, so gate our transmitter for the duration:
  // there is no point shouting at a bus we cannot hear. Expect a resync after the reboot.
  ElegantOTA.onStart([]() {
    g_otaRunning = true;
    g_link.setTxEnabled(false);
    logmsg("ota started - CAN TX gated");
  });
  ElegantOTA.onEnd([](bool ok) {
    // CLEAR THE FLAG ON FAILURE. A latched "OTA in progress" that nothing resets leaves every
    // console command silently ignored until somebody reboots, and it took a while to find.
    if (!ok) { g_otaRunning = false; g_link.setTxEnabled(true); }
    logmsg("ota %s", ok ? "ok, rebooting" : "FAILED - TX ungated");
  });
  ElegantOTA.begin(&g_server);

  routes();
}

// Runs on the LIBRARY'S OWN TASK, not on loop(). logmsg() is safe there — it takes a
// spinlock and formats into a fixed buffer — but nothing heavier belongs here: a blocking
// callback shows up directly as Status::pollLateMaxUs and eventually as a missed ACK.
void onSync(affa::SyncState s, void*) {
  logmsg("sync: state 0x%02X%s", static_cast<unsigned>(s),
         affa::hasFlag(s, affa::SyncState::Failed) ? " (FAILED)" : "");
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\nAffaDisplay 04_rows");

  // NETWORK AND OTA FIRST, before anything that can fail. Everything after this point may
  // go wrong without costing you the board.
  startNetwork();
  startHttp();

  if (!g_link.begin(kPins, kBitrate, kForceRecoveryMs))
    logmsg("can: controller did not come up");

  g_rows.setScroll(0, kSpeedFast);
  g_rows.setScroll(1, kSpeedMedium);
  g_rows.setScroll(2, kSpeedSlow);

  // THE ORDER IS THE CONTRACT: callbacks, then begin(), then start(). start() refuses a
  // display that was never begun, and a callback installed after the task is running would
  // miss whatever it had already delivered.
  g_task.onComplete(&onRenderComplete, nullptr);
  g_display.onSync(&onSync, nullptr);
  g_display.onFrame(&onTap, nullptr);       // Layer 0: everything, both directions
  g_display.begin();

  if (!g_task.start(g_display))
    logmsg("task: start() FAILED - nothing will be polled");

  logmsg("up: rows %lu/%lu/%lu ms, popup every %lus for %lus",
         static_cast<unsigned long>(kSpeedFast), static_cast<unsigned long>(kSpeedMedium),
         static_cast<unsigned long>(kSpeedSlow),
         static_cast<unsigned long>(kPopupEveryMs / 1000),
         static_cast<unsigned long>(kPopupHoldMs / 1000));
}

void loop() {
  // NO poll() HERE. The library polls itself on its own task, so loop() may block — for an
  // OTA write, for a slow HTTP response — without costing a timed-out ACK or a lost render.
  ElegantOTA.loop();

  const uint32_t now = millis();

  if (g_rebootAt && affa::expired(now, g_rebootAt)) ESP.restart();

  // An OTA write stalls the CAN ISR; do not fight it for the bus.
  if (g_otaRunning) { delay(10); return; }

  const affa::rtos::Status st = g_task.status();

  // ---- one place where a render's fate is accounted for -------------------
  affa::Result res = affa::Result::Ok;
  switch (pollRender(res)) {
    case 2:
      ++g_renderFails;
      logmsg("render failed: %s", resultName(res));
      // LinkDown and Cancelled mean the world under us changed; anything else is a single
      // screen we can simply draw again. Force the redraw either way.
      g_rows.invalidate();
      break;
    case 0:
      if (affa::expired(now, g_flightSince + kRenderTimeoutMs)) {
        ++g_timeouts;
        logmsg("render %lu never completed in %lu ms - abandoning it",
               static_cast<unsigned long>(g_inFlight),
               static_cast<unsigned long>(kRenderTimeoutMs));
        g_inFlight = 0;
        g_rows.invalidate();
      }
      break;
    default:
      break;
  }

  // ---- losing sync invalidates everything behind it -----------------------
  // The panel forgets its registration, and it may have been powered down entirely. Start
  // from the top rather than carrying on drawing into a screen that is not there.
  if (g_step != Step::WaitPanel && affa::hasFlag(st.sync, affa::SyncState::Failed)) {
    ++g_syncLosses;
    restart("panel stopped answering");
  }

  switch (g_step) {
    case Step::WaitPanel:
      // NOT waiting for registration: it is lazy and happens ON the first render, so waiting
      // for it here would wait for ever.
      if (affa::hasFlag(st.sync, affa::SyncState::Failed)) break;
      logmsg("panel answering - powering the display on");
      g_step = Step::PowerOn;
      break;

    case Step::PowerOn:
      if (g_inFlight) break;
      if (!issue(g_task.setPower(true), now, "setPower")) break;
      g_powerIsOn = true;
      g_deadline  = now + kWarmUpMs;
      g_step      = Step::WarmUp;
      break;

    case Step::WarmUp:
      // Two waits at once, and both are needed: the power render must be acknowledged, AND
      // the glass needs its settling time afterwards. Skipping the second is what makes the
      // first screen vanish and "setText does not work".
      if (g_inFlight) break;
      if (!affa::expired(now, g_deadline)) break;
      logmsg("glass warm - going live");
      g_popupNextMs = now + kPopupEveryMs;
      g_nextSoakMs  = now + kSoakReportMs;
      g_step        = Step::Live;
      break;

    case Step::Live:
      // The console's power switch is honoured here rather than in the handler, so it goes
      // through the same one-render-in-flight discipline as everything else.
      if (g_powerWanted != g_powerIsOn) {
        if (g_inFlight) break;
        if (!issue(g_task.setPower(g_powerWanted), now, "setPower")) break;
        g_powerIsOn = g_powerWanted;
        logmsg("display %s", g_powerIsOn ? "on" : "off");
        if (g_powerIsOn) g_rows.invalidate();   // it comes back blank
        break;
      }
      if (!g_powerIsOn) break;                  // nothing to draw on a dark panel
      pumpScreen(now);
      break;
  }

  soakReport(now);

  // loopTask runs at priority 1 and IDLE at 0, so a loop() that never yields starves IDLE and
  // the single-core C3 panics with "Task watchdog got triggered (IDLE)".
  delay(10);
}
