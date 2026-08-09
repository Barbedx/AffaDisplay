// 17_mediascreen — the console. Everything this library can put on a panel, plus the
// probes for the bytes it does not understand yet.
//
// ONE COMMAND ROUTE: /api/cmd?op=<name>, every one returning {"ok":bool,"msg":string}. The
// version with a route per feature had two that silently did nothing for a day.
//
//   pio run -e ex17_mediascreen -t upload
//   http://<ip>/   console      http://<ip>/update   OTA
//   /api/cmd?op=panic           stops everything, from a bare address bar
//   /api/cmd?op=lasttx          the last payload the builder made, as hex
//   /api/cmd?op=raw&b=10 0E ..  send bytes you chose through the ordinary path
//
// THE BUS IS THE CONSTRAINT. One image is 304 bytes = 44 frames at BlockSize 1, so every
// consecutive frame costs a round trip: 47-64 ms measured, ~23% of the link at 250 ms.
// Identical frames are never resent.

#include <Arduino.h>
#include <AffaDisplay.h>
#include <ESPmDNS.h>       // LDF only scans THIS file for #include, so the framework
#include <ElegantOTA.h>     // libraries shared/net.h uses have to be named here too, or
#include <Preferences.h>    // their include paths are never added to the build
#include <PsychicHttp.h>
#include <WiFi.h>

#include "../shared/net.h"          // WiFi, mDNS, OTA and the PsychicHttp knobs, once
#include "../shared/media_render.h"
#include "../shared/nav_images.h"   // one generator, one header, two consumers

#if !AFFA_PANEL_CARMINAT
#  error "17_mediascreen needs the Carminat panel: build with -D AFFA_PANEL_CARMINAT=1"
#endif

namespace {

constexpr gpio_num_t kRxPin   = GPIO_NUM_5;   // the standard collin80 stack, as 09..16
constexpr gpio_num_t kTxPin   = GPIO_NUM_4;
constexpr uint32_t   kBitrate = 500000;

constexpr const char* kPrefsNamespace = "affamedia";
constexpr const char* kApSsid   = "AffaMedia";
constexpr const char* kApPass   = "affa1234";
constexpr const char* kMdnsName = "affamedia";

struct ArduinoClock final : affa::IClock {
  uint32_t millis() const override { return ::millis(); }
};

affa::CanCommonLink g_link;
ArduinoClock        g_clock;
affa::rtos::AffaTask g_task;      // the library polls itself; loop() never calls poll()
PsychicHttpServer   g_server;

// The panel family is a BOOT choice: Carminat syncs on 0x3AF, UpdateList on 0x3DF, and the
// handshake, ids and text encoding all differ. Stored in NVS, applied on the next boot, and
// the console says so rather than pretending a runtime switch is possible.
enum class Family : uint8_t { Carminat = 0, UpdateList = 1 };
Family g_family = Family::Carminat;

affa::CarminatDisplay* g_carminat = nullptr;
affa::IDisplay*        g_panel    = nullptr;
affa::AffaDisplayBase* g_base     = nullptr;

void logmsg(const char* fmt, ...);
affa::Submitted takeMainLine();          // defined below; the opening's last step needs it

// Set by any command that needs a restart to take effect. The route restarts AFTER the
// response has gone out, so the browser still gets its verdict.
bool g_wantReboot = false;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
enum class Scene : uint8_t {
  Spectrum = 0, Vu, Wave, Clock, Stars, Bounce, Rings, Eyes,
  Globe, Tryzub, TryzubClock, Renault, Dash, Gauges, Combo, FontSheet, Checker,
  EyesStill, EyesLeft, EyesBlink, EyesHappy,
  Custom, Blank, kCount
};
const char* kSceneName[] = {
  "spectrum","vu","wave","clock","stars","bounce","rings","eyes",
  "globe","tryzub","tryzubclock","renault","dash","gauges","combo","fontsheet","checker",
  "eyesstill","eyesleft","eyesblink","eyeshappy",
  "custom","blank"
};
static_assert(sizeof(kSceneName)/sizeof(kSceneName[0]) == static_cast<size_t>(Scene::kCount),
              "scene name table drifted from the enum");

Scene    g_scene    = Scene::Tryzub;
bool     g_paneOn   = true;
uint32_t g_periodMs = 250;
uint32_t g_nextFrameMs = 0, g_sceneFrame = 0, g_uptimeS = 0, g_nextSecondMs = 0;

media::Bars g_bars; media::Vu g_vu; media::Wave g_wave;
media::Stars g_stars; media::Bounce g_bounce; media::Eyes g_eyes;

// Double buffered: showNavBitmap BORROWS until the ticket completes, so drawing into the
// buffer still being transmitted would tear the image on the wire.
uint8_t  g_frame[2][media::kBytes];
uint8_t  g_custom[media::kBytes];        // whatever the browser's editor drew
uint8_t  g_drawInto = 0;
bool     g_forceFrame = true, g_everSent = false;
volatile bool  g_navBusy = false;
affa::TxTicket g_navTicket = affa::kNoTicket;

uint8_t g_menuBuf[affa::CarminatDisplay::menuScreenBytes(affa::carminat::kMenuMaxItems)];
volatile bool  g_menuBusy = false;
affa::TxTicket g_menuTicket = affa::kNoTicket;

// ---- the OEM's own Navigation menu, byte for byte ---------------------------
// docs/captures/"some more logs from origin"/"mENU NAVIGATION MAIN SCREEN AFTER BACK.csv",
// one ISO-TP message, `10 C6` = FF_DL 198 = 36 + 27*6, PCI included.
//
// IT IS THE CONTROL. Press it and if the book is still drawn, a builder change moved —
// not the panel. That is how the pictogram field was settled on 2026-08-07.
//
//   op=oem                 verbatim, [3][4] = 3D 14, the book
//   op=oem&i0=0x80&i1=0x00 the same 200 bytes with the "no icon" pair
constexpr uint16_t kOemNavBytes = 200;
const uint8_t kOemNavMenu[kOemNavBytes] = {
  0x10, 0xC6, 0x21, 0x01, 0x01, 0x3D, 0x14, 0x00, 0x86, 0xFF, 0x03, 0x4E,
  0x61, 0x76, 0x69, 0x67, 0x61, 0x74, 0x69, 0x6F, 0x6E, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x01, 0x00, 0x44, 0x65, 0x73, 0x74, 0x69, 0x6E, 0x61, 0x74, 0x69,
  0x6F, 0x6E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x44, 0x65, 0x73, 0x74, 0x2E, 0x20,
  0x6D, 0x65, 0x6D, 0x6F, 0x72, 0x79, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x4C, 0x61, 0x73,
  0x74, 0x20, 0x64, 0x65, 0x73, 0x74, 0x69, 0x6E, 0x61, 0x74, 0x69, 0x6F,
  0x6E, 0x73, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03,
  0x50, 0x2E, 0x4F, 0x2E, 0x49, 0x2E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x04, 0x52, 0x6F, 0x75, 0x74, 0x65, 0x20, 0x73, 0x65, 0x74,
  0x74, 0x69, 0x6E, 0x67, 0x73, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x42, 0x61, 0x63, 0x6B, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
// Wire indices, not payload indices: the payload starts at 2 because the PCI is in here.
constexpr uint16_t kOemAtSel = 4;   // payload [2] — selection
constexpr uint16_t kOemAtI0  = 5;   // payload [3] — gutter glyph index
constexpr uint16_t kOemAtI1  = 6;   // payload [4] — scrollbar thumb
static_assert(kOemNavBytes == 2 + 36 + 27 * 6, "36 + 27N, PCI included");

// ---- text, with scrolling as an option rather than a fact --------------------
// The main line shows 8 characters and an info-menu row shows 8 — both measured with a
// column ruler, not assumed. Anything longer has to scroll or be cut, and which one is the
// application's choice, so it is a per-target switch here.
constexpr uint8_t kMainWidth = 8, kRowWidth = 8;

struct Scroller {
  char     text[64] = "";
  bool     on       = false;
  uint16_t at       = 0;
  uint32_t periodMs = 700, nextMs = 0;
  char     sent[16] = "";

  // TWO SPACES OF GAP at the wrap: without them the loop reads as a jump cut.
  void window(char* out, uint8_t w) {
    const size_t len = strlen(text);
    if (!on || len <= w) { snprintf(out, w + 1u, "%-*s", w, text); return; }
    const size_t span = len + 2;
    for (uint8_t i = 0; i < w; ++i) {
      const size_t k = (at + i) % span;
      out[i] = (k < len) ? text[k] : ' ';
    }
    out[w] = 0;
    at = static_cast<uint16_t>((at + 1) % span);
  }
  // True only when the visible window actually changed. A display shows what it was last
  // told, so telling it again is not free — the previous version repainted a static line
  // every 700 ms for ever.
  bool changed(const char* w) { if (!strcmp(w, sent)) return false;
                                snprintf(sent, sizeof(sent), "%s", w); return true; }
  void reset() { at = 0; sent[0] = 0; }
};
Scroller g_main;
Scroller g_row[3];
bool     g_rowsLive = false;

uint8_t g_icon = affa::carminat::kIconsNone;
uint8_t g_src  = affa::carminat::kSrcIconNone;
uint8_t g_fmt  = affa::carminat::kFormatPlain;
// The SECOND icon bank, payload byte 4. It was hard-coded 0x55 inside the builder and
// unreachable from any caller, which is exactly the shape that hides a symptom: a CD icon
// that is lit on the glass no matter what the first bank is set to. Exposed so the byte can
// be swept from the console instead of guessed at. 0x55 is the captured value.
uint8_t g_fmt2 = affa::carminat::kIconBank2;

uint32_t g_holdMs = 0, g_holdUntil = 0;   // optional auto-return of the main line

// ---- counters ---------------------------------------------------------------
volatile uint32_t g_frames = 0, g_ok = 0, g_fail = 0, g_drops = 0, g_txMs = 0;
uint32_t g_frameStart = 0;

// ---------------------------------------------------------------------------
// Rings: log, wire, keys
// ---------------------------------------------------------------------------
portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;

constexpr int kLogLines = 32;
char     g_log[kLogLines][88];
uint16_t g_logHead = 0;

void logmsg(const char* fmt, ...) {
  char line[88];
  va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof(line), fmt, ap); va_end(ap);
  portENTER_CRITICAL(&g_mux);
  snprintf(g_log[g_logHead], sizeof(g_log[0]), "%8lu %s", (unsigned long)::millis(), line);
  g_logHead = (g_logHead + 1) % kLogLines;
  portEXIT_CRITICAL(&g_mux);
}

// The wire. A log line prints whether or not the call was accepted; a frame either went out
// or it did not, so this is the only thing on the page that is evidence.
constexpr int kFrameRing = 64;
struct FrameRec { uint16_t id; uint8_t len, dir, d[8]; uint16_t count; uint32_t ms; };
FrameRec g_fr[kFrameRing];
uint16_t g_frHead = 0;
bool     g_tapAll = false;


// ---------------------------------------------------------------------------
// OVERRIDE — the payload, not the frames
// ---------------------------------------------------------------------------
// The frame ring shows what left the controller; this shows what the BUILDER produced,
// which is the thing worth editing. TX frames are reassembled here rather than by a
// library — ours to build, ours to concatenate, and fifteen exact lines.
constexpr uint16_t kOvrMax = 320;   // one nav screen is 304; nothing this console sends is bigger
uint8_t  g_lastTx[kOvrMax];
uint16_t g_lastTxLen = 0;
uint16_t g_lastTxId  = 0;
volatile bool  g_rawBusy = false;      // an override past 113 bytes is BORROWED until done
affa::TxTicket g_rawTicket = affa::kNoTicket;
uint8_t  g_asm[kOvrMax];       // in progress
uint16_t g_asmLen = 0, g_asmId = 0, g_asmWant = 0;


// One log line per payload, hex, ready to paste back into op=raw or next to docs/WIRE.md.
void logHex(const char* what, uint16_t id, const uint8_t* d, uint16_t n) {
  char line[3 * kOvrMax + 32];
  int k = snprintf(line, sizeof(line), "%s %03X ", what, id);
  for (uint16_t i = 0; i < n && k < static_cast<int>(sizeof(line)) - 4; ++i)
    k += snprintf(line + k, sizeof(line) - k, "%02X ", d[i]);
  logmsg("%s", line);
}

void captureTx(const affa::Frame& f) {
  if (!f.len) return;
  const uint8_t pci = f.data[0] & 0xF0;
  if (pci == 0x00) {                                   // single frame: len in the low nibble
    const uint8_t n = static_cast<uint8_t>((f.data[0] & 0x0F) + 1);
    g_lastTxId = static_cast<uint16_t>(f.id);
    g_lastTxLen = n > f.len ? f.len : n;
    memcpy(g_lastTx, f.data, g_lastTxLen);
    g_asmLen = 0;
    return;
  }
  if (pci == 0x10) {                                   // first frame: 12-bit declared length
    g_asmId  = static_cast<uint16_t>(f.id);
    g_asmWant = static_cast<uint16_t>(((f.data[0] & 0x0F) << 8) | f.data[1]) + 2;
    g_asmLen = f.len;
    memcpy(g_asm, f.data, f.len);
    return;
  }
  if (pci == 0x20 && g_asmLen && static_cast<uint16_t>(f.id) == g_asmId) {
    const uint8_t n = static_cast<uint8_t>(f.len - 1);
    if (g_asmLen + n <= kOvrMax) { memcpy(g_asm + g_asmLen, f.data + 1, n); g_asmLen += n; }
    if (g_asmLen >= g_asmWant) {                       // complete
      g_lastTxId = g_asmId;
      g_lastTxLen = g_asmWant < g_asmLen ? g_asmWant : g_asmLen;
      memcpy(g_lastTx, g_asm, g_lastTxLen);
      g_asmLen = 0;
    }
  }
}

void onTap(const affa::Frame& f, affa::Direction dir, void*) {
  const uint16_t id = static_cast<uint16_t>(f.id);
  const uint8_t  d  = (dir == affa::Direction::Rx) ? 0 : 1;

  // REASSEMBLE FIRST, FILTER SECOND. The filter below exists to keep the display ring
  // readable, not to hide bytes: a nav screen is 44 frames, and dropping its continuations
  // before captureTx() means op=lasttx can never show the one payload most worth editing.
  if (d) captureTx(f);

  // Nav CONTINUATIONS are dropped from the RING unless asked for: 1 + 43 CFs + 43 flow
  // controls at 4 fps flushes 64 slots ten times a second, and a tap that records everything
  // ends up showing nothing. The FIRST frame is always kept; that is the header.
  if (!g_tapAll) {
    if (id == affa::carminat::kIdNav && f.len && (f.data[0] & 0xF0) == 0x20) return;
    if (id == (affa::carminat::kIdNav | 0x400)) return;
  }
  portENTER_CRITICAL(&g_mux);
  FrameRec& last = g_fr[(g_frHead + kFrameRing - 1) % kFrameRing];
  if (last.count && last.dir == d && last.id == id && last.len == f.len &&
      memcmp(last.d, f.data, 8) == 0) {
    ++last.count;
  } else {
    FrameRec& r = g_fr[g_frHead];
    r.id = id; r.len = f.len; r.dir = d;
    memcpy(r.d, f.data, 8); r.count = 1; r.ms = ::millis();
    g_frHead = (g_frHead + 1) % kFrameRing;
  }
  portEXIT_CRITICAL(&g_mux);
}

// Keys. The whole point of the key tab: press one on the stalk and see WHICH the library
// decoded, so a mapping can be confirmed rather than assumed.
constexpr int kKeyRing = 16;
struct KeyRec { uint16_t code; uint8_t edge; uint32_t ms; };
KeyRec   g_keys[kKeyRing];
uint16_t g_keyHead = 0;
uint32_t g_keyCount = 0;

void onKey(affa::Key k, affa::KeyEdge e, void*) {
  portENTER_CRITICAL(&g_mux);
  KeyRec& r = g_keys[g_keyHead];
  r.code = static_cast<uint16_t>(k);
  r.edge = static_cast<uint8_t>(e);
  r.ms   = ::millis();
  g_keyHead = (g_keyHead + 1) % kKeyRing;
  ++g_keyCount;
  portEXIT_CRITICAL(&g_mux);
}

// ---------------------------------------------------------------------------
// The opening — the pane will not accept an image without it
// ---------------------------------------------------------------------------
//   52 09 00  display ON. The "display off" traces send 52 00 00 and nothing follows.
//   54 01     unexplained, always sent
//   54 03     closes the full window, so the windowed layout is current

// ---------------------------------------------------------------------------
// SELF-CHECK — the link proved one step at a time, each with its own verdict
// ---------------------------------------------------------------------------
// Absorbed from 01_bringup, which existed for this and carried a whole second console to
// hold it. The opening above LIGHTS THE GLASS; this proves the link works and, when it does
// not, names the step and the reason instead of leaving you with "nothing happened".
//
// Each step waits for its own ticket's completion, so a verdict means the PANEL answered —
// not that a render was accepted into a queue. That distinction is the entire point:
// `Ok` from setText() means "queued", and the four counters below have all been zero on a
// board whose glass never moved.
enum class Chk : uint8_t { Idle, Link, Power, Warm, Text, Time, Done, Failed };
constexpr uint8_t  kChkTries  = 3;
constexpr uint32_t kChkAckMs  = 2500;   // one ACK timeout plus slack
constexpr uint32_t kChkWarmMs = 1200;   // the panel is lit but not ready; see below

// EIGHT CHARACTERS, because that is what panelGeometry().mainChars promises and the panel
// truncates without saying so — the first run drew "SELFCHEC" and looked like a bug in the
// builder. The time is the golden vector's own 12:34, so a glass showing anything else is
// a finding rather than a coincidence.
constexpr const char* kChkText = "SELFTEST";
constexpr const char* kChkTime = "1234";

struct SelfCheck {
  Chk      step = Chk::Idle;
  uint8_t  tries = 0;
  uint32_t deadline = 0;
  affa::TxTicket req = affa::kNoTicket;
  affa::Result   last = affa::Result::Ok;
  bool     acked = false;
  uint8_t  verdict[4] = {0, 0, 0, 0};   // link, power, text, time: 0 pending 1 ok 2 fail
  char     why[48] = {0};
} g_chk;

const char* chkName(Chk s) {
  switch (s) {
    case Chk::Idle: return "idle";     case Chk::Link:  return "link";
    case Chk::Power: return "power";   case Chk::Warm:  return "warm";
    case Chk::Text: return "text";     case Chk::Time:  return "time";
    case Chk::Done: return "done";     default:         return "FAILED";
  }
}

void chkStart() {
  g_chk = SelfCheck{};
  g_chk.step = Chk::Link;
  logmsg("self-check: waiting for the link");
}

void chkFail(uint8_t slot, const char* why) {
  snprintf(g_chk.why, sizeof(g_chk.why), "%s: %s", chkName(g_chk.step), why);
  g_chk.verdict[slot] = 2;
  g_chk.step = Chk::Failed;
  logmsg("self-check %s", g_chk.why);
}

// Issue a render and wait on ITS ticket. A refusal is not a failure of the step — the queue
// was full or the task is not running — so it retries rather than condemning the panel.
bool chkIssue(const affa::Submitted& s, uint8_t slot, const char* what) {
  if (!s) {
    if (++g_chk.tries >= kChkTries) { chkFail(slot, "refused every time"); return false; }
    g_chk.deadline = ::millis() + 400;
    logmsg("self-check %s: %s, retry %u", what, affa::resultName(s.result), g_chk.tries);
    return false;
  }
  g_chk.req = s.ticket;
  g_chk.acked = false;
  g_chk.deadline = ::millis() + kChkAckMs;
  return true;
}

void chkPoll() {
  if (g_chk.step == Chk::Idle || g_chk.step == Chk::Done || g_chk.step == Chk::Failed) return;
  const uint32_t now = ::millis();
  const bool     due = static_cast<int32_t>(now - g_chk.deadline) >= 0;

  switch (g_chk.step) {
    case Chk::Link:
      if (g_base->phase() != affa::Phase::Ready) return;   // no deadline: a silent bus is
      g_chk.verdict[0] = 1;                                // normal until the panel speaks
      g_chk.tries = 0; g_chk.deadline = 0; g_chk.step = Chk::Power;
      logmsg("self-check link OK");
      return;

    case Chk::Power:
      if (!g_chk.req) { if (due) chkIssue(g_panel->setPower(true), 1, "power"); return; }
      if (g_chk.acked) {
        g_chk.req = affa::kNoTicket;
        if (g_chk.last != affa::Result::Ok) { chkFail(1, affa::resultName(g_chk.last)); return; }
        g_chk.verdict[1] = 1;
        // THE WAIT THE PANEL NEEDS. It has acknowledged; the glass is not lit yet. Text
        // drawn inside this window goes into a display still coming up, and the usual
        // conclusion is "setText does not work".
        g_chk.deadline = now + kChkWarmMs; g_chk.step = Chk::Warm;
        logmsg("self-check power OK, warming %lu ms", (unsigned long)kChkWarmMs);
      } else if (due) { g_chk.req = affa::kNoTicket; chkFail(1, "no completion"); }
      return;

    case Chk::Warm:
      if (due) { g_chk.tries = 0; g_chk.step = Chk::Text; }
      return;

    case Chk::Text:
      if (!g_chk.req) { if (due) chkIssue(g_panel->setText(kChkText), 2, "text"); return; }
      if (g_chk.acked) {
        g_chk.req = affa::kNoTicket;
        if (g_chk.last != affa::Result::Ok) { chkFail(2, affa::resultName(g_chk.last)); return; }
        g_chk.verdict[2] = 1; g_chk.tries = 0; g_chk.deadline = now;
        g_chk.step = g_panel->supports(affa::Feature::Time) ? Chk::Time : Chk::Done;
        logmsg("self-check text OK");
      } else if (due) { g_chk.req = affa::kNoTicket; chkFail(2, "no completion"); }
      return;

    case Chk::Time:
      if (!g_chk.req) { if (due) chkIssue(g_panel->setTime(kChkTime), 3, "time"); return; }
      if (g_chk.acked) {
        g_chk.req = affa::kNoTicket;
        if (g_chk.last != affa::Result::Ok) { chkFail(3, affa::resultName(g_chk.last)); return; }
        g_chk.verdict[3] = 1; g_chk.step = Chk::Done;
        logmsg("self-check PASSED");
      } else if (due) { g_chk.req = affa::kNoTicket; chkFail(3, "no completion"); }
      return;

    default: return;
  }
}

struct Opening { bool done = false; uint8_t step = 0; uint32_t nextMs = 0; } g_open;

void openingPoll() {
  if (g_open.done || !g_carminat || g_base->phase() != affa::Phase::Ready) return;
  if (static_cast<int32_t>(::millis() - g_open.nextMs) < 0) return;
  switch (g_open.step) {
    case 0: logmsg("opening 1/3 display ON -> %d", static_cast<int>(g_panel->setPower(true).result));
            break;
    // THE ONE PLACE THIS CONSOLE STILL WRITES RAW BYTES, and it is here because no builder
    // exists for `54 01`: the OEM always sends it during the opening and nothing decodes
    // what it means, so the library has nothing honest to call it. Everything else on this
    // page goes through a library method.
    case 1: { const uint8_t p[3] = {0x02, affa::carminat::kCmdClose, 0x01};
              (void)g_base->enqueue(affa::carminat::kIdSetText, p, sizeof(p));
              logmsg("opening 2/3  54 01 (raw: no builder)"); break; }
    // `54 03` DOES have a builder — it is hidePopup(), the panel's only close command.
    case 2: logmsg("opening 3/3  54 03 -> %d", static_cast<int>(g_panel->hidePopup().result));
            break;
    // The opening ends with a close-window, so the glass is empty and the main line is ours
    // again — but lastRendered() cannot say that (see takeMainLine). Assert it.
    default: g_open.done = true; g_forceFrame = true;
             logmsg("opening complete -> main line %d", static_cast<int>(takeMainLine().result));
             return;
  }
  ++g_open.step;
  g_open.nextMs = ::millis() + 250;
}

// ---------------------------------------------------------------------------
// The pane
// ---------------------------------------------------------------------------
void renderScene(uint8_t* b) {
  switch (g_scene) {
    case Scene::Spectrum: media::drawMedia(b, g_bars, g_uptimeS, 213, 3, 12, true); break;
    case Scene::Vu:       media::drawVu(b, g_vu);               break;
    case Scene::Wave:     media::drawWave(b, g_wave);           break;
    case Scene::Clock:    media::drawClockFace(b, g_uptimeS);   break;
    case Scene::Stars:    media::drawStars(b, g_stars);         break;
    case Scene::Bounce:   media::drawBounce(b, g_bounce);       break;
    case Scene::Rings:    media::drawRings(b, g_sceneFrame);    break;
    case Scene::Eyes:     media::drawEyes(b, g_eyes);           break;
    case Scene::Custom:   memcpy(b, g_custom, media::kBytes);   break;
    case Scene::Blank:    media::clear(b);                      break;
    case Scene::Globe:       memcpy(b, navlab::kBmpGlobe,       media::kBytes); break;
    case Scene::Tryzub:      memcpy(b, navlab::kBmpTryzub,      media::kBytes); break;
    case Scene::TryzubClock: memcpy(b, navlab::kBmpTryzubClock, media::kBytes); break;
    case Scene::Renault:     memcpy(b, navlab::kBmpRenault,     media::kBytes); break;
    case Scene::Dash:        memcpy(b, navlab::kBmpDash,        media::kBytes); break;
    case Scene::Gauges:      memcpy(b, navlab::kBmpGauges,      media::kBytes); break;
    case Scene::Combo:       memcpy(b, navlab::kBmpCombo,       media::kBytes); break;
    case Scene::FontSheet:   memcpy(b, navlab::kBmpFontSheet,   media::kBytes); break;
    case Scene::Checker:     memcpy(b, navlab::kBmpChecker,     media::kBytes); break;
    case Scene::EyesStill:   memcpy(b, navlab::kBmpEyes,        media::kBytes); break;
    case Scene::EyesLeft:    memcpy(b, navlab::kBmpEyesLeft,    media::kBytes); break;
    case Scene::EyesBlink:   memcpy(b, navlab::kBmpEyesBlink,   media::kBytes); break;
    case Scene::EyesHappy:   memcpy(b, navlab::kBmpEyesHappy,   media::kBytes); break;
    default: media::clear(b); break;
  }
}

void stepScene() {
  ++g_sceneFrame;
  switch (g_scene) {
    case Scene::Spectrum: g_bars.step(true);  break;
    case Scene::Vu:       g_vu.step(true);    break;
    case Scene::Wave:     g_wave.step(true);  break;
    case Scene::Stars:    g_stars.step();     break;
    case Scene::Bounce:   g_bounce.step();    break;
    case Scene::Eyes:     g_eyes.step();      break;
    default: break;
  }
}

void pushFrame() {
  if (!g_carminat || !g_paneOn) return;
  if (g_navBusy) { ++g_drops; return; }          // skip, never queue: a backlog never drains
  uint8_t* const buf = g_frame[g_drawInto];
  renderScene(buf);
  // An identical image is 44 CAN frames spent redrawing pixels that have not moved.
  if (!g_forceFrame && g_everSent &&
      memcmp(buf, g_frame[g_drawInto ^ 1], media::kBytes) == 0) return;
  const affa::Submitted nav = g_carminat->showNavBitmap(buf);
  if (!nav) { ++g_fail; return; }
  g_navTicket = nav.ticket;
  g_navBusy = true; g_forceFrame = false; g_everSent = true;
  g_frameStart = ::millis();
  g_drawInto ^= 1;
  ++g_frames;
}

void onDone(affa::TxTicket t, affa::Result r, void*) {
  if (t == g_menuTicket) { g_menuBusy = false; g_menuTicket = affa::kNoTicket; }
  if (t == g_rawTicket)  { g_rawBusy  = false; g_rawTicket  = affa::kNoTicket; }
  if (t == g_chk.req && g_chk.req != affa::kNoTicket) { g_chk.acked = true; g_chk.last = r; }
  if (t != g_navTicket) return;
  g_navBusy = false;
  if (r == affa::Result::Ok) { ++g_ok; g_txMs += ::millis() - g_frameStart; }
  else { ++g_fail; logmsg("frame FAILED (%d)", static_cast<int>(r)); }
}

void onSyncChanged(affa::SyncState s, void*) {
  logmsg("sync 0x%02X%s", static_cast<unsigned>(s),
         affa::hasFlag(s, affa::SyncState::Failed) ? " FAILED" : "");
  if (affa::hasFlag(s, affa::SyncState::Failed)) { g_open.done = false; g_open.step = 0; }
}

// The main line is ours only while nothing else is on the glass. lastRendered() is the
// library's own record of the last ACKed screen, so unlike a flag this file keeps it cannot
// drift out of step with what was actually drawn.
// SCROLLING A ROW IMPLIES REPAINTING IT. The console used to have two switches for one
// intention: a per-row "scroll" checkbox that only moved a window in RAM, and a separate
// "keep repainting" checkbox that was the only thing that actually put the moved window on
// the glass. Ticking scroll alone did nothing visible, which reads as a broken feature
// rather than as a second switch left off.
inline bool rowsShouldTick() {
  if (!g_carminat) return false;
  if (g_rowsLive) return true;
  for (const auto& r : g_row)
    if (r.on) return true;
  return false;
}

// TAKE THE LINE BACK, NOW, whatever lastRendered() currently says. Two callers: an explicit
// SET TEXT, and the end of the opening.
//
// The opening needs it because its last step is `02 54 03` — the close-window command,
// which the library spells hidePopup() — and that leaves lastRendered() reading Popup. The
// glass is in fact EMPTY at that point, but nothing in the record says so: a hide and a show
// are both "the last thing I did was a popup". Without this the scrolling line would sit
// silent from boot, waiting for a screen to clear that had already cleared.
affa::Submitted takeMainLine() {
  char w[kMainWidth + 1];
  g_main.window(w, kMainWidth);
  (void)g_main.changed(w);          // adopt it: the loop must not repaint identical bytes
  g_main.nextMs = ::millis() + g_main.periodMs;
  g_holdUntil   = 0;
  return g_carminat ? g_carminat->setTextStyled(w, g_icon, g_src, g_fmt, g_fmt2)
                    : g_panel->setText(w);
}

inline bool mainLineIsOurs() {
  const affa::RenderSlot s = g_base->lastRendered();
  return s == affa::RenderSlot::None || s == affa::RenderSlot::Text ||
         s == affa::RenderSlot::Clock;
}

// THE SAME QUESTION FOR THE INFO ROWS, and the answer is NOT mainLineIsOurs() nor its
// negation. The rows own the glass when nothing a user DELIBERATELY OPENED is on it.
// InfoPopup counts as theirs (their own render must not stop them scrolling); Menu,
// Highlight, Popup, Fullscreen and ConfirmBox do not.
//
// Gated on !mainLineIsOurs() it was backwards both ways and shipped: a menu got repainted
// over every 700 ms, and a plain text line never ticked at all.
inline bool rowsAreOurs() {
  const affa::RenderSlot s = g_base->lastRendered();
  return s == affa::RenderSlot::None || s == affa::RenderSlot::Text ||
         s == affa::RenderSlot::Clock || s == affa::RenderSlot::InfoPopup;
}

// ---------------------------------------------------------------------------
// Settings that survive a flash
// ---------------------------------------------------------------------------
void saveSettings() {
  Preferences p;
  if (!p.begin(kPrefsNamespace, false)) return;
  p.putString("main", g_main.text);
  p.putUChar("mainscroll", g_main.on ? 1 : 0);
  p.putUShort("mainms", static_cast<uint16_t>(g_main.periodMs));
  for (int i = 0; i < 3; ++i) {
    char k[8];
    snprintf(k, sizeof(k), "r%d", i);      p.putString(k, g_row[i].text);
    snprintf(k, sizeof(k), "r%ds", i);     p.putUChar(k, g_row[i].on ? 1 : 0);
  }
  p.putUChar("rowslive", g_rowsLive ? 1 : 0);
  p.putUChar("scene", static_cast<uint8_t>(g_scene));
  p.putUShort("period", static_cast<uint16_t>(g_periodMs));
  p.putUChar("icon", g_icon); p.putUChar("src", g_src); p.putUChar("fmt", g_fmt);
  p.putUChar("fmt2", g_fmt2);
  p.putUChar("paneon", g_paneOn ? 1 : 0);
  p.end();
}

void loadSettings() {
  Preferences p;
  if (!p.begin(kPrefsNamespace, true)) { snprintf(g_main.text, sizeof(g_main.text), "AFFADISPLAY"); return; }
  if (p.isKey("main")) p.getString("main", g_main.text, sizeof(g_main.text));
  else snprintf(g_main.text, sizeof(g_main.text), "AFFADISPLAY");
  g_main.on       = p.getUChar("mainscroll", 0) != 0;
  g_main.periodMs = p.getUShort("mainms", 700);
  static const char* kRowDefault[3] = {"DESTINATION", "TRAFFIC", "SETTINGS"};
  for (int i = 0; i < 3; ++i) {
    char k[8];
    snprintf(k, sizeof(k), "r%d", i);
    if (p.isKey(k)) p.getString(k, g_row[i].text, sizeof(g_row[i].text));
    else snprintf(g_row[i].text, sizeof(g_row[i].text), "%s", kRowDefault[i]);
    snprintf(k, sizeof(k), "r%ds", i);
    g_row[i].on = p.getUChar(k, 0) != 0;
    g_row[i].periodMs = 700;
  }
  g_rowsLive = p.getUChar("rowslive", 0) != 0;
  const uint8_t sc = p.getUChar("scene", static_cast<uint8_t>(Scene::Tryzub));
  if (sc < static_cast<uint8_t>(Scene::kCount)) g_scene = static_cast<Scene>(sc);
  g_periodMs = p.getUShort("period", 250);
  g_icon = p.getUChar("icon", affa::carminat::kIconsNone);
  g_src  = p.getUChar("src",  affa::carminat::kSrcIconNone);
  g_fmt  = p.getUChar("fmt",  affa::carminat::kFormatPlain);
  g_fmt2 = p.getUChar("fmt2", affa::carminat::kIconBank2);
  g_paneOn = p.getUChar("paneon", 1) != 0;
  p.end();
}

// ---------------------------------------------------------------------------
// THE COMMAND DISPATCHER — one place where every action lives
// ---------------------------------------------------------------------------
struct Cmd { bool ok; const char* msg; };
constexpr Cmd kOk{true, "ok"};
Cmd fail(const char* m) { return Cmd{false, m}; }
Cmd fromResult(const affa::Submitted& s) {
  // Takes the Submitted, not a Result: every render in this file returns one, and the
  // handle is right there for a caller that wants to correlate a completion.
  return s ? kOk : fail("panel refused it");
}

const char* sendMenuN(const String& title, const String& csv, uint8_t sel, uint8_t scroll,
                      uint8_t icon, uint8_t thumb) {
  if (!g_carminat) return "Carminat only";
  if (g_menuBusy)  return "busy: the menu buffer is still lent out";
  static char store[400];
  static const char* items[affa::carminat::kMenuMaxItems];
  snprintf(store, sizeof(store), "%s", csv.c_str());
  uint8_t n = 0; char* cur = store;
  while (n < affa::carminat::kMenuMaxItems && cur && *cur) {
    items[n++] = cur;
    char* bar = strchr(cur, '|');
    if (!bar) break;
    *bar = 0; cur = bar + 1;
  }
  if (!n) return "no items";
  const affa::Submitted menu =
      g_carminat->showMenuN(g_menuBuf, sizeof(g_menuBuf), title.c_str(), items, n, 0, sel,
                            scroll, icon, thumb);
  if (!menu) return "refused";
  g_menuTicket = menu.ticket;
  g_menuBusy = true;
  return nullptr;
}

Cmd dispatch(PsychicRequest* r) {
  const auto S = [&](const char* k, const char* d) {
    return r->hasParam(k) ? r->getParam(k)->value() : String(d);
  };
  const auto N = [&](const char* k, long d) {
    return r->hasParam(k) ? strtol(r->getParam(k)->value().c_str(), nullptr, 0) : d;
  };
  const auto B = [&](const char* k, bool d) {
    return r->hasParam(k) ? (r->getParam(k)->value() != "0") : d;
  };
  const String op = S("op", "");

  // ---- power, clock -------------------------------------------------------
  if (op == "power")  return fromResult(g_panel->setPower(B("on", true)));
  if (op == "time") {
    const String t = S("t", "");
    if (t.length() != 4) return fail("HHMM, four digits");
    return fromResult(g_panel->setTime(t.c_str()));
  }

  // ---- the main line ------------------------------------------------------
  //
  // AN EXPLICIT SET TEXT TAKES THE LINE BACK, whatever is on the glass. This op used to
  // leave the send to loop()'s repaint gate, so after a fullscreen or a menu it reported
  // SUCCESS AND SENT NOTHING, for ever. The gate exists to stop a scroll TICK painting over
  // a screen somebody is reading — never to stop a person who pressed the button.
  if (op == "text") {
    if (r->hasParam("t")) { snprintf(g_main.text, sizeof(g_main.text), "%s", S("t","").c_str());
                            g_main.reset(); }
    g_main.on       = B("scroll", g_main.on);
    g_main.periodMs = static_cast<uint32_t>(N("ms", static_cast<long>(g_main.periodMs)));
    if (g_main.periodMs < 150) g_main.periodMs = 150;
    g_icon = static_cast<uint8_t>(N("icon", g_icon));
    g_src  = static_cast<uint8_t>(N("src",  g_src));
    g_fmt  = static_cast<uint8_t>(N("fmt",  g_fmt));
    g_fmt2 = static_cast<uint8_t>(N("fmt2", g_fmt2));
    saveSettings();

    return fromResult(takeMainLine());
  }

  // ---- menus --------------------------------------------------------------
  // `s0`/`s1` are payload bytes [3] and [4] — the list screen's last undecoded field. The
  // defaults are the pair the library has always sent, so leaving them alone changes no
  // byte; the other two pairs are what the OEM's Settings and Navigation menus carry.
  if (op == "menu") {
    if (!g_carminat) return fromResult(g_panel->showMenu(S("h","MENU").c_str(),
                                                         S("a","ROW ONE").c_str(),
                                                         S("b","ROW TWO").c_str(),
                                                         static_cast<uint8_t>(N("scroll",0))));
    return fromResult(g_carminat->showMenuIcon(
        S("h","MENU").c_str(), S("a","ROW ONE").c_str(), S("b","ROW TWO").c_str(),
        static_cast<uint8_t>(N("scroll",0)),
        static_cast<uint8_t>(N("i0", affa::carminat::kMenuIconOemBlank)),
        static_cast<uint8_t>(N("i1", affa::carminat::kMenuThumbNone))));
  }
  if (op == "hilite") return fromResult(g_panel->highlightItem(static_cast<uint8_t>(N("n",0))));
  if (op == "menun") {
    const char* e = sendMenuN(S("h","NAVIGATION"),
                              S("i","DESTINATION|ROUTE|MAP|TRAFFIC|SETTINGS|BACK"),
                              static_cast<uint8_t>(N("n",0)),
                              static_cast<uint8_t>(N("scroll", affa::carminat::kScrollBoth)),
                              static_cast<uint8_t>(N("i0", affa::carminat::kMenuIconOemBlank)),
                              static_cast<uint8_t>(N("i1", affa::carminat::kMenuThumbNone)));
    return e ? fail(e) : kOk;
  }
  if (op == "select") {
    if (!g_carminat) return fail("Carminat only");
    return fromResult(g_carminat->selectMenuItem(static_cast<uint8_t>(N("n",0))));
  }
  // The OEM Navigation menu, as HEX into the override editor rather than as a second send
  // path. It is the CONTROL — press it and if the book still draws, a builder change moved,
  // not the panel — and a control is worth more when it goes out through exactly the same
  // enqueue() as everything else.
  if (op == "oemhex") {
    static char msg[3 * kOemNavBytes + 8];
    int k = snprintf(msg, sizeof(msg), "151 ");
    for (uint16_t i = 0; i < kOemNavBytes && k < static_cast<int>(sizeof(msg)) - 4; ++i)
      k += snprintf(msg + k, sizeof(msg) - k, "%02X ", kOemNavMenu[i]);
    return Cmd{true, msg};
  }

  // ---- info menu ----------------------------------------------------------
  if (op == "infomenu") {
    if (!g_carminat) return fail("Carminat only");
    for (int i = 0; i < 3; ++i) {
      char k[8];
      snprintf(k, sizeof(k), "r%d", i);
      if (r->hasParam(k)) { snprintf(g_row[i].text, sizeof(g_row[i].text), "%s",
                                     r->getParam(k)->value().c_str()); g_row[i].reset(); }
      snprintf(k, sizeof(k), "s%d", i);
      if (r->hasParam(k)) g_row[i].on = (r->getParam(k)->value() != "0");
    }
    g_rowsLive = B("live", g_rowsLive);
    saveSettings();
    char w0[kRowWidth+1], w1[kRowWidth+1], w2[kRowWidth+1];
    g_row[0].window(w0, kRowWidth); g_row[1].window(w1, kRowWidth); g_row[2].window(w2, kRowWidth);
    return fromResult(g_carminat->showInfoMenu(w0, w1, w2));
  }
  // No `infopopup` op. showInfoPopup() IS showInfoMenu() with the OEM's default offsets —
  // the same three 0x76 messages, not a second screen — so the console had two buttons for
  // one thing and no way to tell them apart. `infomenu` above is that screen.
  // No `infohide` either: hideInfoPopup() was setText("RENAULT") dressed as a close
  // command, and it has been removed from the library.

  // ---- overlays and boxes -------------------------------------------------
  if (op == "popup") {
    if (!g_carminat) return fromResult(g_panel->showPopupText(S("t","VOL 28").c_str()));
    return fromResult(g_carminat->showPopupText(S("t","VOL 28").c_str(),
                                                static_cast<uint8_t>(N("icon", affa::carminat::kPopupIcon)),
                                                static_cast<uint8_t>(N("src",  affa::carminat::kSrcIconNone)),
                                                static_cast<uint8_t>(N("fmt",  affa::carminat::kFormatPlain))));
  }
  // hidePopup() is the ONLY close command this panel has, and it is the same 02 54 03 the
  // removed hideFullscreenText() sent. There is no `fshide`: a fullscreen is replaced by
  // the next render — SET TEXT on the Text tab is how you close one.
  if (op == "pophide")    return fromResult(g_panel->hidePopup());
  if (op == "fullscreen") return fromResult(g_panel->showFullscreenText(S("a","").c_str(),
                                                                        S("b","").c_str(),
                                                                        S("c","").c_str()));

  // The message box, with the button count as a real parameter: 0, 1 or 2. Two is the OEM's
  // Yes/No form — `21 05 <sel> 00 02 49` plus two 6-byte labels — and it is 119 wire bytes,
  // so it wraps the ISO-TP counter exactly as the OEM's own capture does.
  if (op == "confirm") {
    if (!g_carminat) return fromResult(g_panel->showConfirmBox(S("h","OK").c_str(),
                                                               S("a","DELETE ENTRY?").c_str(),
                                                               S("b","").c_str()));
    const long   nb = N("btn", 1);
    const String l0 = S("l0", "YES"), l1 = S("l1", "NO");
    const char*  labels[2] = {l0.c_str(), l1.c_str()};
    return fromResult(g_carminat->showMessageBox(
        S("a","DELETE ENTRY?").c_str(), S("b","").c_str(),
        nb > 0 ? labels : nullptr, static_cast<uint8_t>(nb),
        static_cast<uint8_t>(N("sel", 0))));
  }
  // `03 29 05 <n>` — a library call now, and three bytes rather than the seven this console
  // used to hand-assemble. The old raw frame was the two-row list's `29 01` shape with the
  // box mode pasted into it, which addresses the wrong screen.
  if (op == "boxsel") {
    if (!g_carminat) return fail("Carminat only");
    return fromResult(g_carminat->selectBoxButton(static_cast<uint8_t>(N("n",0))));
  }

  // ---- the pane -----------------------------------------------------------
  if (op == "scene") {
    const String n = S("n","");
    for (int i = 0; i < static_cast<int>(Scene::kCount); ++i)
      if (n == kSceneName[i]) {
        g_scene = static_cast<Scene>(i);
        g_sceneFrame = 0;
        g_paneOn = true; g_forceFrame = true;   // choosing a scene means showing it
        pushFrame();
        saveSettings();
        return kOk;
      }
    return fail("unknown scene");
  }
  if (op == "pane") {
    g_paneOn = B("on", true);
    if (g_paneOn) { g_forceFrame = true; pushFrame(); }
    saveSettings();
    return kOk;
  }
  if (op == "period") {
    // Floor 120 ms: one image is ~50 ms of round trips, below that every frame lands on a
    // busy transmitter and everything else starves.
    long v = N("ms", 250);
    g_periodMs = static_cast<uint32_t>(v < 120 ? 120 : (v > 5000 ? 5000 : v));
    saveSettings();
    return kOk;
  }
  // What the LAST builder actually produced, as one payload — the thing to copy, edit and
  // send back through op=raw.
  if (op == "lasttx") {
    if (!g_lastTxLen) return fail("nothing sent yet");
    static char msg[3 * kOvrMax + 24];
    int k = snprintf(msg, sizeof(msg), "%03X ", g_lastTxId);
    for (uint16_t i = 0; i < g_lastTxLen && k < static_cast<int>(sizeof(msg)) - 4; ++i)
      k += snprintf(msg + k, sizeof(msg) - k, "%02X ", g_lastTx[i]);
    return Cmd{true, msg};
  }

  if (op == "navtick") {
    if (!g_carminat) return fail("Carminat only");
    return fromResult(g_carminat->navTick(N("n",0) != 0));
  }

  // OVERRIDE — send bytes you typed, on any function id.
  //
  //   op=raw&id=0x151&b=10 0E 77 55 55 FF 60 01 52 45 ...
  //
  // Straight to enqueue(), the choke point every builder uses, so segmentation, retries and
  // the ACK verdict are the ordinary ones — the only thing that differs is who chose the
  // bytes. `id` defaults to the family text id; separators in `b` are ignored.
  if (op == "raw") {
    if (!g_base) return fail("no panel");
    if (g_rawBusy) return fail("busy: the last override is still lent out");
    // STATIC, not a stack buffer: anything past AFFA_MAX_PAYLOAD goes out through
    // enqueueExternal(), which BORROWS the pointer until the ticket completes.
    static uint8_t buf[kOvrMax];
    uint16_t n = 0;
    // BODY FIRST, QUERY SECOND. 200 bytes of hex is 600 characters and esp_http_server
    // caps a URI at 512 — the OEM control simply would not fit, and the failure is a server
    // error page rather than anything this dispatcher could report. A POST body has no such
    // ceiling, so the console posts and the address bar still works for short payloads.
    const String hex = r->body().length() ? r->body() : S("b", "");
    const char* p = hex.c_str();
    while (*p && n < kOvrMax) {
      while (*p && !isxdigit(static_cast<unsigned char>(*p))) ++p;
      if (!*p) break;
      char* end = nullptr;
      const long v = strtol(p, &end, 16);
      if (end == p) break;
      buf[n++] = static_cast<uint8_t>(v & 0xFF);
      p = end;
    }
    if (!n) return fail("no bytes");
    const uint16_t id = static_cast<uint16_t>(N("id", g_family == Family::Carminat
                                                    ? affa::carminat::kIdSetText
                                                    : affa::updatelist::kIdSetText));
    // Logged BEFORE the verdict, because the verdict is the least interesting part: a
    // refusal still tells you what you asked for.
    logHex("raw", id, buf, n);

    // WHICH ENQUEUE IS A LENGTH QUESTION, and the library is explicit about it: inline slots
    // are AFFA_MAX_PAYLOAD (113) and a nav screen is 304. Past the ceiling the payload is
    // BORROWED, so the buffer must stay put and unchanged until onDone releases it.
    if (n <= AFFA_MAX_PAYLOAD)
      return fromResult(g_base->enqueue(id, buf, static_cast<uint8_t>(n)));
    const affa::Submitted s = g_base->enqueueExternal(id, buf, n);
    if (!s) return fromResult(s);
    g_rawTicket = s.ticket;
    g_rawBusy   = true;
    return kOk;
  }
  // Absorbed from 01_bringup, which is deleted.
  if (op == "selfcheck") { chkStart(); return kOk; }

  // THE THIRTY-SECOND DIAGNOSTIC. Gate our transmitter and watch the counters: if errors keep
  // climbing with nothing of ours on the wire, the fault is not ours. It has settled more
  // arguments on this bench than any other single control.
  if (op == "txgate") { g_link.setTxGate(B("on", true)); return kOk; }

  if (op == "opening") { g_open.done = false; g_open.step = 0; g_open.nextMs = ::millis();
                         return kOk; }

  // ---- housekeeping -------------------------------------------------------
  if (op == "tap")   { g_tapAll = B("all", false); return kOk; }
  if (op == "hold")  { g_holdMs = static_cast<uint32_t>(N("ms",0)); return kOk; }
  if (op == "keysclear") { portENTER_CRITICAL(&g_mux); g_keyCount = 0;
                           for (auto& k : g_keys) k.ms = 0;
                           portEXIT_CRITICAL(&g_mux); return kOk; }
  if (op == "family") {
    Preferences p;
    if (!p.begin(kPrefsNamespace, false)) return fail("nvs");
    p.putUChar("family", S("f","carminat") == "updatelist" ? 1 : 0);
    p.end();
    logmsg("family stored, rebooting");
    g_wantReboot = true;
    return Cmd{true, "stored - rebooting into that family"};
  }
  // WRITE the STA credentials — without this, moving the board between networks means a
  // reflash. A wrong password is NOT a brick: the join is bounded and the SoftAP comes up
  // behind it with OTA intact. With no `ssid` param it REPORTS, and reports the SSID only.
  if (op == "wifi") {
    if (!r->hasParam("ssid")) {
      String s;
      if (!affanet::storedSsid(s)) return fail("nvs");
      static char msg[80];
      snprintf(msg, sizeof(msg), "stored ssid: %s", s.length() ? s.c_str() : "(none)");
      return Cmd{true, msg};
    }
    if (!affanet::storeWifi(S("ssid", "").c_str(), S("pass", "").c_str())) return fail("nvs");
    g_wantReboot = B("reboot", true);
    return Cmd{true, g_wantReboot ? "stored - rebooting to join it"
                                  : "stored - takes effect on the next boot"};
  }
  if (op == "reboot") { g_wantReboot = true; return Cmd{true, "rebooting"}; }
  if (op == "panic") {
    // The per-row scroll flags too, and they are the ones that mattered: `rowsShouldTick()`
    // is true if ANY row has one, they persist in NVS, and panic used to clear only the
    // `live` switch — so the thing you pressed to stop everything left the rows repainting.
    for (auto& r : g_row) r.on = false;
    g_main.on = false; g_rowsLive = false; g_paneOn = false;
    snprintf(g_main.text, sizeof(g_main.text), "AFFA");
    g_main.reset();
    (void)g_panel->setText("AFFA");
    saveSettings();
    logmsg("PANIC");
    return Cmd{true, "stopped: scroll off, pane off, static line"};
  }
  return fail("unknown op");
}

#include "web_ui.h"

// ---------------------------------------------------------------------------
esp_err_t cmdReply(PsychicRequest* r) {
    const String op = r->hasParam("op") ? r->getParam("op")->value() : String();
    const Cmd c = dispatch(r);
    logmsg("%s -> %s", op.c_str(), c.ok ? "ok" : c.msg);
    String j("{\"ok\":");
    j += c.ok ? "true" : "false";
    j += ",\"op\":\""; j += op; j += "\"";
    j += ",\"msg\":\""; j += c.msg; j += "\"}";
    PsychicResponse res(r);
    res.setContentType("application/json");
    res.setContent(j.c_str());
    const esp_err_t e = res.send();
    // A FLAG, NOT AN OP-NAME LIST. Whether a command needs a restart is the command's own
    // business — `wifi` can be asked not to — and the list had already been forgotten once.
    if (g_wantReboot) { g_wantReboot = false; delay(250); ESP.restart(); }
    return e;
}

void routes() {
  g_server.on("/", HTTP_GET, [](PsychicRequest* r) {
    PsychicResponse res(r);
    res.setContentType("text/html; charset=utf-8");
    res.setContent(kIndexHtml);
    return res.send();
  });

  // ONE ROUTE. Every action, one result shape, and the console shows the answer for each.
  // GET and POST on the same route: everything works from an address bar, and the one op
  // that can exceed a 512-byte URI (raw) reads its bytes from the body instead.
  g_server.on("/api/cmd", HTTP_GET,  [](PsychicRequest* r) { return cmdReply(r); });
  g_server.on("/api/cmd", HTTP_POST, [](PsychicRequest* r) { return cmdReply(r); });

  // The editor's bitmap: 288 bytes as hex is 576 characters, past what a query string can
  // carry safely, so it is a POST body.
  g_server.on("/api/bmp", HTTP_POST, [](PsychicRequest* r) {
    const String& b = r->body();
    if (b.length() < media::kBytes * 2) return r->reply(400, "text/plain", "need 288 bytes of hex");
    for (uint16_t i = 0; i < media::kBytes; ++i)
      g_custom[i] = static_cast<uint8_t>(strtoul(b.substring(i*2, i*2+2).c_str(), nullptr, 16));
    g_scene = Scene::Custom;
    g_paneOn = true; g_forceFrame = true;
    pushFrame();
    saveSettings();
    return r->reply(200, "text/plain", "sent");
  });

  g_server.on("/api/img", HTTP_GET, [](PsychicRequest* r) {
    const String n = r->hasParam("n") ? r->getParam("n")->value() : String("globe");
    const uint8_t* p = nullptr;
    if      (n == "globe")   p = navlab::kBmpGlobe;
    else if (n == "tryzub")  p = navlab::kBmpTryzub;
    else if (n == "renault") p = navlab::kBmpRenault;
    else if (n == "dash")    p = navlab::kBmpDash;
    else if (n == "gauges")  p = navlab::kBmpGauges;
    else if (n == "combo")   p = navlab::kBmpCombo;
    else if (n == "fontsheet") p = navlab::kBmpFontSheet;
    else if (n == "checker") p = navlab::kBmpChecker;
    else if (n == "tryzubclock") p = navlab::kBmpTryzubClock;
    else if (n == "custom")  p = g_custom;
    if (!p) return r->reply(404, "text/plain", "no such image");
    static char hex[media::kBytes * 2 + 1];
    for (uint16_t i = 0; i < media::kBytes; ++i)
      snprintf(hex + i*2, 3, "%02X", p[i]);
    PsychicResponse res(r);
    res.setContentType("text/plain");
    res.setContent(hex);
    return res.send();
  });

  // ONE PAGE THAT ANSWERS "IS THIS THING WELL, AND IF NOT WHERE". Four groups, and the order
  // is the order you read them in when a panel is dark:
  //
  //   check   the self-check's per-step verdicts — the only ones that mean the PANEL answered
  //   link    the controller: is it live, is it erroring, is the ring overflowing
  //   panel   the protocol: which phase, registered, and who took the session away
  //   task    the library's own poll loop: what blocked it and when
  //
  // Everything here is COUNTED, never sampled, so a 300 ms fault an hour ago is still here.
  g_server.on("/api/health", HTTP_GET, [](PsychicRequest* r) {
    const affa::rtos::Status st = g_task.status();
    const affa::Stats        ls = g_link.stats();
    static const char* kStepName[4] = {"link", "power", "text", "time"};
    String j("{\"check\":{\"state\":\"");
    j += chkName(g_chk.step); j += "\",\"why\":\""; j += g_chk.why; j += "\",\"steps\":[";
    for (int i = 0; i < 4; ++i) {
      if (i) j += ",";
      j += "{\"n\":\""; j += kStepName[i];
      j += "\",\"v\":"; j += String(static_cast<int>(g_chk.verdict[i])); j += "}";
    }
    j += "]},\"link\":{\"live\":";  j += g_link.isLive() ? "true" : "false";
    j += ",\"rx\":";    j += String((unsigned long)ls.rxFrames);
    j += ",\"tx\":";    j += String((unsigned long)ls.txFrames);
    j += ",\"drop\":";  j += String((unsigned long)ls.txDropped);
    j += ",\"ovf\":";   j += String((unsigned long)ls.ringOverflow);
    j += "},\"panel\":{\"phase\":\""; j += affa::phaseName(g_base->phase());
    j += "\",\"registered\":"; j += st.registered ? "true" : "false";
    j += ",\"lost\":";  j += String((unsigned long)st.sessionsLost);
    j += ",\"why\":\"";  j += affa::lossReasonName(g_base->lastLossReason()); j += "\"";
    j += ",\"opened\":"; j += g_open.done ? "true" : "false";
    j += "},\"task\":{\"late\":"; j += String((unsigned long)st.pollLateMaxUs);
    j += ",\"cb\":\"";   j += affa::cbName(st.slowestCb); j += "\"";
    j += ",\"cbms\":";  j += String((unsigned long)st.slowestCbMs);
    j += ",\"cbat\":";  j += String((unsigned long)st.slowestCbAtMs);
    j += ",\"over\":";  j += String((unsigned long)st.cbOverruns);
    j += ",\"foreign\":"; j += String((unsigned long)st.foreignPolls);
    j += ",\"posted\":";  j += String((unsigned int)st.posted);
    j += ",\"dropped\":"; j += String((unsigned long)st.postDropped);
    j += ",\"stack\":";   j += String((unsigned long)st.stackFreeBytes);
    j += "},\"heap\":";   j += String((unsigned long)ESP.getFreeHeap());
    j += ",\"up\":";      j += String((unsigned long)(::millis() / 1000)); j += "}";
    PsychicResponse res(r);
    res.setContentType("application/json");
    res.setContent(j.c_str());
    return res.send();
  });

  g_server.on("/api/state", HTTP_GET, [](PsychicRequest* r) {
    const uint32_t up = ::millis() ? ::millis() : 1;
    String j("{");
    j += "\"family\":\"";  j += (g_family == Family::Carminat) ? "carminat" : "updatelist"; j += "\"";
    j += ",\"phase\":\"";  j += affa::phaseName(g_base->phase()); j += "\"";
    // FOUR COUNTERS THAT SAY WHY IT BROKE, not merely that it did. All counted, never
    // sampled, so a 300 ms fault an hour ago is still visible.
    //   lost/why      the panel taking the session away
    //   posted/dropped renders from other tasks waiting, and ones the ring refused
    //   foreign       poll() called by a task that does not own it — 0 for ever
    //   cbworst/cbms/cbat  WHICH callback blocked the poll task, how long, and WHEN. A
    //                 peak at boot is WiFi associating; a peak that keeps moving is a
    //                 callback that blocks every time. Without cbat they read alike.
    {
      const affa::rtos::Status st = g_task.status();
      j += ",\"lost\":";     j += st.sessionsLost;
      j += ",\"why\":\"";    j += affa::lossReasonName(st.lastLossReason); j += "\"";
      j += ",\"posted\":";   j += st.posted;
      j += ",\"dropped\":";  j += st.postDropped;
      j += ",\"foreign\":";  j += st.foreignPolls;
      j += ",\"cbworst\":\"";j += affa::cbName(st.slowestCb); j += "\"";
      j += ",\"cbms\":";     j += st.slowestCbMs;
      j += ",\"cbat\":";     j += st.slowestCbAtMs;
      j += ",\"cbover\":";   j += st.cbOverruns;
      j += ",\"latemax\":";  j += st.pollLateMaxUs;
      j += ",\"stackfree\":";j += st.stackFreeBytes;
    }
    j += ",\"live\":";     j += g_link.isLive() ? "true" : "false";
    j += ",\"opened\":";   j += g_open.done ? "true" : "false";
    j += ",\"nav\":";      j += g_carminat ? "true" : "false";
    j += ",\"scene\":\"";  j += kSceneName[static_cast<int>(g_scene)]; j += "\"";
    j += ",\"paneon\":";   j += g_paneOn ? "true" : "false";
    j += ",\"period\":";   j += g_periodMs;
    j += ",\"main\":\"";   j += g_main.text; j += "\"";
    j += ",\"scroll\":";   j += g_main.on ? "true" : "false";
    j += ",\"mainms\":";   j += g_main.periodMs;
    j += ",\"icon\":";     j += g_icon;
    j += ",\"src\":";      j += g_src;
    j += ",\"fmt\":";      j += g_fmt;
    j += ",\"fmt2\":";     j += g_fmt2;
    j += ",\"rowslive\":"; j += g_rowsLive ? "true" : "false";
    // WHAT IS ACTUALLY REPAINTING, not what was asked for. `rowslive` read false for the
    // whole session in which the rows were overwriting every menu at 700 ms, because a
    // per-row scroll flag drives the tick too. A status field that cannot answer "is this
    // thing running right now" is how a live repaint loop stays invisible.
    j += ",\"rowstick\":"; j += rowsShouldTick() ? "true" : "false";
    j += ",\"rows\":[";
    for (int i = 0; i < 3; ++i) {
      if (i) j += ",";
      j += "{\"t\":\""; j += g_row[i].text; j += "\",\"s\":";
      j += g_row[i].on ? "true" : "false"; j += "}";
    }
    j += "]";
    j += ",\"owner\":\"";  j += mainLineIsOurs() ? "main" : "screen"; j += "\"";
    j += ",\"slot\":";     j += static_cast<int>(g_base->lastRendered());
    j += ",\"queued\":";   j += g_base->queued();
    j += ",\"frames\":";   j += g_frames;
    j += ",\"ok\":";       j += g_ok;
    j += ",\"fail\":";     j += g_fail;
    j += ",\"drops\":";    j += g_drops;
    j += ",\"duty\":";     j += (g_txMs * 100) / up;
    j += ",\"avgms\":";    j += (g_ok ? g_txMs / g_ok : 0);
    j += ",\"keys\":";     j += g_keyCount;
    j += ",\"heap\":";     j += (uint32_t)ESP.getFreeHeap();
    j += ",\"up\":";       j += up / 1000;
    j += "}";
    PsychicResponse res(r);
    res.setContentType("application/json");
    res.setContent(j.c_str());
    return res.send();
  });

  g_server.on("/api/keys", HTTP_GET, [](PsychicRequest* r) {
    String j("[");
    portENTER_CRITICAL(&g_mux);
    bool first = true;
    for (int i = 0; i < kKeyRing; ++i) {
      const KeyRec& k = g_keys[(g_keyHead + i) % kKeyRing];
      if (!k.ms) continue;
      if (!first) j += ",";
      first = false;
      j += "{\"ms\":"; j += k.ms;
      j += ",\"code\":"; j += k.code;
      j += ",\"name\":\""; j += affa::keyName(static_cast<affa::Key>(k.code));
      j += "\",\"edge\":\""; j += affa::edgeName(static_cast<affa::KeyEdge>(k.edge));
      j += "\"}";
    }
    portEXIT_CRITICAL(&g_mux);
    j += "]";
    PsychicResponse res(r);
    res.setContentType("application/json");
    res.setContent(j.c_str());
    return res.send();
  });

  g_server.on("/api/frames", HTTP_GET, [](PsychicRequest* r) {
    static char out[kFrameRing * 60 + 96];
    size_t at = 0;
    at += snprintf(out + at, sizeof(out) - at, "ms\tdir\tid\tdata\tcount\n");
    portENTER_CRITICAL(&g_mux);
    for (int i = 0; i < kFrameRing; ++i) {
      const FrameRec& f = g_fr[(g_frHead + i) % kFrameRing];
      if (!f.count || at + 70 >= sizeof(out)) continue;
      at += snprintf(out + at, sizeof(out) - at, "%lu\t%s\t%03X\t", (unsigned long)f.ms,
                     f.dir ? "TX" : "RX", f.id);
      for (int k = 0; k < f.len && k < 8; ++k)
        at += snprintf(out + at, sizeof(out) - at, "%02X ", f.d[k]);
      at += snprintf(out + at, sizeof(out) - at, "\t%u\n", f.count);
    }
    portEXIT_CRITICAL(&g_mux);
    PsychicResponse res(r);
    res.setContentType("text/plain");
    res.setContent(out);
    return res.send();
  });

  g_server.on("/api/log", HTTP_GET, [](PsychicRequest* r) {
    static char out[kLogLines * 88 + 64];
    size_t at = 0;
    portENTER_CRITICAL(&g_mux);
    for (int i = 0; i < kLogLines; ++i) {
      const char* l = g_log[(g_logHead + i) % kLogLines];
      if (!l[0]) continue;
      const size_t n = strlen(l);
      if (at + n + 2 >= sizeof(out)) break;
      memcpy(out + at, l, n); at += n; out[at++] = '\n';
    }
    portEXIT_CRITICAL(&g_mux);
    out[at] = 0;
    PsychicResponse res(r);
    res.setContentType("text/plain");
    res.setContent(out);
    return res.send();
  });
}

void startWifi() {
  affanet::startWifi(kApSsid, kApPass, kMdnsName, "media");
}

void startHttp() {
  affanet::startHttp(g_server,
                     []() { g_link.setTxGate(false); logmsg("ota started"); },
                     [](bool ok) { if (!ok) g_link.setTxGate(true); logmsg("ota %s", ok ? "ok" : "FAILED"); },
                     &routes);
}

}  // namespace

void setup() {
  pinMode(kTxPin, OUTPUT);
  digitalWrite(kTxPin, HIGH);        // release the bus before the driver claims the matrix
  Serial.begin(115200);
  delay(300);

  { Preferences p;
    if (p.begin(kPrefsNamespace, true)) { g_family = static_cast<Family>(p.getUChar("family",0)); p.end(); } }
#if AFFA_PANEL_UPDATELIST
  if (g_family == Family::UpdateList) {
    static affa::UpdateListDisplay d(g_link, g_clock);
    g_panel = &d; g_base = &d;
  }
#else
  g_family = Family::Carminat;
#endif
  if (!g_panel) {
    static affa::CarminatDisplay d(g_link, g_clock);
    g_carminat = &d; g_panel = &d; g_base = &d;
    g_family = Family::Carminat;
  }
  loadSettings();
  memcpy(g_custom, navlab::kBmpTryzub, media::kBytes);

  Serial.printf("\n[media] 17_mediascreen — %s\n",
                g_family == Family::Carminat ? "Carminat" : "UpdateList");

  if (!g_link.begin(kRxPin, kTxPin, kBitrate)) Serial.println("[media] CAN begin FAILED");
  g_base->onFrame(&onTap, nullptr);
  g_base->onKey(&onKey, nullptr);
  g_base->onComplete(&onDone, nullptr);
  g_base->onSync(&onSyncChanged, nullptr);
  if (!g_base->begin()) Serial.println("[media] display begin FAILED");

  // THE LIBRARY POLLS ITSELF FROM HERE. Callbacks first, begin(), then start() — that order
  // is the contract, and start() refuses a display that was never begun.
  //
  // WHY THIS CONSOLE IS THE ONE THAT HAD TO CHANGE. Every /api/cmd handler below calls the
  // display from the HTTP server's task while loop() used to call poll() from the Arduino
  // task, and both mutate the transmit queue. It was an unguarded data race that happened
  // not to have bitten yet. Since 2.0 a render from any task builds its bytes on that task's
  // stack and crosses into the poll task as data, so the handlers are correct as written.
  if (!g_task.start(*g_base))
    Serial.println("[media] AffaTask::start() FAILED — nothing will be polled");

  startWifi();
  startHttp();
  logmsg("boot: %s", g_family == Family::Carminat ? "carminat" : "updatelist");
}

void loop() {
  // NO poll() HERE — g_task owns it. loop() may block without costing a timed-out ACK.
  ElegantOTA.loop();
  openingPoll();
  chkPoll();

  const uint32_t now = millis();
  if (static_cast<int32_t>(now - g_nextSecondMs) >= 0) { g_nextSecondMs = now + 1000; ++g_uptimeS; }

  if (static_cast<int32_t>(now - g_nextFrameMs) >= 0) {
    g_nextFrameMs = now + g_periodMs;
    stepScene();
    pushFrame();
  }

  // The main line, only while nothing else is on the glass. A screen the user opened must
  // not be repainted over by a scroll tick — see lastRendered() in the library.
  const bool expired = g_holdMs && g_holdUntil && static_cast<int32_t>(now - g_holdUntil) >= 0;
  if (!mainLineIsOurs() && g_holdMs && !g_holdUntil) g_holdUntil = now + g_holdMs;
  if (mainLineIsOurs()) g_holdUntil = 0;
  if ((mainLineIsOurs() || expired) && static_cast<int32_t>(now - g_main.nextMs) >= 0) {
    g_main.nextMs = now + g_main.periodMs;
    if (expired) { g_main.sent[0] = 0; g_holdUntil = 0; }
    char w[kMainWidth + 1];
    g_main.window(w, kMainWidth);
    if (g_main.changed(w)) {
      if (g_carminat) (void)g_carminat->setTextStyled(w, g_icon, g_src, g_fmt, g_fmt2);
      else            (void)g_panel->setText(w);
    }
  }

  // The info rows, if asked to keep scrolling. Three rows repainting on a timer is a lot of
  // traffic next to one main line, so it is opt-in.
  if (rowsShouldTick() && rowsAreOurs() &&
      static_cast<int32_t>(now - g_row[0].nextMs) >= 0) {
    g_row[0].nextMs = now + g_row[0].periodMs;
    char w0[kRowWidth+1], w1[kRowWidth+1], w2[kRowWidth+1];
    g_row[0].window(w0, kRowWidth); g_row[1].window(w1, kRowWidth); g_row[2].window(w2, kRowWidth);
    (void)g_carminat->showInfoMenu(w0, w1, w2);
  }

  delay(1);      // yield: without it the IDLE task starves and the console stops answering
}
