// 90_bench_ota — the interactive web console.
//
// This is the harness that proves the library on the real board. The rig at
// 192.168.100.85 has NO SERIAL CABLE and NO BUTTONS (they are in the car), so:
//
//   * WiFi and OTA come up FIRST and unconditionally. If they do not, the board is gone.
//   * Every public capability of the library is reachable and observable from a browser.
//   * `GET /` is one self-contained page — no CDN, no external asset. It is used in a car.
//
// WHERE THE BOUNDARY SITS (also in the README, deliberately in both places): this file
// contains NO menu logic and NO key mapping of its own. It translates HTTP into nav(),
// pressKey() and render calls, and nothing else. If something here started deciding which
// row to highlight or what a wheel detent means, the library would be missing an API.
//
// THREADING. The library is per-instance and unlocked: exactly one task may drive it. That
// task is loop(). The HTTP handlers run in the esp_http_server task and NEVER touch the
// display, the twin, the rings or the JSON buffer directly — they post a command into a
// one-slot mailbox and block on it. The wait is bounded by one loop iteration, and the
// HTTP task is allowed to sleep because it is not the one that must not.
//
//   pio run -e ex90_bench_ota
//   first flash over USB, thereafter http://<ip>/update

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <PsychicHttp.h>
#include <ElegantOTA.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <AffaDisplay.h>

#include "BenchLink.h"
#include "BenchPage.h"

#if !AFFA_PANEL_CARMINAT
#  error "90_bench_ota needs -D AFFA_PANEL_CARMINAT=1"
#endif
#if !AFFA_ENABLE_VIRTUAL_PANEL
#  error "90_bench_ota needs -D AFFA_ENABLE_VIRTUAL_PANEL=1 (it runs the twin)"
#endif
#if !AFFA_ENABLE_MENU
#  error "90_bench_ota drives the menu seam from the browser; it needs AFFA_ENABLE_MENU=1"
#endif

namespace {

// ---------------------------------------------------------------------------
// Board
// ---------------------------------------------------------------------------
// ESP32-C3 SuperMini: rx = GPIO4, tx = GPIO3. MeganeCAN's board is MIRRORED (rx = 3,
// tx = 4) — the same module, soldered the other way. The named struct is what stops the
// swap from becoming a silent bus with no error anywhere.
// BENCH EXPERIMENT, 2026-07-27: MegaOpen's BoardProfile.h asserts this board is mirrored
// relative to MeganeCAN (rx=4, tx=3) and MegaOpen did receive on GPIO4 — but MeganeCAN,
// which the owner confirms worked on this hardware, used rx=3, tx=4. Under rx=4/tx=3 this
// firmware transmits frames the panel ACKs (txErr stayed 0 over 26 frames, which only
// happens if something acknowledged them) and receives NOTHING. Both cannot be true, so
// try the MeganeCAN orientation and let the wire decide.
#ifndef BENCH_PINS_MIRRORED
#  define BENCH_PINS_MIRRORED 0
#endif
#if BENCH_PINS_MIRRORED
constexpr affa::CanPins kPins{ .rx = GPIO_NUM_3, .tx = GPIO_NUM_4 };   // MeganeCAN
#else
constexpr affa::CanPins kPins{ .rx = GPIO_NUM_4, .tx = GPIO_NUM_3 };   // MegaOpen
#endif
constexpr uint32_t      kBitrate = 500000;

// Read-only, from the namespace MegaOpen already writes (verified against
// MegaOpen/src/cfg/Config.cpp: NS = "megaopen", keys "ssid" and "pass").
constexpr const char* kNvsNamespace = "megaopen";
constexpr const char* kApSsid       = "AffaBench";
constexpr const char* kApPass       = "affabench";
constexpr const char* kMdnsName     = "affabench";
constexpr uint32_t    kStaJoinMs    = 15000;

struct ArduinoClock final : affa::IClock {
  uint32_t millis() const override { return ::millis(); }
};

// ---------------------------------------------------------------------------
// Objects
// ---------------------------------------------------------------------------
// `link` alone would be ambiguous with ::link() from <unistd.h>, which <Arduino.h> drags
// in. Everything here is prefixed.
affa::Esp32CanLink         g_hw;
BenchLink                  g_bench(g_hw);
ArduinoClock               g_clock;
affa::CarminatDisplay      g_display(g_bench, g_clock);

// The twin. In REAL mode it is PASSIVE and only decodes what we transmit, so /api/screen
// answers even with a live panel on the bus. In VIRTUAL mode it EMULATES and answers
// through its own loopback, which main's loop drains back into BenchLink::inject().
//
// AckMode::Declared and not the Done default: Declared answers PARTIAL while the declared
// FF_DL is unsatisfied and DONE at it, which is what the hardware does. With Done the twin
// would terminate every multi-frame transfer after frame 0 — the transmit FSM treats
// "DONE while bytes remain" as SUCCESS, because showMenu depends on exactly that — and the
// twin would see 8 bytes of a 96-byte screen. Declared is the only mode that reproduces
// showMenu at 13 frames (last PCI 0x2C), which is the hardware count.
affa::CarminatVirtualPanel g_twin;
affa::LoopbackLink<32>     g_twinLink;

PsychicHttpServer          g_server;

bool     g_canUp     = false;   // Esp32CanLink::begin() succeeded
bool     g_txGate    = true;    // the user's software TX gate setting
uint32_t g_rebootAt  = 0;       // 0 = no reboot scheduled

// ---------------------------------------------------------------------------
// Rings
// ---------------------------------------------------------------------------
// Plain circular arrays with a free-running head, written ONLY by the loop task and read
// ONLY by the loop task (the HTTP task reads them through the mailbox, so it never races
// them). No heap, fixed size, and nothing here is a function-local static.

constexpr uint16_t kFrameRing = 128;
constexpr uint16_t kLogRing   = 96;
constexpr uint16_t kKeyRing   = 32;

struct FrameRec {
  uint32_t ms;
  uint16_t id;
  uint8_t  len;
  uint8_t  dir;                 // 1 = Rx, 2 = Tx
  uint8_t  d[8];
};
struct LogRec {
  uint32_t ms;
  uint8_t  level;
  char     tag[12];
  char     msg[80];
};
struct KeyRec {
  uint32_t ms;
  uint16_t code;
  uint8_t  hold;
  char     src[10];             // "panel" | "local" | "wire" | "both"
};

FrameRec g_frames[kFrameRing];
LogRec   g_logs[kLogRing];
KeyRec   g_keys[kKeyRing];
uint32_t g_frameHead = 0;
uint32_t g_logHead   = 0;
uint32_t g_keyHead   = 0;

void pushFrame(const affa::Frame& f, affa::Direction d) {
  FrameRec& r = g_frames[g_frameHead % kFrameRing];
  r.ms  = millis();
  r.id  = static_cast<uint16_t>(f.id);
  r.len = f.len > 8 ? 8 : f.len;
  r.dir = (d == affa::Direction::Rx) ? 1 : 2;
  for (uint8_t i = 0; i < 8; ++i) r.d[i] = f.data[i];
  ++g_frameHead;
}

void pushLog(uint8_t level, const char* tag, const char* msg) {
  LogRec& r = g_logs[g_logHead % kLogRing];
  r.ms    = millis();
  r.level = level;
  snprintf(r.tag, sizeof(r.tag), "%s", tag ? tag : "");
  snprintf(r.msg, sizeof(r.msg), "%s", msg ? msg : "");
  ++g_logHead;
  // `if (Serial)` IS LOAD-BEARING ON THIS BOARD. HWCDC::write queues into a ring the USB
  // host drains; with no host attached the ring fills and every write then blocks for
  // Serial's TX timeout. At AFFA_LOG_LEVEL=5 on a live bus that is a stall per frame in
  // the one loop that must not stall — on a rig with no serial cable, which is this one.
  // HWCDC::operator bool() is the connected state, so an unattended board mirrors nothing.
  if (Serial)
    Serial.printf("[%lu] %u %s: %s\n", static_cast<unsigned long>(r.ms),
                  static_cast<unsigned>(level), r.tag, r.msg);
}

void logmsg(uint8_t level, const char* tag, const char* fmt, ...)
     __attribute__((format(printf, 3, 4)));
void logmsg(uint8_t level, const char* tag, const char* fmt, ...) {
  char buf[80];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  pushLog(level, tag, buf);
}

void pushKey(uint16_t code, bool hold, const char* src) {
  KeyRec& r = g_keys[g_keyHead % kKeyRing];
  r.ms   = millis();
  r.code = code;
  r.hold = hold ? 1 : 0;
  snprintf(r.src, sizeof(r.src), "%s", src);
  ++g_keyHead;
}

// The library's log sink. A sink is a LEAF: it must never call back into the library, and
// this one does not — it appends to a ring and prints.
struct RingSink final : affa::ILogSink {
  void write(uint8_t level, const char* tag, const char* msg) override {
    pushLog(level, tag, msg);
  }
};
RingSink g_sink;

// ---------------------------------------------------------------------------
// Measurements
// ---------------------------------------------------------------------------
// Everything here is measured, never estimated. docs/API.md §3b.8 names the one number
// the library cannot compute — the panel's ACK turnaround — and asks for it to be
// recorded; that is what ackMin/ackMean/ackMax are.
uint32_t g_pollLastUs  = 0;
uint32_t g_pollMaxUs   = 0;    // the worst loop period seen = the L1 bound (API.md §3b.3)
uint32_t g_keyStampUs  = 0;    // key frame observed / injection issued
uint32_t g_keyToCbUs   = 0;    // that -> KeyCb
uint32_t g_keyCbUs     = 0;
bool     g_awaitTx     = false;
uint32_t g_keyToWireUs = 0;    // KeyCb -> the first data frame it caused

uint32_t g_ackT0Us   = 0;
uint16_t g_ackAwait  = 0;      // the reply id we are waiting for, 0 = none
uint32_t g_ackMinUs  = 0xFFFFFFFF;
uint32_t g_ackMaxUs  = 0;
uint64_t g_ackSumUs  = 0;
uint32_t g_ackN      = 0;

uint32_t g_staleDropped = 0;   // completions with Result::Aborted — superseded renders

void resetMeasurements() {
  g_pollMaxUs = 0;
  g_keyToCbUs = g_keyToWireUs = 0;
  g_ackMinUs = 0xFFFFFFFF; g_ackMaxUs = 0; g_ackSumUs = 0; g_ackN = 0;
  g_staleDropped = 0;
}

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------
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
    case affa::Result::Busy:         return "Busy";
    case affa::Result::Aborted:      return "Aborted";
  }
  return "?";
}

const char* keyName(uint16_t code) {
  switch (code) {
    case 0x0000: return "Load";
    case 0x0001: return "SrcNext";
    case 0x0002: return "SrcPrev";
    case 0x0003: return "VolUp";
    case 0x0004: return "VolDown";
    case 0x0005: return "Pause";
    case 0x0101: return "RollUp";
    case 0x0141: return "RollDown";
  }
  return "Unknown";
}

bool keyFromCode(long code, affa::Key& out) {
  switch (code) {
    case 0x0000: case 0x0001: case 0x0002: case 0x0003:
    case 0x0004: case 0x0005: case 0x0101: case 0x0141:
      out = static_cast<affa::Key>(static_cast<uint16_t>(code));
      return true;
  }
  return false;
}

const char* modeName(affa::ScreenModel::Mode m) {
  switch (m) {
    case affa::ScreenModel::Mode::Menu: return "Menu";
    case affa::ScreenModel::Mode::Info: return "Info";
    default:                            return "None";
  }
}

// ---------------------------------------------------------------------------
// JSON writer — fixed buffer, no heap, bounds-checked, truncation-safe
// ---------------------------------------------------------------------------
char   g_out[12288];
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

// ---------------------------------------------------------------------------
// The demo menu — CONTENT ONLY. Every behaviour below it belongs to the library.
// ---------------------------------------------------------------------------
const char* const kModeList[] = { "Off", "Auto", "On" };

void onMenuChange(const affa::MenuItem& it, uint8_t fieldIndex, void*) {
  const affa::Field& f = it.fields[fieldIndex];
  if (f.type == affa::FieldType::List && f.list && f.value < f.listCount)
    logmsg(3, "menu", "%s[%u] -> %s", it.label, static_cast<unsigned>(fieldIndex),
         f.list[f.value]);
  else
    logmsg(3, "menu", "%s[%u] -> %ld", it.label, static_cast<unsigned>(fieldIndex),
         static_cast<long>(f.value));
}

void onMenuActivate(void*) { logmsg(3, "menu", "activated"); }

void buildDemoMenu() {
  affa::Menu& m = g_display.getMenu();

  // 1 — an integer field, 0..100 step 5, coarse step 20 on a Hold edge.
  affa::MenuItem bright;
  bright.label      = "Bright";
  bright.fields[0]  = affa::integerField(50, 0, 100, 5, 4, "%");
  bright.fieldCount = 1;
  bright.onChange   = &onMenuChange;
  m.addItem(bright);

  // 2 — a list field.
  affa::MenuItem mode;
  mode.label      = "Mode";
  mode.fields[0]  = affa::listField(kModeList, 3, 1);
  mode.fieldCount = 1;
  mode.onChange   = &onMenuChange;
  m.addItem(mode);

  // 3 — THREE fields on one item, which is what exercises nextFieldOrExit(). The third is
  // readOnlyField() so all three Field KINDS are on the glass at once; it is still an
  // integer field, and Select skips over it rather than editing it.
  affa::MenuItem tm;
  tm.label      = "Time";
  tm.fields[0]  = affa::integerField(12, 0, 23, 1, 6, "h");
  tm.fields[1]  = affa::integerField(30, 0, 59, 1, 10, "m");
  tm.fields[2]  = affa::readOnlyField(0, "s");
  tm.fieldCount = 3;
  tm.onChange   = &onMenuChange;
  tm.onActivate = nullptr;
  m.addItem(tm);

  (void)&onMenuActivate;
}

// ---------------------------------------------------------------------------
// The counter scenario (docs/API.md §3b.6), driven from the browser
// ---------------------------------------------------------------------------
struct CounterState {
  bool     run    = false;
  uint16_t hz     = 10;
  uint32_t nextMs = 0;
  uint32_t untilMs = 0;      // 0 = until stopped
  uint32_t n      = 0;
  uint32_t rejected = 0;     // renders the queue refused
} g_counter;

void counterTick(uint32_t now) {
  if (!g_counter.run) return;
  if (g_counter.untilMs && affa::expired(now, g_counter.untilMs)) {
    g_counter.run = false;
    logmsg(3, "bench", "counter stopped after %lu renders",
         static_cast<unsigned long>(g_counter.n));
    return;
  }
  if (!affa::expired(now, g_counter.nextMs)) return;
  const uint16_t hz = g_counter.hz ? g_counter.hz : 1;
  g_counter.nextMs = now + (1000u / hz ? 1000u / hz : 1);
  char buf[8];
  snprintf(buf, sizeof(buf), "%04lu", static_cast<unsigned long>(g_counter.n % 10000));
  const affa::Result r = g_display.setText(buf);
  ++g_counter.n;
  if (r != affa::Result::Ok) ++g_counter.rejected;
}

// ---------------------------------------------------------------------------
// The self test — the goal, end to end, as a state machine
// ---------------------------------------------------------------------------
// It is a state machine and not a blocking routine for the same reason the library is:
// the thing it waits for is delivered by the poll() it would otherwise be blocking.
struct SelfTest {
  enum class Phase : uint8_t { Idle, WaitSync, Enqueue, WaitDeliver, Done };
  Phase          phase   = Phase::Idle;
  bool           ran     = false;
  bool           ok      = false;
  uint32_t       startMs = 0;
  uint32_t       deadline = 0;
  affa::TxTicket ticket  = affa::kNoTicket;
  affa::Result   enq     = affa::Result::Ok;
  affa::Result   delivered = affa::Result::Ok;
  bool     syncOk = false, regOk = false, textOk = false;
  uint32_t syncMs = 0, deliverMs = 0;
  char     note[64] = {0};
} g_st;

void selfTestStart() {
  g_st = SelfTest{};
  g_st.phase    = SelfTest::Phase::WaitSync;
  g_st.ran      = true;
  g_st.startMs  = millis();
  g_st.deadline = g_st.startMs + 10000;
  snprintf(g_st.note, sizeof(g_st.note), "running");
  logmsg(3, "bench", "selftest started");
}

void selfTestFail(const char* why) {
  g_st.phase = SelfTest::Phase::Done;
  g_st.ok    = false;
  snprintf(g_st.note, sizeof(g_st.note), "%s", why);
  logmsg(2, "bench", "selftest failed: %s", why);
}

void selfTestTick(uint32_t now) {
  switch (g_st.phase) {
    case SelfTest::Phase::WaitSync:
      if (g_display.synced()) {
        g_st.syncOk = true;
        g_st.syncMs = now - g_st.startMs;
        g_st.phase  = SelfTest::Phase::Enqueue;
      } else if (affa::expired(now, g_st.deadline)) {
        selfTestFail("no sync within 10 s");
      }
      break;

    case SelfTest::Phase::Enqueue: {
      // The first payload after a resync drags the lazy 0x70 registration burst in front
      // of it, one probe per entry of the function table in declaration order. That is
      // what makes registered() latch, so it is tested by observing it, not by asking.
      g_st.enq = g_display.setText("AFFA OK");
      if (g_st.enq != affa::Result::Ok) { selfTestFail(resultName(g_st.enq)); break; }
      g_st.ticket   = g_display.lastEnqueued();
      g_st.deadline = now + 8000;
      g_st.phase    = SelfTest::Phase::WaitDeliver;
      break;
    }

    case SelfTest::Phase::WaitDeliver:
      if (affa::expired(now, g_st.deadline)) selfTestFail("delivery deadline");
      break;                                  // completion arrives through onComplete

    default:
      break;
  }
}

void selfTestComplete(affa::TxTicket t, affa::Result r, uint32_t now) {
  if (g_st.phase != SelfTest::Phase::WaitDeliver || t != g_st.ticket) return;
  g_st.delivered = r;
  g_st.textOk    = (r == affa::Result::Ok);
  g_st.regOk     = g_display.registered();
  g_st.deliverMs = now - g_st.startMs;
  g_st.ok        = g_st.syncOk && g_st.regOk && g_st.textOk;
  g_st.phase     = SelfTest::Phase::Done;
  snprintf(g_st.note, sizeof(g_st.note), "%s", g_st.ok ? "pass" : "fail");
  logmsg(g_st.ok ? 3 : 2, "bench", "selftest %s: setText delivered %s in %lu ms",
       g_st.ok ? "PASS" : "FAIL", resultName(r),
       static_cast<unsigned long>(g_st.deliverMs));
}

// On boot, once sync is established, attempt setText("AFFA OK") exactly once and record
// the result — so the goal is verifiable with a single GET.
bool g_bootTestDone = false;

// ---------------------------------------------------------------------------
// Library callbacks — all on the loop task
// ---------------------------------------------------------------------------
const char* g_injectLabel = nullptr;   // non-null only inside an injected pressKey()

void onTap(const affa::Frame& f, affa::Direction d, void*) {
  pushFrame(f, d);

  const uint16_t id = static_cast<uint16_t>(f.id);
  const uint32_t us = micros();

  if (d == affa::Direction::Tx) {
    // The twin sees exactly what a sniffer on the bus would see, in both modes. In REAL
    // mode it is PASSIVE and decodes only; in VIRTUAL mode it EMULATES and answers.
    g_twin.onFrame(f);

    if (id == affa::carminat::kIdSetText || id == affa::carminat::kIdNav) {
      if (g_awaitTx) { g_keyToWireUs = us - g_keyCbUs; g_awaitTx = false; }
      g_ackT0Us  = us;
      g_ackAwait = static_cast<uint16_t>(id | affa::kReplyFlag);
    }
    return;
  }

  // ---- inbound ----
  if (g_ackAwait && id == g_ackAwait) {
    const uint32_t dt = us - g_ackT0Us;
    if (dt < 200000u) {                       // ignore a stale pairing across a resync
      if (dt < g_ackMinUs) g_ackMinUs = dt;
      if (dt > g_ackMaxUs) g_ackMaxUs = dt;
      g_ackSumUs += dt;
      ++g_ackN;
    }
    g_ackAwait = 0;
  }
  // A key frame from the panel. The `03 89` guard is the library's; this is only a
  // timestamp, and it deliberately uses the same guard so it cannot stamp on `70 A3..`.
  if (id == affa::carminat::kIdKeyPressed && f.len >= 4 &&
      f.data[0] == 0x03 && f.data[1] == 0x89) {
    g_keyStampUs = us;
  }
}

void onKey(affa::Key k, affa::KeyEdge e, void*) {
  const uint32_t us = micros();
  if (g_keyStampUs) { g_keyToCbUs = us - g_keyStampUs; g_keyStampUs = 0; }
  g_keyCbUs = us;
  g_awaitTx = true;
  pushKey(static_cast<uint16_t>(k), e == affa::KeyEdge::Hold,
          g_injectLabel ? g_injectLabel : "panel");
}

affa::Result   g_lastEnqRes    = affa::Result::Ok;
affa::TxTicket g_lastEnqTicket = affa::kNoTicket;
affa::Result   g_lastDelRes    = affa::Result::Ok;
affa::TxTicket g_lastDelTicket = affa::kNoTicket;

void onComplete(affa::TxTicket t, affa::Result r, void*) {
  g_lastDelRes    = r;
  g_lastDelTicket = t;
  if (r == affa::Result::Aborted) ++g_staleDropped;
  selfTestComplete(t, r, millis());
}

void onSyncChanged(affa::SyncState s, void*) {
  logmsg(3, "bench", "sync 0x%02X synced=%d registered=%d", static_cast<unsigned>(s),
       g_display.synced() ? 1 : 0, g_display.registered() ? 1 : 0);
}

// ---------------------------------------------------------------------------
// The command mailbox
// ---------------------------------------------------------------------------
enum class Op : uint8_t {
  None, Status, Frames, Log, Keys, Screen,
  Text, Time, Power, ShowMenu, Highlight,
  Popup, PopupHide, Fullscreen, FullscreenHide, Confirm, Info, InfoHide,
  Nav, KeyPress, MenuShow, MenuState,
  Counter, Abort, TxGate, SelfTest, Mode, Reboot,
};

struct Cmd {
  Op   op = Op::None;
  char s1[40] = {0};
  char s2[40] = {0};
  char s3[40] = {0};
  long a = 0, b = 0, c = 0, d = 0;
};

SemaphoreHandle_t g_mx      = nullptr;
volatile bool     g_pending = false;
volatile bool     g_done    = false;
Cmd               g_cmd;

void execCmd(const Cmd& c);          // forward; runs on the loop task

// Called from the HTTP task. Returns false only if the loop never picked the command up,
// which means loop() is wedged — worth reporting as 503 rather than pretending.
bool post(const Cmd& c) {
  if (!g_mx) return false;
  if (xSemaphoreTake(g_mx, pdMS_TO_TICKS(2000)) != pdTRUE) return false;
  g_cmd    = c;
  g_done   = false;
  __sync_synchronize();
  g_pending = true;
  const uint32_t t0 = millis();
  bool ok = false;
  while (millis() - t0 < 2000) {
    if (g_done) { ok = true; break; }
    vTaskDelay(1);
  }
  g_pending = false;
  xSemaphoreGive(g_mx);
  return ok;
}

void pumpCmd() {
  if (!g_pending || g_done) return;
  execCmd(g_cmd);
  __sync_synchronize();
  g_done = true;
}

// ---------------------------------------------------------------------------
// JSON bodies
// ---------------------------------------------------------------------------
void jRenderDone(affa::Result r) {
  g_lastEnqRes = r;
  g_lastEnqTicket = (r == affa::Result::Ok) ? g_display.lastEnqueued() : affa::kNoTicket;
  jclear();
  // A gated-off capability answers NotSupported and not 404, so the console doubles as a
  // capability probe: every endpoint exists on every build.
  if (r == affa::Result::NotSupported) { jf("{\"error\":\"NotSupported\"}"); return; }
  jf("{\"ticket\":%u,\"enqueued\":", static_cast<unsigned>(g_lastEnqTicket));
  jstr(resultName(r));
  jf("}");
}

void jStatus() {
  const affa::Stats     s  = g_display.stats();
  const affa::SyncState ss = g_display.syncState();
  affa::Menu&           m  = g_display.getMenu();

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
    const String sid = WiFi.isConnected() ? WiFi.SSID() : String(kApSsid);
    jkv("ssid", sid.c_str()); jf(",");
  }
  jf("\"rssi\":%d},", WiFi.isConnected() ? static_cast<int>(WiFi.RSSI()) : 0);

  jkv("panel", g_bench.isVirtual() ? "virtual" : "real"); jf(",");
  jf("\"txGate\":%s,\"canUp\":%s,", g_txGate ? "true" : "false",
     g_canUp ? "true" : "false");

  jf("\"sync\":{\"state\":%u,\"synced\":%s,\"registered\":%s},",
     static_cast<unsigned>(ss),
     g_display.synced() ? "true" : "false",
     g_display.registered() ? "true" : "false");

  jf("\"busy\":%s,\"queued\":%u,\"txQueueDepth\":%u,",
     g_display.busy() ? "true" : "false",
     static_cast<unsigned>(g_display.queued()),
     static_cast<unsigned>(AFFA_TX_QUEUE_DEPTH));

  jf("\"lastEnqueue\":{\"ticket\":%u,", static_cast<unsigned>(g_lastEnqTicket));
  jkv("result", resultName(g_lastEnqRes)); jf("},");
  jf("\"lastDelivered\":{\"ticket\":%u,", static_cast<unsigned>(g_lastDelTicket));
  jkv("result", resultName(g_lastDelRes)); jf("},");

  jf("\"link\":{\"rxFrames\":%lu,\"txFrames\":%lu,\"txDropped\":%lu,"
     "\"ringOverflow\":%lu,\"txErr\":%lu,\"rxErr\":%lu,\"txFailed\":%lu},",
     static_cast<unsigned long>(s.rxFrames), static_cast<unsigned long>(s.txFrames),
     static_cast<unsigned long>(s.txDropped), static_cast<unsigned long>(s.ringOverflow),
     static_cast<unsigned long>(s.txErr), static_cast<unsigned long>(s.rxErr),
     static_cast<unsigned long>(s.txFailed));

  // Raw controller state. Stats::rxFrames counts what reached the LIBRARY; msgsToRx counts
  // what reached the DRIVER. Both zero means nothing is arriving at the peripheral at all;
  // msgsToRx climbing while rxFrames stays flat means esp32_can is not delivering what the
  // controller already has. That one number separates a bus problem from a library bug,
  // which is the question this rig exists to answer.
  {
    const auto d = g_hw.driverState();
    jf("\"drv\":{\"valid\":%s,\"state\":%u,\"msgsToRx\":%lu,\"msgsToTx\":%lu,"
       "\"txErr\":%lu,\"rxErr\":%lu,\"busErr\":%lu,\"arbLost\":%lu,\"rxMissed\":%lu},",
       d.valid ? "true" : "false", static_cast<unsigned>(d.state),
       static_cast<unsigned long>(d.msgsToRx), static_cast<unsigned long>(d.msgsToTx),
       static_cast<unsigned long>(d.txErr),   static_cast<unsigned long>(d.rxErr),
       static_cast<unsigned long>(d.busErr),  static_cast<unsigned long>(d.arbLost),
       static_cast<unsigned long>(d.rxMissed));
  }

  jf("\"menu\":{\"open\":%s,\"editing\":%s,\"count\":%u,\"selected\":%u,\"row\":%u},",
     m.isOpen() ? "true" : "false", m.isEditing() ? "true" : "false",
     static_cast<unsigned>(m.count()), static_cast<unsigned>(m.selectedIndex()),
     static_cast<unsigned>(m.selectedRow()));

  jf("\"counter\":{\"run\":%s,\"hz\":%u,\"n\":%lu,\"rejected\":%lu},",
     g_counter.run ? "true" : "false", static_cast<unsigned>(g_counter.hz),
     static_cast<unsigned long>(g_counter.n),
     static_cast<unsigned long>(g_counter.rejected));

  jf("\"lat\":{\"keyToCbUs\":%lu,\"keyToWireUs\":%lu,\"pollMaxUs\":%lu,"
     "\"staleDropped\":%lu,\"ackN\":%lu,\"ackMinUs\":%lu,\"ackMeanUs\":%lu,"
     "\"ackMaxUs\":%lu},",
     static_cast<unsigned long>(g_keyToCbUs),
     static_cast<unsigned long>(g_keyToWireUs),
     static_cast<unsigned long>(g_pollMaxUs),
     static_cast<unsigned long>(g_staleDropped),
     static_cast<unsigned long>(g_ackN),
     static_cast<unsigned long>(g_ackN ? g_ackMinUs : 0),
     static_cast<unsigned long>(g_ackN ? (uint32_t)(g_ackSumUs / g_ackN) : 0),
     static_cast<unsigned long>(g_ackMaxUs));

  jf("\"twin\":{\"emulating\":%s,\"synced\":%s,\"acks\":%lu,\"syncReplies\":%lu,"
     "\"lastDecodeMs\":%lu},",
     g_twin.emulating() ? "true" : "false", g_twin.synced() ? "true" : "false",
     static_cast<unsigned long>(g_twin.acksSent()),
     static_cast<unsigned long>(g_twin.syncRepliesSent()),
     static_cast<unsigned long>(g_twin.lastDecodeMs()));

  jf("\"selftest\":{\"ran\":%s,\"running\":%s,\"ok\":%s,",
     g_st.ran ? "true" : "false",
     (g_st.ran && g_st.phase != SelfTest::Phase::Done) ? "true" : "false",
     g_st.ok ? "true" : "false");
  jkv("note", g_st.note);
  jf("}}");
}

void jFrames(long n) {
  if (n <= 0 || n > kFrameRing) n = kFrameRing;
  const uint32_t head  = g_frameHead;
  const uint32_t avail = head < kFrameRing ? head : kFrameRing;
  const uint32_t take  = (static_cast<uint32_t>(n) < avail) ? static_cast<uint32_t>(n)
                                                            : avail;
  jclear();
  jf("{\"head\":%lu,\"f\":[", static_cast<unsigned long>(head));
  for (uint32_t i = 0; i < take; ++i) {
    if (jroom() < 80) break;
    const FrameRec& r = g_frames[(head - take + i) % kFrameRing];
    jf("%s[%lu,%u,%u,\"", i ? "," : "", static_cast<unsigned long>(r.ms),
       static_cast<unsigned>(r.dir), static_cast<unsigned>(r.id));
    for (uint8_t b = 0; b < r.len; ++b) jf("%02X", r.d[b]);
    jf("\"]");
  }
  jf("]}");
}

void jLog(long n) {
  if (n <= 0 || n > kLogRing) n = kLogRing;
  const uint32_t head  = g_logHead;
  const uint32_t avail = head < kLogRing ? head : kLogRing;
  const uint32_t take  = (static_cast<uint32_t>(n) < avail) ? static_cast<uint32_t>(n)
                                                            : avail;
  jclear();
  jf("{\"head\":%lu,\"l\":[", static_cast<unsigned long>(head));
  for (uint32_t i = 0; i < take; ++i) {
    if (jroom() < 200) break;
    const LogRec& r = g_logs[(head - take + i) % kLogRing];
    jf("%s[%lu,%u,", i ? "," : "", static_cast<unsigned long>(r.ms),
       static_cast<unsigned>(r.level));
    jstr(r.tag); jf(","); jstr(r.msg); jf("]");
  }
  jf("]}");
}

void jKeys() {
  const uint32_t head  = g_keyHead;
  const uint32_t take  = head < kKeyRing ? head : kKeyRing;
  jclear();
  jf("{\"head\":%lu,\"k\":[", static_cast<unsigned long>(head));
  for (uint32_t i = 0; i < take; ++i) {
    if (jroom() < 80) break;
    const KeyRec& r = g_keys[(head - take + i) % kKeyRing];
    jf("%s[%lu,%u,", i ? "," : "", static_cast<unsigned long>(r.ms),
       static_cast<unsigned>(r.code));
    jstr(keyName(r.code)); jf(",%u,", static_cast<unsigned>(r.hold));
    jstr(r.src); jf("]");
  }
  jf("]}");
}

void jScreen() {
  const affa::ScreenModel& s = g_twin.screen();
  jclear();
  jf("{"); jkv("mode", modeName(s.mode)); jf(",");
  jkv("header", s.header); jf(",");
  jkv("row0", s.row0); jf(",");
  jkv("row1", s.row1); jf(",");
  jf("\"row0Id\":%u,\"row1Id\":%u,\"sel\":%d,\"scroll\":%u,",
     static_cast<unsigned>(s.row0Id), static_cast<unsigned>(s.row1Id),
     static_cast<int>(s.sel), static_cast<unsigned>(s.scroll));
  jf("\"info\":[");
  for (uint8_t i = 0; i < s.infoCount && i < 3; ++i) {
    if (i) jf(",");
    jstr(s.info[i]);
  }
  jf("],\"lastDecodeMs\":%lu,", static_cast<unsigned long>(g_twin.lastDecodeMs()));
  jf("\"nowMs\":%lu,", static_cast<unsigned long>(millis()));
  jkv("panel", g_bench.isVirtual() ? "virtual" : "real");
  jf("}");
}

void jMenuState() {
  affa::Menu& m = g_display.getMenu();
  jclear();
  jf("{\"open\":%s,\"editing\":%s,\"selected\":%u,\"row\":%u,\"count\":%u,\"items\":[",
     m.isOpen() ? "true" : "false", m.isEditing() ? "true" : "false",
     static_cast<unsigned>(m.selectedIndex()), static_cast<unsigned>(m.selectedRow()),
     static_cast<unsigned>(m.count()));
  for (uint8_t i = 0; i < m.count(); ++i) {
    const affa::MenuItem* it = m.item(i);
    if (!it) continue;
    if (i) jf(",");
    jf("{"); jkv("label", it->label ? it->label : ""); jf(",\"fields\":[");
    for (uint8_t fi = 0; fi < it->fieldCount; ++fi) {
      const affa::Field& f = it->fields[fi];
      if (fi) jf(",");
      jf("{");
      jkv("type", f.type == affa::FieldType::List ? "list"
                                                  : (f.readOnly ? "readonly" : "integer"));
      jf(",\"value\":%ld,\"min\":%ld,\"max\":%ld,\"step\":%ld,\"mult\":%ld,",
         static_cast<long>(f.value), static_cast<long>(f.minValue),
         static_cast<long>(f.maxValue), static_cast<long>(f.step),
         static_cast<long>(f.stepMultiplier));
      jkv("unit", f.unit ? f.unit : "");
      if (f.type == affa::FieldType::List && f.list) {
        jf(",\"list\":[");
        for (uint8_t li = 0; li < f.listCount; ++li) {
          if (li) jf(",");
          jstr(f.list[li]);
        }
        jf("]");
      }
      jf("}");
    }
    jf("]}");
  }
  jf("]}");
}

void jSelfTest() {
  jclear();
  jf("{\"ran\":%s,\"ok\":%s,", g_st.ran ? "true" : "false", g_st.ok ? "true" : "false");
  jkv("note", g_st.note);
  jf(",\"steps\":[");
  jf("{"); jkv("name", "link"); jf(",\"ok\":%s,", g_display.stats().txFrames ? "true"
                                                                             : "false");
  jkv("detail", g_bench.isVirtual() ? "virtual bus" : (g_canUp ? "controller running"
                                                               : "controller down"));
  jf("},");
  jf("{"); jkv("name", "sync"); jf(",\"ok\":%s,\"ms\":%lu,", g_st.syncOk ? "true" : "false",
     static_cast<unsigned long>(g_st.syncMs));
  jkv("detail", g_display.synced() ? "PeerAlive latched" : "not synced");
  jf("},");
  jf("{"); jkv("name", "registration"); jf(",\"ok\":%s,", g_st.regOk ? "true" : "false");
  jkv("detail", "0x70 probe per function id, in table order");
  jf("},");
  jf("{"); jkv("name", "setText(\"AFFA OK\") enqueued"); jf(",\"ok\":%s,",
     g_st.enq == affa::Result::Ok ? "true" : "false");
  jkv("detail", resultName(g_st.enq)); jf("},");
  jf("{"); jkv("name", "delivered"); jf(",\"ok\":%s,\"ms\":%lu,",
     g_st.textOk ? "true" : "false", static_cast<unsigned long>(g_st.deliverMs));
  jkv("detail", resultName(g_st.delivered)); jf("},");
  jf("{"); jkv("name", "twin decoded"); jf(",\"ok\":%s,",
     g_twin.screen().header[0] ? "true" : "false");
  jkv("detail", g_twin.screen().header);
  jf("}]}");
}

// ---------------------------------------------------------------------------
// execCmd — the ONLY place that touches the library, and it runs on the loop task
// ---------------------------------------------------------------------------
void execCmd(const Cmd& c) {
  switch (c.op) {
    case Op::Status:    jStatus(); break;
    case Op::Frames:    jFrames(c.a); break;
    case Op::Log:       jLog(c.a); break;
    case Op::Keys:      jKeys(); break;
    case Op::Screen:    jScreen(); break;
    case Op::MenuState: jMenuState(); break;

    case Op::Text:
      jRenderDone(g_display.setText(c.s1, static_cast<uint8_t>(c.a)));
      break;
    case Op::Time:
      jRenderDone(g_display.setTime(c.s1));
      break;
    case Op::Power:
      jRenderDone(g_display.setPower(c.a != 0));
      break;
    case Op::ShowMenu:
      jRenderDone(g_display.showMenu(c.s1, c.s2, c.s3, static_cast<uint8_t>(c.a)));
      break;
    case Op::Highlight:
      jRenderDone(g_display.highlightItem(static_cast<uint8_t>(c.a)));
      break;
    case Op::Popup:
      jRenderDone(g_display.showPopupText(c.s1, static_cast<uint8_t>(c.a),
                                          static_cast<uint8_t>(c.b),
                                          static_cast<uint8_t>(c.c)));
      break;
    case Op::PopupHide:      jRenderDone(g_display.hidePopup()); break;
    case Op::Fullscreen:
      jRenderDone(g_display.showFullscreenText(c.s1, c.s2, c.s3));
      break;
    case Op::FullscreenHide: jRenderDone(g_display.hideFullscreenText()); break;
    case Op::Confirm:
      jRenderDone(g_display.showConfirmBox(c.s1, c.s2, c.s3));
      break;
    case Op::Info:
      jRenderDone(g_display.showInfoPopup(c.s1, c.s2, c.s3));
      break;
    case Op::InfoHide:       jRenderDone(g_display.hideInfoPopup()); break;

    case Op::Nav: {
      affa::NavCommand n;
      bool okName = true;
      if      (!strcmp(c.s1, "open"))   n = affa::NavCommand::Open;
      else if (!strcmp(c.s1, "next"))   n = affa::NavCommand::Next;
      else if (!strcmp(c.s1, "prev"))   n = affa::NavCommand::Prev;
      else if (!strcmp(c.s1, "select")) n = affa::NavCommand::Select;
      else if (!strcmp(c.s1, "back"))   n = affa::NavCommand::Back;
      else if (!strcmp(c.s1, "inc"))    n = affa::NavCommand::Increase;
      else if (!strcmp(c.s1, "dec"))    n = affa::NavCommand::Decrease;
      else { n = affa::NavCommand::Open; okName = false; }
      if (!okName) { jclear(); jf("{\"error\":\"BadArgument\"}"); break; }
      // nav() is the whole navigation surface. If this file ever needed to know which row
      // is highlighted in order to answer a D-pad press, the library would be missing an
      // API — see the README's boundary note.
      g_injectLabel = "local";
      g_keyStampUs  = micros();
      const affa::Result r = g_display.nav(n);
      g_injectLabel = nullptr;
      jclear();
      if (r == affa::Result::NotSupported) { jf("{\"error\":\"NotSupported\"}"); break; }
      jf("{\"ticket\":%u,\"enqueued\":", static_cast<unsigned>(g_display.lastEnqueued()));
      jstr(resultName(r)); jf("}");
      break;
    }

    case Op::KeyPress: {
      affa::Key k;
      if (!keyFromCode(c.a, k)) { jclear(); jf("{\"error\":\"BadArgument\"}"); break; }
      const affa::KeyEdge e = c.b ? affa::KeyEdge::Hold : affa::KeyEdge::Click;
      affa::KeySource src = affa::KeySource::Local;
      if      (!strcmp(c.s1, "wire")) src = affa::KeySource::Wire;
      else if (!strcmp(c.s1, "both")) src = affa::KeySource::Both;
      g_injectLabel = c.s1;
      g_keyStampUs  = micros();
      const affa::Result r = g_display.pressKey(k, e, src);
      g_injectLabel = nullptr;
      // A Wire-only press has no local effect, so KeyCb never fires and the ring would
      // stay silent about a frame that DID go on the bus. Record it here. Seeing "wire"
      // put a frame on the bus while "local" puts none is the point of this endpoint.
      if (src == affa::KeySource::Wire) {
        g_keyStampUs = 0;
        pushKey(static_cast<uint16_t>(k), e == affa::KeyEdge::Hold, "wire");
      }
      jclear();
      jf("{"); jkv("pressed", keyName(static_cast<uint16_t>(k)));
      jf(",\"hold\":%s,", c.b ? "true" : "false");
      jkv("src", c.s1); jf(",");
      jkv("result", resultName(r)); jf("}");
      break;
    }

    case Op::MenuShow: {
      affa::Menu& m = g_display.getMenu();
      affa::Result r;
      if (!m.isOpen()) r = g_display.nav(affa::NavCommand::Open);
      else             r = m.render();
      jRenderDone(r);
      break;
    }

    case Op::Counter: {
      g_counter.run = (c.a != 0);
      if (c.b > 0)  g_counter.hz = static_cast<uint16_t>(c.b > 200 ? 200 : c.b);
      g_counter.untilMs = (c.c > 0) ? (millis() + static_cast<uint32_t>(c.c)) : 0;
      if (g_counter.run) {
        g_counter.n = 0;
        g_counter.rejected = 0;
        g_counter.nextMs = millis();
        logmsg(3, "bench", "counter start %u Hz for %ld ms",
             static_cast<unsigned>(g_counter.hz), c.c);
      } else {
        logmsg(3, "bench", "counter stop after %lu renders",
             static_cast<unsigned long>(g_counter.n));
      }
      jclear();
      jf("{\"run\":%s,\"hz\":%u,\"n\":%lu}", g_counter.run ? "true" : "false",
         static_cast<unsigned>(g_counter.hz),
         static_cast<unsigned long>(g_counter.n));
      break;
    }

    case Op::Abort: {
      const uint8_t dropped = g_display.abortPending();
      logmsg(3, "bench", "abortPending() dropped %u", static_cast<unsigned>(dropped));
      jclear();
      jf("{\"dropped\":%u,\"busy\":%s}", static_cast<unsigned>(dropped),
         g_display.busy() ? "true" : "false");
      break;
    }

    case Op::TxGate: {
      g_txGate = (c.a != 0);
      g_bench.setGate(g_txGate);
      // The library's own equivalent, kept in step. Neither is a driver mode change:
      // Esp32CanLink::setTxEnabled() only makes send() return false, and the controller
      // keeps ACKing other nodes at the link layer — which on a two-node bus is required.
      if (g_canUp) g_hw.setTxEnabled(g_txGate && !g_bench.isVirtual());
      logmsg(3, "bench", "software TX gate %s", g_txGate ? "open" : "shut");
      jclear();
      jf("{\"txGate\":%s}", g_txGate ? "true" : "false");
      break;
    }

    case Op::SelfTest:
      if (!g_st.ran || g_st.phase == SelfTest::Phase::Done) selfTestStart();
      jSelfTest();
      break;

    case Op::Mode: {
      const bool wantVirtual = (strcmp(c.s1, "virtual") == 0);
      if (wantVirtual != g_bench.isVirtual()) {
        g_bench.setVirtual(wantVirtual);
        g_twinLink.clear();
        g_twin.setEmulate(wantVirtual);          // EMULATION off the bus, PASSIVE on it
        g_twin.begin(g_twinLink, g_clock);
        if (g_canUp) g_hw.setTxEnabled(g_txGate && !wantVirtual);
        // begin() resets the FSMs and cancels the queue; the handshake restarts against
        // whichever peer is now on the other side.
        g_display.begin();
        resetMeasurements();
        g_bootTestDone = false;
        logmsg(3, "bench", "panel mode -> %s", wantVirtual ? "virtual" : "real");
      }
      jclear();
      jf("{"); jkv("panel", g_bench.isVirtual() ? "virtual" : "real");
      jf(",\"emulating\":%s,\"txGate\":%s}", g_twin.emulating() ? "true" : "false",
         g_txGate ? "true" : "false");
      break;
    }

    case Op::Reboot:
      g_rebootAt = millis() + 400;    // after the reply has gone out
      logmsg(2, "bench", "reboot requested");
      jclear();
      jf("{\"reboot\":true,\"inMs\":400}");
      break;

    default:
      jclear();
      jf("{\"error\":\"BadArgument\"}");
      break;
  }
}

// ---------------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------------
void pstr(PsychicRequest* r, const char* key, char* dst, size_t n, const char* def = "") {
  const char* src = def;
  String v;
  if (r->hasParam(key)) { v = r->getParam(key)->value(); src = v.c_str(); }
  snprintf(dst, n, "%s", src);
}

long pnum(PsychicRequest* r, const char* key, long def) {
  if (!r->hasParam(key)) return def;
  const String v = r->getParam(key)->value();
  if (!v.length()) return def;
  return strtol(v.c_str(), nullptr, 0);       // base 0 => "0x60" and "96" both work
}

esp_err_t run(PsychicRequest* r, const Cmd& c) {
  if (!post(c))
    return r->reply(503, "application/json", "{\"error\":\"Busy\"}");
  return r->reply(200, "application/json", g_out);
}

esp_err_t simple(PsychicRequest* r, Op op) {
  Cmd c; c.op = op;
  return run(r, c);
}

void routes() {
  g_server.on("/", HTTP_GET, [](PsychicRequest* r) {
    return r->reply(200, "text/html", kBenchPage);
  });

  g_server.on("/api/status", HTTP_GET, [](PsychicRequest* r) { return simple(r, Op::Status); });
  g_server.on("/api/keys",   HTTP_GET, [](PsychicRequest* r) { return simple(r, Op::Keys); });
  g_server.on("/api/screen", HTTP_GET, [](PsychicRequest* r) { return simple(r, Op::Screen); });
  g_server.on("/api/menu/state", HTTP_GET, [](PsychicRequest* r) { return simple(r, Op::MenuState); });
  g_server.on("/api/menu/show",  HTTP_GET, [](PsychicRequest* r) { return simple(r, Op::MenuShow); });
  g_server.on("/api/popup/hide", HTTP_GET, [](PsychicRequest* r) { return simple(r, Op::PopupHide); });
  g_server.on("/api/fullscreen/hide", HTTP_GET, [](PsychicRequest* r) { return simple(r, Op::FullscreenHide); });
  g_server.on("/api/info/hide",  HTTP_GET, [](PsychicRequest* r) { return simple(r, Op::InfoHide); });
  g_server.on("/api/abort",      HTTP_GET, [](PsychicRequest* r) { return simple(r, Op::Abort); });
  g_server.on("/api/selftest",   HTTP_GET, [](PsychicRequest* r) { return simple(r, Op::SelfTest); });
  g_server.on("/api/reboot",     HTTP_GET, [](PsychicRequest* r) { return simple(r, Op::Reboot); });

  g_server.on("/api/frames", HTTP_GET, [](PsychicRequest* r) {
    Cmd c; c.op = Op::Frames; c.a = pnum(r, "n", kFrameRing);
    return run(r, c);
  });
  g_server.on("/api/log", HTTP_GET, [](PsychicRequest* r) {
    Cmd c; c.op = Op::Log; c.a = pnum(r, "n", 48);
    return run(r, c);
  });

  g_server.on("/api/text", HTTP_GET, [](PsychicRequest* r) {
    Cmd c; c.op = Op::Text;
    pstr(r, "t", c.s1, sizeof(c.s1));
    c.a = pnum(r, "d", 255);
    return run(r, c);
  });
  g_server.on("/api/time", HTTP_GET, [](PsychicRequest* r) {
    Cmd c; c.op = Op::Time;
    pstr(r, "hhmm", c.s1, sizeof(c.s1));
    return run(r, c);
  });
  g_server.on("/api/state", HTTP_GET, [](PsychicRequest* r) {
    Cmd c; c.op = Op::Power; c.a = pnum(r, "on", 1);
    return run(r, c);
  });
  g_server.on("/api/menu", HTTP_GET, [](PsychicRequest* r) {
    Cmd c; c.op = Op::ShowMenu;
    pstr(r, "h", c.s1, sizeof(c.s1));
    pstr(r, "a", c.s2, sizeof(c.s2));
    pstr(r, "b", c.s3, sizeof(c.s3));
    c.a = pnum(r, "s", affa::carminat::kScrollDown);
    return run(r, c);
  });
  g_server.on("/api/highlight", HTTP_GET, [](PsychicRequest* r) {
    Cmd c; c.op = Op::Highlight; c.a = pnum(r, "row", 0);
    return run(r, c);
  });
  g_server.on("/api/popup", HTTP_GET, [](PsychicRequest* r) {
    Cmd c; c.op = Op::Popup;
    pstr(r, "t", c.s1, sizeof(c.s1));
    c.a = pnum(r, "icon", affa::carminat::kPopupIcon);
    c.b = pnum(r, "src",  affa::carminat::kSrcIconNone);
    c.c = pnum(r, "fmt",  affa::carminat::kFormatPlain);
    return run(r, c);
  });
  g_server.on("/api/fullscreen", HTTP_GET, [](PsychicRequest* r) {
    Cmd c; c.op = Op::Fullscreen;
    pstr(r, "l1", c.s1, sizeof(c.s1));
    pstr(r, "l2", c.s2, sizeof(c.s2));
    pstr(r, "l3", c.s3, sizeof(c.s3));
    return run(r, c);
  });
  g_server.on("/api/confirm", HTTP_GET, [](PsychicRequest* r) {
    Cmd c; c.op = Op::Confirm;
    pstr(r, "cap", c.s1, sizeof(c.s1));
    pstr(r, "r1",  c.s2, sizeof(c.s2));
    pstr(r, "r2",  c.s3, sizeof(c.s3));
    return run(r, c);
  });
  g_server.on("/api/info", HTTP_GET, [](PsychicRequest* r) {
    Cmd c; c.op = Op::Info;
    pstr(r, "l1", c.s1, sizeof(c.s1));
    pstr(r, "l2", c.s2, sizeof(c.s2));
    pstr(r, "l3", c.s3, sizeof(c.s3));
    return run(r, c);
  });
  g_server.on("/api/nav", HTTP_GET, [](PsychicRequest* r) {
    Cmd c; c.op = Op::Nav;
    pstr(r, "c", c.s1, sizeof(c.s1), "open");
    return run(r, c);
  });
  g_server.on("/api/key", HTTP_GET, [](PsychicRequest* r) {
    Cmd c; c.op = Op::KeyPress;
    c.a = pnum(r, "k", -1);
    c.b = pnum(r, "hold", 0);
    pstr(r, "src", c.s1, sizeof(c.s1), "local");
    return run(r, c);
  });
  g_server.on("/api/counter", HTTP_GET, [](PsychicRequest* r) {
    Cmd c; c.op = Op::Counter;
    c.a = pnum(r, "run", 1);
    c.b = pnum(r, "hz",  10);
    c.c = pnum(r, "to",  0);
    return run(r, c);
  });
  g_server.on("/api/txgate", HTTP_GET, [](PsychicRequest* r) {
    Cmd c; c.op = Op::TxGate; c.a = pnum(r, "on", 1);
    return run(r, c);
  });
  g_server.on("/api/mode", HTTP_GET, [](PsychicRequest* r) {
    Cmd c; c.op = Op::Mode;
    pstr(r, "panel", c.s1, sizeof(c.s1), "real");
    return run(r, c);
  });
}

// ---------------------------------------------------------------------------
// Network — this runs BEFORE anything else and is not allowed to fail silently
// ---------------------------------------------------------------------------
void startNetwork() {
  // Read-only open of the namespace MegaOpen already populated. Read-only matters: this
  // example must never be the reason those credentials change, and an NVS WRITE stops CAN
  // reception outright (the TWAI ISR is not in IRAM) for the duration of the flash write.
  Preferences p;
  String ssid, pass;
  if (p.begin(kNvsNamespace, /*readOnly=*/true)) {
    ssid = p.getString("ssid", "");
    pass = p.getString("pass", "");
    p.end();
  }

  WiFi.persistent(false);
  WiFi.setSleep(true);      // the C3's single radio has to interleave WiFi with CAN timing

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

  Serial.printf("\n[wifi] %s  ssid=%s  ip=%s\n",
                sta ? "STA" : "AP (fallback)",
                sta ? ssid.c_str() : kApSsid,
                sta ? WiFi.localIP().toString().c_str()
                    : WiFi.softAPIP().toString().c_str());
  Serial.printf("[wifi] console http://%s/   OTA http://%s/update   mdns %s.local\n",
                sta ? WiFi.localIP().toString().c_str() : WiFi.softAPIP().toString().c_str(),
                sta ? WiFi.localIP().toString().c_str() : WiFi.softAPIP().toString().c_str(),
                kMdnsName);
}

void startHttp() {
  // One slot per route plus ElegantOTA's own. Overflowing this does NOT fail loudly — the
  // excess routes simply never register and the page looks broken with no clue why.
  g_server.config.max_uri_handlers = 48;
  g_server.listen(80);
  routes();

  // An OTA write stops CAN reception outright: the TWAI ISR is not in IRAM, so a flash
  // write looks exactly like a dead panel. Shut our own transmitter for the duration so we
  // are not shouting at a bus we cannot hear, and expect PeerLost + a resync afterwards.
  // This is also why AFFA_PEER_TIMEOUT_MS must stay above the longest flash write.
  ElegantOTA.onStart([]() {
    g_bench.setGate(false);
    if (g_canUp) g_hw.setTxEnabled(false);
    pushLog(2, "ota", "update started — CAN TX gated off, RX will stall on flash writes");
  });
  ElegantOTA.onEnd([](bool success) {
    pushLog(success ? 3 : 1, "ota", success ? "update ok, rebooting" : "update FAILED");
  });
  ElegantOTA.begin(&g_server);
}

}  // namespace

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  // Belt to the `if (Serial)` braces in pushLog(): even when the host is attached and then
  // goes away mid-write, a zero timeout makes the write fail instead of stalling the loop.
  Serial.setTxTimeoutMs(0);
  delay(300);                     // the application may sleep; the library may not

  Serial.println("\nAffaDisplay 90_bench_ota");

  g_mx = xSemaphoreCreateMutex();

  // 1. NETWORK FIRST, ALWAYS. Everything after this can fail and still leave a way in.
  startNetwork();
  startHttp();

  // 2. CAN. A failure here is reported, never fatal — the console then runs in virtual
  //    mode against the twin, which is the whole point of having one.
  // Ask for bus-off auto-recovery. Measured on this rig without it: the controller took
  // 17 bus errors, went bus-off, the driver's watchdog called twai_initiate_recovery(),
  // and that left it in TWAI_STATE_STOPPED — where it neither transmits NOR RECEIVES, for
  // ever. `rxFrames` stuck at 0 with no errors climbing looks exactly like a dead bus or a
  // broken transceiver, and it is neither: it is a stopped peripheral nobody restarted.
  // A bench must dig itself out; 2 s is the driver's own default outage.
  // BENCH_LISTEN_ONLY=1 answers "can we read this bus at all?" and nothing else — no ACK,
  // so no handshake and no text, by design.
#ifndef BENCH_LISTEN_ONLY
#  define BENCH_LISTEN_ONLY 0
#endif
  // forceRecoveryMs=0. MEASURED, do not "improve" this back to 2000: with auto-recovery
  // armed against a bus that keeps going bus-off, every cycle costs the driver's full
  // disable/2 s/enable, and they accumulate INSIDE setup(). The serial log showed WiFi up
  // immediately, `AFFA: begin` at 3.4 s, and the end of CAN bring-up at 185 999 ms — three
  // minutes before the console answered its first request, which reads exactly like a
  // board that never booted. A bench that cannot be reached is worse than a bus that
  // stays down, and isLive() reports the latter honestly.
  // 250, not 0 and not 2000. Measured on this rig, both ends:
  //   0    — a bus-off ends in TWAI_STATE_STOPPED and the link stays down until a reboot.
  //          At power-up the panel is not ready for a moment, our heartbeat goes
  //          unacknowledged, and ~30 frames later the controller is stopped for good:
  //          rxFrames 0, busErr frozen at 62, txFrames still climbing into nothing.
  //   2000 — recovery works but each cycle costs the driver's full disable/delay/enable,
  //          and they accumulate inside setup(): `bench: up` appeared at 185 999 ms, which
  //          is indistinguishable from a board that never booted.
  // 250 ms recovers fast enough that a panel which wakes a second late is caught, and
  // cheap enough that a genuinely dead bus costs seconds of bring-up rather than minutes.
  g_canUp = g_hw.begin(kPins, kBitrate, /*forceRecoveryMs=*/250,
                       BENCH_LISTEN_ONLY ? affa::Esp32CanLink::LinkMode::ListenOnly
                                         : affa::Esp32CanLink::LinkMode::Normal);
  if (!g_canUp)
    Serial.println("[can] controller did not come up — check pins, transceiver, "
                   "termination. Falling back to the virtual panel.");

  // 3. The library.
  g_display.setLogSink(&g_sink);
  g_display.onFrame(&onTap, nullptr);
  g_display.onKey(&onKey, nullptr);
  g_display.onComplete(&onComplete, nullptr);
  g_display.onSync(&onSyncChanged, nullptr);
  buildDemoMenu();

  // 4. The twin. PASSIVE next to a real panel — two ACKers on one bus is the failure this
  //    switch exists to prevent — EMULATION when there is no bus.
  g_twin.setAckMode(affa::VirtualPanelBase::AckMode::Declared);
  g_twin.setEmulate(!g_canUp);
  g_twin.begin(g_twinLink, g_clock);
  g_bench.setVirtual(!g_canUp);
  if (g_canUp) g_hw.setTxEnabled(true);

  g_display.begin();
  g_pollLastUs = micros();

  logmsg(3, "bench", "up: panel=%s can=%d", g_bench.isVirtual() ? "virtual" : "real",
       g_canUp ? 1 : 0);
}

void loop() {
  // 1. The library. Nothing in this loop blocks.
  g_display.poll();

  const uint32_t nowUs = micros();
  const uint32_t dt    = nowUs - g_pollLastUs;
  g_pollLastUs = nowUs;
  if (dt > g_pollMaxUs && dt < 1000000u) g_pollMaxUs = dt;

  // 2. Hand the twin's replies back. In REAL mode the twin is PASSIVE and this is empty.
  {
    affa::Frame f;
    while (g_twinLink.takeSent(f)) g_bench.inject(f);
  }

  const uint32_t now = millis();

  // 3. Deadline-driven application work. No counters, no delays.
  counterTick(now);
  if (g_st.ran && g_st.phase != SelfTest::Phase::Done) selfTestTick(now);

  // Once, on the first sync: prove the goal without anybody asking.
  if (!g_bootTestDone && g_display.synced()) {
    g_bootTestDone = true;
    selfTestStart();
  }

  // 4. HTTP commands, executed HERE so the library only ever sees one task.
  pumpCmd();

  ElegantOTA.loop();

  if (g_rebootAt && affa::expired(now, g_rebootAt)) ESP.restart();

  // THE ONE SLEEP IN THIS FILE, AND IT IS APPLICATION POLICY, NOT LIBRARY BEHAVIOUR.
  //
  // loopTask runs at FreeRTOS priority 1 and the IDLE task at 0, so a loop() that never
  // blocks starves IDLE outright and the single-core C3 panics with "Task watchdog got
  // triggered (IDLE)" — a reboot loop on a board whose only way back in is the network
  // this reboot loop never brings up. One tick is the smallest cure. MegaOpen ends its
  // loop the same way on the same board for the same reason.
  //
  // What it costs: L1 — the key-to-callback latency — is bounded by the poll period, so
  // this makes it ~1 ms. docs/API.md §3b's worked example assumes 5 ms, so this is better,
  // not worse, and /api/status reports the MEASURED worst period (lat.pollMaxUs) rather
  // than asking anyone to take that on trust.
  //
  // What it is NOT: the library still never sleeps. No render waits, no ACK is waited for,
  // no key is delayed behind a transfer. Removing this line does not make anything faster;
  // it makes the board unbootable.
  delay(1);
}
