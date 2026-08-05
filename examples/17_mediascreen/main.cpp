// 17_mediascreen — the presentation. Everything this library can put on a panel, at once.
//
// It began as a media screen and it still is one: a live 48x48 spectrum, clock and progress
// bar in the `0x1F1` nav pane while `setText` marquees the track title on the main line.
// That pairing is the point — it is the first thing in this repo that needs all three bench
// results from 2026-08-05:
//
//   1. The nav pane renders a 48x48 monochrome bitmap.
//   2. It is an INDEPENDENT LAYER. The info menu draws with it; the image can be replaced
//      underneath an open popup and is seen to change. Not a screen mode.
//   3. The OEM animates the channel itself — two nav images 478 ms apart, different pixels.
//
// Everything else here hangs off that: a picker of animated scenes for the pane, every
// render call the library exposes, a marquee that can drive any of them, and the panel
// family selectable at boot. If you want to know what this library does, run this.
//
//   pio run -e ex17_mediascreen -t upload --upload-port COM5
//   then http://<ip>/          console      http://<ip>/update  OTA
//
// ---------------------------------------------------------------------------
// THE BUS BUDGET, WHICH IS THE REAL CONSTRAINT
// ---------------------------------------------------------------------------
// One 48x48 frame is 304 wire bytes = 44 CAN frames, and the panel runs ISO-TP BlockSize 1,
// so every consecutive frame costs a round trip. Measured on this rig: 47-54 ms alone,
// ~64 ms with a title interleaving on 0x151.
//
// That is the ceiling, and it is why the scenes lean on persistence — trails, peak-hold,
// sweeps — rather than on smoothness. At a 250 ms period the pane occupies ~23% of the link
// and leaves room for everything else. At 100 ms it is ~50% and the title queues behind
// images. The floor is 120 ms and the console reports duty and dropped frames so the trade
// is visible rather than guessed at.
//
// THE FRAME BUFFER IS DOUBLE. showNavBitmap() BORROWS the bytes until the ticket completes,
// so drawing the next frame into the buffer the transmitter is still reading would tear the
// image on the wire. Two buffers, and the renderer only ever touches the idle one.

#include <Arduino.h>
#include <AffaDisplay.h>
#include <ESPmDNS.h>
#include <ElegantOTA.h>
#include <Preferences.h>
#include <PsychicHttp.h>
#include <WiFi.h>

#include "media_render.h"
// The generated 48x48 set, shared with 16_navlab rather than duplicated: one generator,
// one header, two consumers. `node tools/gen_navicons.js` rebuilds it.
#include "../16_navlab/nav_images.h"

#if !AFFA_PANEL_CARMINAT
#  error "17_mediascreen needs the Carminat panel: build with -D AFFA_PANEL_CARMINAT=1"
#endif
#if !AFFA_ENABLE_NAV
#  error "17_mediascreen needs the nav pane: build with -D AFFA_ENABLE_NAV=1"
#endif

namespace {

// The standard collin80 stack, same as 09..16. See 16_navlab's banner for why.
constexpr gpio_num_t kRxPin   = GPIO_NUM_5;
constexpr gpio_num_t kTxPin   = GPIO_NUM_4;
constexpr uint32_t   kBitrate = 500000;

constexpr const char* kWifiNamespace = "megaopen";
constexpr const char* kPrefsNamespace = "affamedia";
constexpr const char* kApSsid   = "AffaMedia";
constexpr const char* kApPass   = "affa1234";
constexpr const char* kMdnsName = "affamedia";
constexpr uint32_t    kStaJoinMs = 15000;

struct ArduinoClock final : affa::IClock {
  uint32_t millis() const override { return ::millis(); }
};

affa::CanCommonLink   g_link;
ArduinoClock          g_clock;
PsychicHttpServer     g_server;

// ---------------------------------------------------------------------------
// Panel family — chosen at BOOT, not at runtime
// ---------------------------------------------------------------------------
// A panel is one family or the other: Carminat syncs on 0x3AF, UpdateList on 0x3DF, and the
// handshake, the ids and the text encoding all differ. There is no runtime switch that is
// honest — so the choice is stored in NVS and applied on the next boot, and the console says
// so rather than pretending otherwise.
//
// `g_panel` is the IDisplay surface both families share, and it is what every generic render
// call below goes through. `g_carminat` is non-null ONLY on Carminat and is what the nav
// pane, the N-item menu and the info menu hang off — the features UpdateList does not have.
enum class Family : uint8_t { Carminat = 0, UpdateList = 1 };
Family g_family = Family::Carminat;

affa::CarminatDisplay*  g_carminat = nullptr;
#if AFFA_PANEL_UPDATELIST
affa::UpdateListDisplay* g_updatelist = nullptr;
#endif
affa::IDisplay*         g_panel  = nullptr;
affa::AffaDisplayBase*  g_base   = nullptr;      // phase(), onComplete(), lastEnqueued()

// ---------------------------------------------------------------------------
// Player + scene state
// ---------------------------------------------------------------------------
enum class Scene : uint8_t {
  Spectrum = 0, Vu, Wave, Clock, Stars, Bounce, Rings,
  Globe, Tryzub, TryzubClock, Renault, Dash, Gauges, Combo, FontSheet, Checker, Blank,
  kCount
};
const char* kSceneName[] = {
  "spectrum", "vu", "wave", "clock", "stars", "bounce", "rings",
  "globe", "tryzub", "tryzubclock", "renault", "dash", "gauges", "combo",
  "fontsheet", "checker", "blank"
};
static_assert(sizeof(kSceneName) / sizeof(kSceneName[0]) == static_cast<size_t>(Scene::kCount),
              "scene name table drifted from the enum");

Scene g_scene = Scene::Spectrum;

struct Player {
  char     title[80]  = "NEVER GONNA GIVE YOU UP";
  char     artist[40] = "RICK ASTLEY";
  uint32_t elapsed    = 0;
  uint32_t total      = 213;
  uint8_t  track      = 3;
  uint8_t  trackCount = 12;
  bool     playing    = true;
} g_p;

media::Bars   g_bars;
media::Vu     g_vu;
media::Wave   g_wave;
media::Stars  g_stars;
media::Bounce g_bounce;
uint32_t      g_sceneFrame = 0;

// DOUBLE-BUFFERED ON PURPOSE. showNavBitmap() borrows until the ticket completes.
uint8_t  g_frame[2][media::kBytes];
uint8_t  g_drawInto = 0;
volatile bool  g_navBusy = false;
affa::TxTicket g_navTicket = affa::kNoTicket;

// The N-item menu is built into a buffer WE own and the library borrows, same contract.
uint8_t  g_menuBuf[affa::CarminatDisplay::menuScreenBytes(affa::carminat::kMenuMaxItems)];
volatile bool  g_menuBusy = false;
affa::TxTicket g_menuTicket = affa::kNoTicket;

// ---------------------------------------------------------------------------
// THE OPENING — why the pane stayed blank until this existed
// ---------------------------------------------------------------------------
// 16_navlab only ever put a bitmap on the glass AFTER replaying the OEM's opening, and this
// example shipped without it: it sent text and images into a panel that had never been told
// to show the nav pane, so the title marqueed happily and the pane stayed exactly as it was.
//
// The captured order, and all of it matters:
//
//   52 09 00   display ON. The three "display off" traces send 52 00 00 and NOTHING follows
//              them — no text, no bitmap. This is the gate.
//   54 01      unexplained, always sent
//   54 03      closes the full window. A 0x77 text sent while the full window is up freezes
//              the main screen, so this is what makes the windowed layout current.
//   text       then the radio's own line
//   0x1F1      and only then the image
//
// Runs once on reaching Ready and again on any resync, because a panel that has taken the
// session away has forgotten all of it.
struct Opening {
  bool     done   = false;
  uint8_t  step   = 0;
  uint32_t nextMs = 0;
} g_open;

bool     g_paneOn        = true;
// Send the next frame even if it is identical to the last one. Set by a scene change or a
// resume, because "show me this" must reach the glass whether or not the pixels differ.
bool     g_forceFrame    = true;
bool     g_everSent      = false;
uint32_t g_framePeriodMs = 250;
uint32_t g_nextFrameMs   = 0;
uint32_t g_nextSecondMs  = 0;

// ---------------------------------------------------------------------------
// Marquee — one engine, several targets
// ---------------------------------------------------------------------------
// The panel's text fields are narrow (the main line shows 8 characters; an info-menu row
// shows 8) and real metadata is not. Rather than truncating, every text target here can
// scroll, on its own timer, from one shared windowing function.
//
// TWO SPACES OF GAP at the wrap, deliberately: without them the loop reads as a jump cut
// rather than as a rotation.
struct Marquee {
  bool     on       = true;
  uint16_t at       = 0;
  uint32_t periodMs = 700;
  uint32_t nextMs   = 0;

  void window(const char* src, char* out, uint8_t width) {
    const size_t len = strlen(src);
    if (!on || len <= width) {
      snprintf(out, width + 1u, "%-*s", width, src);
      return;
    }
    const size_t span = len + 2;
    for (uint8_t i = 0; i < width; ++i) {
      const size_t k = (at + i) % span;
      out[i] = (k < len) ? src[k] : ' ';
    }
    out[width] = 0;
    at = static_cast<uint16_t>((at + 1) % span);
  }
};
Marquee g_mqTitle;                 // drives setText on the main line
Marquee g_mqRows;                  // drives the info-menu rows

constexpr uint8_t kMainWidth = 8;  // 0x77 plain-ASCII field
constexpr uint8_t kRowWidth  = 8;  // measured with the ruler on the bench

char g_row[3][40] = { "DESTINATION MEMORY", "TRAFFIC INFORMATION", "SYSTEM SETTINGS" };
bool g_rowsLive = false;           // keep repainting the info menu with scrolling rows

// Counters.
volatile uint32_t g_frames = 0, g_framesOk = 0, g_framesFail = 0, g_busyDrops = 0;
volatile uint32_t g_txMsAccum = 0;
uint32_t g_frameStartMs = 0;

// ---------------------------------------------------------------------------
// Log ring
// ---------------------------------------------------------------------------
portMUX_TYPE g_logMux = portMUX_INITIALIZER_UNLOCKED;
constexpr int kLogLines = 40;
char     g_log[kLogLines][96];
uint16_t g_logHead = 0;

void logmsg(const char* fmt, ...) {
  char line[96];
  va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof(line), fmt, ap); va_end(ap);
  portENTER_CRITICAL(&g_logMux);
  snprintf(g_log[g_logHead], sizeof(g_log[0]), "%8lu %s", (unsigned long)::millis(), line);
  g_logHead = (g_logHead + 1) % kLogLines;
  portEXIT_CRITICAL(&g_logMux);
}

// ---------------------------------------------------------------------------
// The pane
// ---------------------------------------------------------------------------
void renderScene(uint8_t* b) {
  switch (g_scene) {
    case Scene::Spectrum:
      media::drawMedia(b, g_bars, g_p.elapsed, g_p.total, g_p.track, g_p.trackCount,
                       g_p.playing);
      break;
    case Scene::Vu:     media::drawVu(b, g_vu);                  break;
    case Scene::Wave:   media::drawWave(b, g_wave);              break;
    case Scene::Clock:  media::drawClockFace(b, g_p.elapsed);    break;
    case Scene::Stars:  media::drawStars(b, g_stars);            break;
    case Scene::Bounce: media::drawBounce(b, g_bounce);          break;
    case Scene::Rings:  media::drawRings(b, g_sceneFrame);       break;
    // The generated set. Static, and that is the contrast worth showing: the same channel
    // carries a baked logo and a live instrument equally well.
    case Scene::Globe:       memcpy(b, navlab::kBmpGlobe,       media::kBytes); break;
    case Scene::Tryzub:      memcpy(b, navlab::kBmpTryzub,      media::kBytes); break;
    case Scene::TryzubClock: memcpy(b, navlab::kBmpTryzubClock, media::kBytes); break;
    case Scene::Renault:     memcpy(b, navlab::kBmpRenault,     media::kBytes); break;
    case Scene::Dash:        memcpy(b, navlab::kBmpDash,        media::kBytes); break;
    case Scene::Gauges:      memcpy(b, navlab::kBmpGauges,      media::kBytes); break;
    case Scene::Combo:       memcpy(b, navlab::kBmpCombo,       media::kBytes); break;
    case Scene::FontSheet:   memcpy(b, navlab::kBmpFontSheet,   media::kBytes); break;
    case Scene::Checker:     memcpy(b, navlab::kBmpChecker,     media::kBytes); break;
    // THE ONLY WAY WE HAVE FOUND TO EMPTY THE PANE. No command to erase it is known, and
    // simply stopping the frame pump leaves the last image on the glass — so "clear" is
    // 288 zero bytes sent like any other image.
    case Scene::Blank:       media::clear(b);                                  break;
    default: media::clear(b); break;
  }
}

void stepScene() {
  ++g_sceneFrame;
  switch (g_scene) {
    case Scene::Spectrum: g_bars.step(g_p.playing);   break;
    case Scene::Vu:       g_vu.step(g_p.playing);     break;
    case Scene::Wave:     g_wave.step(g_p.playing);   break;
    case Scene::Stars:    g_stars.step();             break;
    case Scene::Bounce:   g_bounce.step();            break;
    default: break;
  }
}

void pushFrame() {
  if (!g_carminat || !g_paneOn) return;
  if (g_navBusy) { ++g_busyDrops; return; }   // still in flight — SKIP, never queue up

  uint8_t* const buf = g_frame[g_drawInto];
  renderScene(buf);

  // DO NOT RESEND AN IDENTICAL IMAGE. A frame is 44 CAN frames at BlockSize 1; repainting a
  // static logo four times a second spends a quarter of the link redrawing pixels that have
  // not moved. The other buffer holds exactly what went out last time, so the comparison is
  // free — and it is what turns a still scene into one transfer followed by silence.
  if (!g_forceFrame && g_everSent &&
      memcmp(buf, g_frame[g_drawInto ^ 1], media::kBytes) == 0) return;

  if (g_carminat->showNavBitmap(buf) != affa::Result::Ok) { ++g_framesFail; return; }
  g_forceFrame = false;
  g_everSent   = true;
  g_navTicket    = g_base->lastEnqueued();
  g_navBusy      = true;
  g_frameStartMs = ::millis();
  g_drawInto    ^= 1;
  ++g_frames;
}

// ---------------------------------------------------------------------------
void onDone(affa::TxTicket t, affa::Result r, void*) {
  if (t == g_menuTicket) { g_menuBusy = false; g_menuTicket = affa::kNoTicket; }
  if (t != g_navTicket) return;
  g_navBusy = false;
  if (r == affa::Result::Ok) { ++g_framesOk; g_txMsAccum += ::millis() - g_frameStartMs; }
  else { ++g_framesFail; logmsg("frame FAILED (%d)", static_cast<int>(r)); }
}

void openingPoll() {
  if (g_open.done || !g_carminat) return;
  if (g_base->phase() != affa::Phase::Ready) return;
  if (static_cast<int32_t>(::millis() - g_open.nextMs) < 0) return;

  switch (g_open.step) {
    case 0: (void)g_panel->setPower(true);        logmsg("opening 1/3: display ON"); break;
    case 1: { const uint8_t p[3] = {0x02, 0x54, 0x01};
              (void)g_base->enqueue(affa::carminat::kIdSetText, p, sizeof(p));
              logmsg("opening 2/3: 54 01"); break; }
    case 2: { const uint8_t p[3] = {0x02, 0x54, 0x03};
              (void)g_base->enqueue(affa::carminat::kIdSetText, p, sizeof(p));
              logmsg("opening 3/3: 54 03 (close full window)"); break; }
    default:
      g_open.done  = true;
      g_forceFrame = true;             // and push the first image into a panel now ready for it
      logmsg("opening complete - pane should accept images");
      return;
  }
  ++g_open.step;
  g_open.nextMs = ::millis() + 250;
}

void onSyncChanged(affa::SyncState s, void*) {
  logmsg("sync 0x%02X%s", static_cast<unsigned>(s),
         affa::hasFlag(s, affa::SyncState::Failed) ? " FAILED" : "");
  // A lost session means the panel has forgotten the opening. Replay it.
  if (affa::hasFlag(s, affa::SyncState::Failed)) {
    g_open.done = false; g_open.step = 0; g_open.nextMs = 0;
  }
}

// ---------------------------------------------------------------------------
// Every render call the library exposes, behind one route
// ---------------------------------------------------------------------------
const char* sendMenuN(const String& title, const String& csv, uint8_t first, uint8_t sel) {
  if (!g_carminat) return "Carminat only";
  if (g_menuBusy)  return "busy: the menu buffer is still lent out";
  static char store[512];
  static const char* items[affa::carminat::kMenuMaxItems];
  snprintf(store, sizeof(store), "%s", csv.c_str());
  uint8_t n = 0;
  char* cur = store;
  while (n < affa::carminat::kMenuMaxItems && cur && *cur) {
    items[n++] = cur;
    char* bar = strchr(cur, '|');
    if (!bar) break;
    *bar = 0; cur = bar + 1;
  }
  if (!n) return "no items";
  if (g_carminat->showMenuN(g_menuBuf, sizeof(g_menuBuf), title.c_str(), items, n, first, sel)
      != affa::Result::Ok) return "refused";
  g_menuTicket = g_base->lastEnqueued();
  g_menuBusy   = true;
  return nullptr;
}

affa::Result callApi(const String& w, const String& a, const String& b, const String& c,
                     long n) {
  if (w == "text")       return g_panel->setText(a.c_str());
  if (w == "time")       return g_panel->setTime(a.c_str());
  if (w == "power_on")   return g_panel->setPower(true);
  if (w == "power_off")  return g_panel->setPower(false);
  if (w == "menu")       return g_panel->showMenu(a.c_str(), b.c_str(), c.c_str());
  if (w == "hilite")     return g_panel->highlightItem(static_cast<uint8_t>(n));
  if (w == "popup")      return g_panel->showPopupText(a.c_str());
  if (w == "pophide")    return g_panel->hidePopup();
  if (w == "fullscreen") return g_panel->showFullscreenText(a.c_str(), b.c_str(), c.c_str());
  if (w == "fshide")     return g_panel->hideFullscreenText();
  if (w == "confirm")    return g_panel->showConfirmBox(a.c_str(), b.c_str(), c.c_str());
  if (w == "infopopup")  return g_panel->showInfoPopup(a.c_str(), b.c_str(), c.c_str());
  if (w == "infohide")   return g_panel->hideInfoPopup();
  if (!g_carminat) return affa::Result::NotSupported;
  if (w == "infomenu")   return g_carminat->showInfoMenu(a.c_str(), b.c_str(), c.c_str());
  if (w == "select")     return g_carminat->selectMenuItem(static_cast<uint8_t>(n));
  if (w == "navtick")    return g_carminat->navTick(n != 0);
  return affa::Result::BadArgument;
}

#include "web_ui.h"

void routes() {
  g_server.on("/", HTTP_GET, [](PsychicRequest* r) {
    PsychicResponse res(r);
    res.setContentType("text/html; charset=utf-8");
    res.setContent(kIndexHtml);
    return res.send();
  });

  g_server.on("/api/state", HTTP_GET, [](PsychicRequest* r) {
    const uint32_t up = ::millis() ? ::millis() : 1;
    String j("{");
    j += "\"family\":\"";  j += (g_family == Family::Carminat) ? "carminat" : "updatelist"; j += "\"";
    j += ",\"phase\":\"";  j += affa::phaseName(g_base->phase()); j += "\"";
    j += ",\"live\":";     j += g_link.isLive() ? "true" : "false";
    j += ",\"scene\":\"";  j += kSceneName[static_cast<int>(g_scene)]; j += "\"";
    j += ",\"paneon\":";   j += g_paneOn ? "true" : "false";
    j += ",\"playing\":";  j += g_p.playing ? "true" : "false";
    j += ",\"elapsed\":";  j += g_p.elapsed;
    j += ",\"total\":";    j += g_p.total;
    j += ",\"track\":";    j += g_p.track;
    j += ",\"tracks\":";   j += g_p.trackCount;
    j += ",\"title\":\"";  j += g_p.title;  j += "\"";
    j += ",\"artist\":\""; j += g_p.artist; j += "\"";
    j += ",\"mqtitle\":";  j += g_mqTitle.on ? "true" : "false";
    j += ",\"mqrows\":";   j += g_mqRows.on ? "true" : "false";
    j += ",\"rowslive\":"; j += g_rowsLive ? "true" : "false";
    j += ",\"frames\":";   j += g_frames;
    j += ",\"ok\":";       j += g_framesOk;
    j += ",\"fail\":";     j += g_framesFail;
    j += ",\"drops\":";    j += g_busyDrops;
    j += ",\"periodms\":"; j += g_framePeriodMs;
    j += ",\"duty\":";     j += (g_txMsAccum * 100) / up;
    j += ",\"avgms\":";    j += (g_framesOk ? g_txMsAccum / g_framesOk : 0);
    j += ",\"heap\":";     j += (uint32_t)ESP.getFreeHeap();
    j += ",\"nav\":";      j += g_carminat ? "true" : "false";
    j += ",\"opened\":";   j += g_open.done ? "true" : "false";
    j += "}";
    PsychicResponse res(r);
    res.setContentType("application/json");
    res.setContent(j.c_str());
    return res.send();
  });

  // Every render call, behind one route.
  g_server.on("/api/call", HTTP_GET, [](PsychicRequest* r) {
    const auto s = [&](const char* k, const char* d) {
      return r->hasParam(k) ? r->getParam(k)->value() : String(d);
    };
    const String w = s("w", "");
    const long n = r->hasParam("n") ? strtol(r->getParam("n")->value().c_str(), nullptr, 0) : 0;
    if (w == "menun") {
      const char* err = sendMenuN(s("a", "NAVIGATION"),
                                  s("i", "DESTINATION|ROUTE|MAP|TRAFFIC|SETTINGS|BACK"),
                                  0, static_cast<uint8_t>(n));
      return r->reply(err ? 409 : 200, "text/plain", err ? err : "menu queued");
    }
    const affa::Result res = callApi(w, s("a", "AFFA"), s("b", "ROW ONE"), s("c", "ROW TWO"), n);
    logmsg("%s -> %d", w.c_str(), static_cast<int>(res));
    String m(w);
    m += (res == affa::Result::Ok) ? " ok" : " refused";
    return r->reply(res == affa::Result::Ok ? 200 : 409, "text/plain", m.c_str());
  });

  // Replay the opening on demand — the first thing to try when the pane will not draw.
  g_server.on("/api/opening", HTTP_GET, [](PsychicRequest* r) {
    g_open.done = false; g_open.step = 0; g_open.nextMs = ::millis();
    logmsg("opening: replay requested");
    return r->reply(200, "text/plain", "replaying the OEM opening");
  });

  g_server.on("/api/scene", HTTP_GET, [](PsychicRequest* r) {
    const String n = r->hasParam("n") ? r->getParam("n")->value() : String();
    for (int i = 0; i < static_cast<int>(Scene::kCount); ++i)
      if (n == kSceneName[i]) {
        g_scene      = static_cast<Scene>(i);
        g_sceneFrame = 0;
        // CHOOSING A SCENE MEANS SHOWING IT. Selecting one while the pump was stopped used
        // to change a variable and nothing else — the previous image stayed frozen on the
        // glass with no way to shift it, which is exactly how "blank" appeared not to work.
        g_paneOn     = true;
        g_forceFrame = true;
        pushFrame();                    // now, not up to a frame period from now
        logmsg("scene %s", kSceneName[i]);
        return r->reply(200, "text/plain", kSceneName[i]);
      }
    if (r->hasParam("on")) {
      g_paneOn = r->getParam("on")->value() != "0";
      if (g_paneOn) { g_forceFrame = true; pushFrame(); }
      return r->reply(200, "text/plain", g_paneOn ? "sending" : "stopped (glass keeps the last image)");
    }
    return r->reply(400, "text/plain", "unknown scene");
  });

  g_server.on("/api/player", HTTP_GET, [](PsychicRequest* r) {
    const auto has = [&](const char* k) { return r->hasParam(k); };
    const auto str = [&](const char* k) { return r->getParam(k)->value(); };
    const auto num = [&](const char* k) { return strtoul(str(k).c_str(), nullptr, 10); };
    if (has("title"))  { snprintf(g_p.title, sizeof(g_p.title), "%s", str("title").c_str());
                         g_mqTitle.at = 0; }
    if (has("artist")) snprintf(g_p.artist, sizeof(g_p.artist), "%s", str("artist").c_str());
    if (has("total"))   g_p.total   = num("total");
    if (has("elapsed")) g_p.elapsed = num("elapsed");
    if (has("track"))   g_p.track   = static_cast<uint8_t>(num("track"));
    if (has("tracks"))  g_p.trackCount = static_cast<uint8_t>(num("tracks"));
    if (has("play"))    g_p.playing = (str("play") != "0");
    if (has("mqtitle")) g_mqTitle.on = (str("mqtitle") != "0");
    if (has("mqrows"))  g_mqRows.on  = (str("mqrows")  != "0");
    if (has("rowslive"))g_rowsLive   = (str("rowslive") != "0");
    for (int i = 0; i < 3; ++i) {
      char k[8]; snprintf(k, sizeof(k), "row%d", i);
      if (has(k)) snprintf(g_row[i], sizeof(g_row[i]), "%s", str(k).c_str());
    }
    if (has("period")) {
      // FLOOR AT 120 ms. One image is ~50 ms of round trips; below that every frame lands
      // on a busy transmitter and the title starves.
      const uint32_t v = num("period");
      g_framePeriodMs = v < 120 ? 120 : (v > 5000 ? 5000 : v);
    }
    if (has("titlems")) g_mqTitle.periodMs = num("titlems") < 150 ? 150 : num("titlems");
    if (has("rowms"))   g_mqRows.periodMs  = num("rowms")   < 250 ? 250 : num("rowms");
    return r->reply(200, "text/plain", "ok");
  });

  // The family is a BOOT choice, not a runtime one: the two panels do not share a handshake.
  g_server.on("/api/family", HTTP_GET, [](PsychicRequest* r) {
    if (!r->hasParam("f")) return r->reply(400, "text/plain", "f=carminat|updatelist");
    const String f = r->getParam("f")->value();
    Preferences p;
    if (!p.begin(kPrefsNamespace, false)) return r->reply(500, "text/plain", "nvs");
    p.putUChar("family", f == "updatelist" ? 1 : 0);
    p.end();
    r->reply(200, "text/plain", "stored - rebooting into that family");
    delay(250);
    ESP.restart();
    return ESP_OK;
  });

  g_server.on("/api/log", HTTP_GET, [](PsychicRequest* r) {
    static char out[kLogLines * 96 + 64];
    size_t at = 0;
    portENTER_CRITICAL(&g_logMux);
    for (int i = 0; i < kLogLines; ++i) {
      const char* l = g_log[(g_logHead + i) % kLogLines];
      if (!l[0]) continue;
      const size_t n = strlen(l);
      if (at + n + 2 >= sizeof(out)) break;
      memcpy(out + at, l, n); at += n; out[at++] = '\n';
    }
    portEXIT_CRITICAL(&g_logMux);
    out[at] = 0;
    PsychicResponse res(r);
    res.setContentType("text/plain");
    res.setContent(out);
    return res.send();
  });

  g_server.on("/api/reboot", HTTP_GET, [](PsychicRequest* r) {
    r->reply(200, "text/plain", "rebooting");
    delay(200); ESP.restart(); return ESP_OK;
  });
}

void startWifi() {
  Preferences p;
  String ssid, pass;
  if (p.begin(kWifiNamespace, true)) { ssid = p.getString("ssid", ""); pass = p.getString("pass", ""); p.end(); }
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
  Serial.printf("\n[media] %s ip=%s  http://%s/  OTA http://%s/update\n",
                sta ? "STA" : "AP", ip.c_str(), ip.c_str(), ip.c_str());
}

void startHttp() {
  g_server.config.lru_purge_enable  = true;
  g_server.config.max_open_sockets  = 7;
  g_server.config.recv_wait_timeout = 3;
  g_server.config.send_wait_timeout = 3;
  g_server.config.max_uri_handlers  = 64;
  g_server.config.stack_size        = 8192;
  g_server.listen(80);
  // OTA FIRST — the only way back into a board with no cable.
  ElegantOTA.onStart([]() { g_link.setTxGate(false); logmsg("ota started"); });
  ElegantOTA.onEnd([](bool ok) { if (!ok) g_link.setTxGate(true); logmsg("ota %s", ok ? "ok" : "FAILED"); });
  ElegantOTA.begin(&g_server);
  routes();
}

}  // namespace

void setup() {
  pinMode(kTxPin, OUTPUT);
  digitalWrite(kTxPin, HIGH);      // release the bus before the driver claims the matrix

  Serial.begin(115200);
  delay(300);

  {
    Preferences p;
    if (p.begin(kPrefsNamespace, true)) {
      g_family = static_cast<Family>(p.getUChar("family", 0));
      p.end();
    }
  }
#if AFFA_PANEL_UPDATELIST
  if (g_family == Family::UpdateList) {
    static affa::UpdateListDisplay d(g_link, g_clock);
    g_updatelist = &d; g_panel = &d; g_base = &d;
  }
#else
  g_family = Family::Carminat;     // not compiled in; do not pretend
#endif
  if (!g_panel) {
    static affa::CarminatDisplay d(g_link, g_clock);
    g_carminat = &d; g_panel = &d; g_base = &d;
    g_family = Family::Carminat;
  }
  Serial.printf("\n[media] 17_mediascreen — %s\n",
                g_family == Family::Carminat ? "Carminat" : "UpdateList");

  if (!g_link.begin(kRxPin, kTxPin, kBitrate)) Serial.println("[media] CAN begin FAILED");
  g_base->onComplete(&onDone, nullptr);
  g_base->onSync(&onSyncChanged, nullptr);
  if (!g_base->begin()) Serial.println("[media] display begin FAILED");

  startWifi();
  startHttp();
  logmsg("boot: %s, %u-byte frames, %lu ms",
         g_family == Family::Carminat ? "carminat" : "updatelist",
         media::kBytes, (unsigned long)g_framePeriodMs);
}

void loop() {
  g_base->poll();
  ElegantOTA.loop();
  openingPoll();          // BEFORE anything is drawn; the pane will not accept images without it

  const uint32_t now = millis();

  // The clock, once a second, independent of the frame rate.
  if (static_cast<int32_t>(now - g_nextSecondMs) >= 0) {
    g_nextSecondMs = now + 1000;
    if (g_p.playing && g_p.elapsed < g_p.total) ++g_p.elapsed;
    else if (g_p.playing && g_p.total) {
      g_p.elapsed = 0;
      g_p.track = static_cast<uint8_t>((g_p.track % g_p.trackCount) + 1);
    }
  }

  // The pane.
  if (static_cast<int32_t>(now - g_nextFrameMs) >= 0) {
    g_nextFrameMs = now + g_framePeriodMs;
    stepScene();
    pushFrame();
  }

  // The main line. Its own timer, deliberately: the title must not stutter because the pane
  // is busy, and it goes out on 0x151 while the image goes out on 0x1F1, so the two never
  // contend for the same function slot.
  if (static_cast<int32_t>(now - g_mqTitle.nextMs) >= 0) {
    g_mqTitle.nextMs = now + g_mqTitle.periodMs;
    char win[kMainWidth + 1];
    g_mqTitle.window(g_p.title, win, kMainWidth);
    (void)g_panel->setText(win);
  }

  // The info-menu rows, scrolling in place. Off by default — three rows repainting on a
  // timer is a lot of traffic next to one main line, and it is here to be demonstrated
  // rather than to be left running.
  if (g_rowsLive && g_carminat &&
      static_cast<int32_t>(now - g_mqRows.nextMs) >= 0) {
    g_mqRows.nextMs = now + g_mqRows.periodMs;
    char w0[kRowWidth + 1], w1[kRowWidth + 1], w2[kRowWidth + 1];
    const uint16_t at = g_mqRows.at;               // one phase for all three rows
    g_mqRows.at = at; g_mqRows.window(g_row[0], w0, kRowWidth);
    g_mqRows.at = at; g_mqRows.window(g_row[1], w1, kRowWidth);
    g_mqRows.at = at; g_mqRows.window(g_row[2], w2, kRowWidth);
    (void)g_carminat->showInfoMenu(w0, w1, w2);
  }

  delay(1);      // yield: without it the IDLE task starves and the console stops answering
}
