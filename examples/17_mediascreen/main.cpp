// 17_mediascreen — a media screen that uses BOTH layers at once.
//
// This is what 16_navlab's findings were for. Three things were established on the bench on
// 2026-08-05, and this example is the first thing that could not be built without all three:
//
//   1. The 0x1F1 nav pane renders a 48x48 monochrome bitmap.
//   2. It is an INDEPENDENT LAYER. The info menu draws with it; the image can be replaced
//      underneath an open popup and is seen to change. It is not a screen mode.
//   3. The OEM itself sends two nav images 478 ms apart with different pixels, so animating
//      the channel is something the factory radio does, not something we invented.
//
// So: the pane carries a live spectrum, a clock and a progress bar, while `setText` carries
// the track title on the main line — at the same time, on one screen. The pane is redrawn
// on the device rather than replayed from flash, because a clock that advances and a bar
// that fills are not knowable at build time.
//
//   pio run -e ex17_mediascreen -t upload --upload-port COM5
//   then http://<ip>/          console      http://<ip>/update  OTA
//
// ---------------------------------------------------------------------------
// THE BUS BUDGET, WHICH IS THE REAL CONSTRAINT
// ---------------------------------------------------------------------------
// One 48x48 frame is 304 wire bytes = 44 CAN frames, and the panel runs ISO-TP BlockSize 1,
// so every consecutive frame costs a round trip. Measured on this rig: 47-54 ms per image.
//
// That is the ceiling. At a 250 ms frame period the pane alone occupies ~20% of the link and
// leaves room for the title, the clock and whatever else the application wants. At 100 ms it
// is ~50% and the title updates start queueing behind images. kFramePeriodMs defaults to 250
// for that reason and the console reports the duty cycle so the trade is visible rather than
// guessed at.
//
// THE FRAME BUFFER IS DOUBLE. showNavBitmap() BORROWS the bytes until the ticket completes,
// so drawing the next frame into the buffer the transmitter is still reading would tear the
// image on the wire. Two buffers, and the renderer only ever touches the one that is not in
// flight.

#include <Arduino.h>
#include <AffaDisplay.h>
#include <ESPmDNS.h>
#include <ElegantOTA.h>
#include <Preferences.h>
#include <PsychicHttp.h>
#include <WiFi.h>

#include "media_render.h"

#if !AFFA_PANEL_CARMINAT
#  error "17_mediascreen is a Carminat example: build with -D AFFA_PANEL_CARMINAT=1"
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
constexpr const char* kApSsid   = "AffaMedia";
constexpr const char* kApPass   = "affa1234";
constexpr const char* kMdnsName = "affamedia";
constexpr uint32_t    kStaJoinMs = 15000;

struct ArduinoClock final : affa::IClock {
  uint32_t millis() const override { return ::millis(); }
};

affa::CanCommonLink   g_link;
ArduinoClock          g_clock;
affa::CarminatDisplay g_display(g_link, g_clock);
PsychicHttpServer     g_server;

// ---------------------------------------------------------------------------
// Player state
// ---------------------------------------------------------------------------
struct Player {
  char     title[64]  = "NEVER GONNA GIVE YOU UP";
  char     artist[32] = "RICK ASTLEY";
  uint32_t elapsed    = 0;          // seconds
  uint32_t total      = 213;        // seconds
  uint8_t  track      = 3;
  uint8_t  trackCount = 12;
  bool     playing    = true;
} g_p;

media::Bars g_bars;

// DOUBLE-BUFFERED ON PURPOSE. showNavBitmap() borrows until the ticket completes; drawing
// into the buffer still being transmitted would tear the image mid-transfer.
uint8_t  g_frame[2][media::kBytes];
uint8_t  g_drawInto = 0;
volatile bool  g_navBusy = false;
affa::TxTicket g_navTicket = affa::kNoTicket;

uint32_t g_framePeriodMs = 250;
uint32_t g_nextFrameMs   = 0;
uint32_t g_nextSecondMs  = 0;
uint32_t g_nextTitleMs   = 0;
uint32_t g_titlePeriodMs = 700;
uint16_t g_marqueeAt     = 0;

// Counters the console reads back.
volatile uint32_t g_frames = 0, g_framesOk = 0, g_framesFail = 0, g_busyDrops = 0;
volatile uint32_t g_txMsAccum = 0;      // total time images spent in flight
uint32_t g_frameStartMs = 0;

// ---------------------------------------------------------------------------
// Log ring
// ---------------------------------------------------------------------------
portMUX_TYPE g_logMux = portMUX_INITIALIZER_UNLOCKED;
constexpr int kLogLines = 32;
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
// The two layers
// ---------------------------------------------------------------------------

// THE PANE. Draw into the idle buffer, hand it over, swap.
void pushFrame() {
  if (g_navBusy) { ++g_busyDrops; return; }   // still in flight — skip, never queue up

  uint8_t* const buf = g_frame[g_drawInto];
  media::drawMedia(buf, g_bars, g_p.elapsed, g_p.total, g_p.track, g_p.trackCount,
                   g_p.playing);

  const affa::Result r = g_display.showNavBitmap(buf);
  if (r != affa::Result::Ok) { ++g_framesFail; return; }

  g_navTicket    = g_display.lastEnqueued();
  g_navBusy      = true;
  g_frameStartMs = ::millis();
  g_drawInto    ^= 1;                          // next frame goes in the other buffer
  ++g_frames;
}

// THE MAIN LINE. The title is longer than the field, so it marquees — and the field width
// is the thing to be careful about: `0x77` format 0x60 is plain ASCII, up to 8 characters
// visible. Anything wider is the panel's business, not ours.
void pushTitle() {
  char win[9];
  const size_t len = strlen(g_p.title);
  // Two spaces of gap so the wrap reads as a loop rather than a jump cut.
  const size_t span = len + 2;
  for (int i = 0; i < 8; ++i) {
    const size_t at = (g_marqueeAt + i) % span;
    win[i] = (at < len) ? g_p.title[at] : ' ';
  }
  win[8] = 0;
  g_marqueeAt = static_cast<uint16_t>((g_marqueeAt + 1) % span);
  (void)g_display.setText(win);
}

// ---------------------------------------------------------------------------
void onDone(affa::TxTicket t, affa::Result r, void*) {
  if (t != g_navTicket) return;
  g_navBusy = false;
  if (r == affa::Result::Ok) {
    ++g_framesOk;
    g_txMsAccum += ::millis() - g_frameStartMs;
  } else {
    ++g_framesFail;
    logmsg("frame FAILED (%d)", static_cast<int>(r));
  }
}

void onSyncChanged(affa::SyncState s, void*) {
  logmsg("sync 0x%02X%s", static_cast<unsigned>(s),
         affa::hasFlag(s, affa::SyncState::Failed) ? " FAILED" : "");
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
    j += "\"phase\":\"";   j += affa::phaseName(g_display.phase()); j += "\"";
    j += ",\"live\":";     j += g_link.isLive() ? "true" : "false";
    j += ",\"playing\":";  j += g_p.playing ? "true" : "false";
    j += ",\"elapsed\":";  j += g_p.elapsed;
    j += ",\"total\":";    j += g_p.total;
    j += ",\"track\":";    j += g_p.track;
    j += ",\"tracks\":";   j += g_p.trackCount;
    j += ",\"title\":\"";  j += g_p.title;  j += "\"";
    j += ",\"artist\":\""; j += g_p.artist; j += "\"";
    j += ",\"frames\":";   j += g_frames;
    j += ",\"ok\":";       j += g_framesOk;
    j += ",\"fail\":";     j += g_framesFail;
    j += ",\"drops\":";    j += g_busyDrops;
    j += ",\"periodms\":"; j += g_framePeriodMs;
    // The number that decides whether the frame rate is sane: how much of the link the
    // images are actually occupying.
    j += ",\"duty\":";     j += (g_txMsAccum * 100) / up;
    j += ",\"avgms\":";    j += (g_framesOk ? g_txMsAccum / g_framesOk : 0);
    j += ",\"heap\":";     j += (uint32_t)ESP.getFreeHeap();
    j += "}";
    PsychicResponse res(r);
    res.setContentType("application/json");
    res.setContent(j.c_str());
    return res.send();
  });

  g_server.on("/api/player", HTTP_GET, [](PsychicRequest* r) {
    const auto has = [&](const char* k) { return r->hasParam(k); };
    const auto str = [&](const char* k) { return r->getParam(k)->value(); };
    const auto num = [&](const char* k) { return strtoul(str(k).c_str(), nullptr, 10); };

    if (has("title"))  { snprintf(g_p.title, sizeof(g_p.title), "%s", str("title").c_str());
                         g_marqueeAt = 0; }
    if (has("artist")) snprintf(g_p.artist, sizeof(g_p.artist), "%s", str("artist").c_str());
    if (has("total"))  g_p.total   = num("total");
    if (has("elapsed"))g_p.elapsed = num("elapsed");
    if (has("track"))  g_p.track   = static_cast<uint8_t>(num("track"));
    if (has("tracks")) g_p.trackCount = static_cast<uint8_t>(num("tracks"));
    if (has("play"))   g_p.playing = (str("play") != "0");
    if (has("period")) {
      // FLOOR AT 120 ms. One image is ~50 ms of round trips; below about 120 ms the pane
      // starves the title and the handshake and every frame lands on a busy transmitter.
      const uint32_t v = num("period");
      g_framePeriodMs = v < 120 ? 120 : (v > 5000 ? 5000 : v);
    }
    if (has("titlems")) g_titlePeriodMs = num("titlems") < 150 ? 150 : num("titlems");
    logmsg("player: %s %lu/%lu track %u", g_p.playing ? "play" : "pause",
           (unsigned long)g_p.elapsed, (unsigned long)g_p.total, g_p.track);
    return r->reply(200, "text/plain", "ok");
  });

  g_server.on("/api/power", HTTP_GET, [](PsychicRequest* r) {
    const bool on = !r->hasParam("on") || r->getParam("on")->value() != "0";
    const affa::Result res = g_display.setPower(on);
    return r->reply(res == affa::Result::Ok ? 200 : 409, "text/plain", on ? "on" : "off");
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
  Serial.printf("\n[wifi] %s ip=%s  http://%s/  OTA http://%s/update\n",
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
  Serial.println("\n[media] 17_mediascreen — both layers at once");

  if (!g_link.begin(kRxPin, kTxPin, kBitrate)) Serial.println("[media] CAN begin FAILED");
  g_display.onComplete(&onDone, nullptr);
  g_display.onSync(&onSyncChanged, nullptr);
  if (!g_display.begin()) Serial.println("[media] display begin FAILED");

  startWifi();
  startHttp();
  logmsg("boot: %u-byte frames, %lu ms period", media::kBytes,
         (unsigned long)g_framePeriodMs);
}

void loop() {
  g_display.poll();
  ElegantOTA.loop();

  const uint32_t now = millis();

  // The clock, once a second, independent of the frame rate.
  if (static_cast<int32_t>(now - g_nextSecondMs) >= 0) {
    g_nextSecondMs = now + 1000;
    if (g_p.playing && g_p.elapsed < g_p.total) ++g_p.elapsed;
    else if (g_p.playing && g_p.total) {
      g_p.elapsed = 0;                                    // loop the track
      g_p.track = static_cast<uint8_t>((g_p.track % g_p.trackCount) + 1);
    }
  }

  // The pane.
  if (static_cast<int32_t>(now - g_nextFrameMs) >= 0) {
    g_nextFrameMs = now + g_framePeriodMs;
    g_bars.step(g_p.playing);
    pushFrame();
  }

  // The main line. Deliberately on its own timer: the title should not stutter because the
  // spectrum is busy, and it goes out on 0x151 while the image goes out on 0x1F1, so the
  // two never contend for the same function slot.
  if (static_cast<int32_t>(now - g_nextTitleMs) >= 0) {
    g_nextTitleMs = now + g_titlePeriodMs;
    pushTitle();
  }

  delay(1);      // yield: without it the IDLE task starves and the console stops answering
}
