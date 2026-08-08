// 18_aiscreen — a content feed on the Carminat, from anything that can compose a document.
//
// The producer is an LLM in the intended setup, but nothing here knows that. It receives a
// DisplayDocument (DisplayDocument.h) and does not care whether a model, a weather service,
// Home Assistant or curl composed it. That is the whole architecture, and the rule behind it:
//
//     A producer may compose SCREENS. A producer may never compose AffaDisplay commands.
//
// ── it works with no server at all ───────────────────────────────────────────
// A built-in deck rotates on a timer from the first second. Add a producer and it takes over;
// take the network away and the deck comes back. That is not a demo mode, it is the design:
// a car goes into tunnels, and a screen that goes blank when the hotspot drops is worse than
// one that repeats itself.
//
// ── endpoints ────────────────────────────────────────────────────────────────
//   GET /api/caps                    what this panel can show, as JSON. A producer builds
//                                    its prompt FROM THIS, so adding a panel family costs
//                                    nothing upstream.
//   GET /api/push?type=…&title=…     push one document. Query params, not a JSON body: same
//                                    field names as the contract, zero dependencies, and
//                                    curl-able from a phone. The body form is a drop-in
//                                    replacement when a parser is worth its flash.
//   GET /api/context?track=…&lat=…   what the car knows, for the producer to use
//   GET /api/state                   feed + link + the library's own diagnostics
//   GET /api/next                    advance the feed by hand
//   /update                          OTA
//
//   pio run -e ex18_aiscreen -t upload
#include <Arduino.h>
#include <AffaDisplay.h>
#include <ElegantOTA.h>
#include <Preferences.h>
#include <PsychicHttp.h>
#include <WiFi.h>

#include "DisplayDocument.h"
#include "../shared/media_render.h"
#include "../shared/nav_images.h"

#if !AFFA_PANEL_CARMINAT
#  error "18_aiscreen needs the Carminat panel: build with -D AFFA_PANEL_CARMINAT=1"
#endif

namespace {

constexpr gpio_num_t kRxPin   = GPIO_NUM_5;
constexpr gpio_num_t kTxPin   = GPIO_NUM_4;
constexpr uint32_t   kBitrate = 500000;

constexpr const char* kWifiNamespace = "megaopen";
constexpr const char* kApSsid   = "AffaScreen";
constexpr const char* kApPass   = "affa1234";
constexpr uint32_t    kStaJoinMs = 15000;

struct ArduinoClock final : affa::IClock {
  uint32_t millis() const override { return ::millis(); }
};

affa::CanCommonLink   g_link;
ArduinoClock          g_clock;
affa::CarminatDisplay g_display(g_link, g_clock);
affa::rtos::AffaTask  g_task;
PsychicHttpServer     g_server;

uint8_t g_menuBuf[affa::CarminatDisplay::menuScreenBytes(affa::carminat::kMenuMaxItems)];

// ---------------------------------------------------------------------------
// The feed
// ---------------------------------------------------------------------------
// A RING, NOT A QUEUE, and it is what makes the thing survive a tunnel. Documents pushed by
// a producer land here and rotate on their own TTL; when the ring runs dry the built-in deck
// refills it. Nothing ever waits on the network to have something to show.
//
// PREFETCH IS THE POINT. A producer sends several documents at once and the device rotates
// them locally, so a key press is instant and a lost connection costs nothing until the ring
// empties. One request an hour, not one per screen — an LLM in the render path would be
// seconds of latency, money per screen, and a blank panel whenever it failed.
constexpr uint8_t kFeedDepth = 6;

struct Feed {
  aidoc::DisplayDocument d[kFeedDepth];
  uint8_t  head = 0, count = 0;
  uint8_t  cur  = 0;
  uint32_t showUntil = 0;
  uint32_t served = 0, pushed = 0, dropped = 0;

  bool push(const aidoc::DisplayDocument& doc) {
    if (count >= kFeedDepth) { ++dropped; return false; }   // refuse, never overwrite
    d[(head + count) % kFeedDepth] = doc;
    ++count; ++pushed;
    return true;
  }
  aidoc::DisplayDocument* current() { return count ? &d[cur] : nullptr; }
  void advance() {
    if (!count) return;
    cur = static_cast<uint8_t>((cur + 1) % count);
    showUntil = 0;                                          // draw it on the next tick
  }
};
Feed g_feed;

aidoc::Context g_ctx;

// ---------------------------------------------------------------------------
// The built-in deck
// ---------------------------------------------------------------------------
// WHAT IT SHOWS WITH NO PRODUCER AT ALL. Deliberately the same shape a producer sends, so
// the offline path and the online path go through identical code — a fallback that took a
// different route would be the one that breaks unnoticed.
struct Canned { aidoc::DocType type; const char* title; const char* l0; const char* l1;
                const char* icon; const char* scene; };
const Canned kDeck[] = {
  { aidoc::DocType::Info,  "USELESS FACT", "Octopuses",  "have 3 hearts", "gps",   "" },
  { aidoc::DocType::Text,  "HELLO",        "",           "",             "",      "tryzub" },
  { aidoc::DocType::Info,  "CAR FACT",     "The Megane", "II is 2002",   "book",  "" },
  { aidoc::DocType::Info,  "NO NETWORK",   "showing the","built-in deck","search","rings" },
};
constexpr uint8_t kDeckCount = sizeof(kDeck) / sizeof(kDeck[0]);
uint8_t g_deckAt = 0;

void refillFromDeck() {
  const Canned& c = kDeck[g_deckAt];
  g_deckAt = static_cast<uint8_t>((g_deckAt + 1) % kDeckCount);

  aidoc::DisplayDocument doc;
  snprintf(doc.id, sizeof(doc.id), "deck-%u", g_deckAt);
  doc.type = c.type;
  snprintf(doc.title, sizeof(doc.title), "%s", c.title);
  if (*c.l0) snprintf(doc.lines[doc.lineCount++], aidoc::kTextMax, "%s", c.l0);
  if (*c.l1) snprintf(doc.lines[doc.lineCount++], aidoc::kTextMax, "%s", c.l1);
  snprintf(doc.icon,  sizeof(doc.icon),  "%s", c.icon);
  snprintf(doc.scene, sizeof(doc.scene), "%s", c.scene);
  doc.ttlMs = 8000;
  if (aidoc::normalise(doc, g_display.panelGeometry())) g_feed.push(doc);
}

// ---------------------------------------------------------------------------
// The pane — a NAMED scene, resolved here
// ---------------------------------------------------------------------------
// The document says "stars"; the bytes never leave the device. See DisplayDocument.h for why
// an animation is a name and not a payload.
// DELIBERATELY NOT THE DEMO'S SCENE ENGINE. 17_mediascreen dispatches eighteen scenes with
// their own state objects; copying that here would be the duplication this whole refactor
// has been removing. What this example needs is the CONTRACT — a name resolves to pixels on
// the device — so it carries the stills, which are already in flash, plus the two animations
// that need nothing but a counter.
//
// A scene is EITHER a still OR a draw function, never a name matched again at draw time. The
// `still ? memcpy : strcmp(name,"clock") ? ... : ...` chain that used to live in
// pushPaneFrame() meant adding an animation touched two places and re-compared strings on
// every frame; now the table is the only place.
uint32_t g_sceneFrame = 0;   // above the table: the animated scenes close over it
uint32_t g_paneFrames = 0;   // pane frames ACCEPTED for transmit — the only way to tell
                             // an animation apart from a still from outside the box
media::Eyes g_eyes;
struct Scene { const char* name; const uint8_t* still; void (*draw)(uint8_t*); };
const Scene kScenes[] = {
  { "globe",   navlab::kBmpGlobe,   nullptr },
  { "tryzub",  navlab::kBmpTryzub,  nullptr },
  { "renault", navlab::kBmpRenault, nullptr },
  { "gauges",  navlab::kBmpGauges,  nullptr },
  { "clock",   nullptr, [](uint8_t* b) { media::drawClockFace(b, ::millis() / 1000); } },
  { "rings",   nullptr, [](uint8_t* b) { media::drawRings(b, g_sceneFrame); } },
  // The face. It belongs on THIS example more than on the demo: an AI that tells you an
  // octopus has three hearts should have somewhere to look while you read it.
  { "eyes",    nullptr, [](uint8_t* b) { g_eyes.step(); media::drawEyes(b, g_eyes); } },
};
constexpr uint8_t kSceneCount = sizeof(kScenes) / sizeof(kScenes[0]);

int8_t     g_scene = -1;                    // -1 = the pane is not ours to drive
uint32_t   g_nextFrameMs = 0, g_panePeriodMs = 250;
uint8_t    g_frame[2][media::kBytes];
uint8_t    g_drawInto = 0;
bool       g_paneBusy = false, g_forceFrame = true;
affa::TxTicket g_paneTicket = affa::kNoTicket;

void selectScene(const char* name) {
  const int8_t was = g_scene;
  g_scene = -1;
  if (name && *name)
    for (uint8_t i = 0; i < kSceneCount; ++i)
      if (!strcmp(kScenes[i].name, name)) { g_scene = static_cast<int8_t>(i); break; }
  if (g_scene != was) g_forceFrame = true;
}

void pushPaneFrame() {
  if (g_scene < 0 || g_paneBusy) return;
  const Scene& sc = kScenes[g_scene];
  // A STILL IS SENT ONCE. Re-transmitting an unchanged image is 44 CAN frames buying nothing,
  // and on this bus that is a quarter of the link.
  if (sc.still && !g_forceFrame) return;
  if (static_cast<int32_t>(::millis() - g_nextFrameMs) < 0) return;
  g_nextFrameMs = ::millis() + g_panePeriodMs;

  uint8_t* const buf = g_frame[g_drawInto];
  if (sc.still) memcpy(buf, sc.still, media::kBytes);
  else          { media::clear(buf); sc.draw(buf); }
  ++g_sceneFrame;

  if (!g_forceFrame && memcmp(buf, g_frame[g_drawInto ^ 1], media::kBytes) == 0) return;

  // BORROWED UNTIL THE TICKET COMPLETES, which is why there are two buffers: drawing into
  // the one still being transmitted would tear the image on the wire.
  const affa::Submitted s = g_display.showNavBitmap(buf);
  if (!s) return;
  ++g_paneFrames;
  g_paneTicket = s.ticket;
  g_paneBusy   = true;
  g_forceFrame = false;
  g_drawInto  ^= 1;
}

// ---------------------------------------------------------------------------
// Drawing one document
// ---------------------------------------------------------------------------
uint32_t g_renders = 0, g_renderFail = 0;
char     g_lastId[16] = {0};

void showCurrent() {
  aidoc::DisplayDocument* doc = g_feed.current();
  if (!doc) { refillFromDeck(); doc = g_feed.current(); if (!doc) return; }

  selectScene(doc->scene);
  const affa::Submitted s = aidoc::render(g_display, *doc, g_menuBuf, sizeof(g_menuBuf));
  if (s) ++g_renders; else ++g_renderFail;

  snprintf(g_lastId, sizeof(g_lastId), "%s", doc->id);
  g_feed.showUntil = ::millis() + (doc->ttlMs ? doc->ttlMs : 8000);
  ++g_feed.served;
}

void onDone(affa::TxTicket t, affa::Result, void*) {
  if (t == g_paneTicket) { g_paneBusy = false; g_paneTicket = affa::kNoTicket; }
}

// KEYS ROTATE THE FEED, and they do it LOCALLY. A press that had to reach a server first
// would be 200 ms on a good hotspot and never in a tunnel; the ring is already full, so the
// next screen is instant and the producer refills behind it.
void onKey(affa::Key k, affa::KeyEdge e, void*) {
  if (e != affa::KeyEdge::Click) return;
  if (k == affa::Key::RollDown || k == affa::Key::SrcNext) { g_feed.advance(); }
  else if (k == affa::Key::RollUp || k == affa::Key::SrcPrev) {
    if (g_feed.count) g_feed.cur = static_cast<uint8_t>((g_feed.cur + g_feed.count - 1) %
                                                        g_feed.count);
    g_feed.showUntil = 0;
  }
}

// ---------------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------------
String g_out;
void jclear() { g_out = ""; }
void jf(const char* fmt, ...) {
  char b[320]; va_list ap; va_start(ap, fmt);
  vsnprintf(b, sizeof(b), fmt, ap); va_end(ap); g_out += b;
}
esp_err_t replyJson(PsychicRequest* r) { return r->reply(200, "application/json", g_out.c_str()); }

String q(PsychicRequest* r, const char* k, const char* def = "") {
  return r->hasParam(k) ? r->getParam(k)->value() : String(def);
}

// THE PRODUCER'S PROMPT IS BUILT FROM THIS. Every number comes from panelGeometry(), so a
// different panel family reports different limits and the producer adapts with no change
// upstream — which is the entire reason the geometry is asked for instead of assumed.
esp_err_t caps(PsychicRequest* r) {
  const affa::PanelGeometry g = g_display.panelGeometry();
  jclear();
  jf("{\"panel\":\"carminat\",\"types\":[\"text\",\"info\",\"list\",\"message\",\"question\",\"image\"]");
  jf(",\"mainChars\":%u,\"infoRows\":%u,\"infoRowChars\":%u", g.mainChars, g.infoRows,
     g.infoRowChars);
  jf(",\"menuRows\":%u,\"menuRowChars\":%u", g.menuRows, g.menuRowChars);
  jf(",\"listMaxItems\":%u,\"listItemChars\":%u", g.listMaxItems, g.listItemChars);
  jf(",\"image\":{\"w\":%u,\"h\":%u}", g.imageWidth, g.imageHeight);
  jf(",\"charset\":\"ascii — Cyrillic and Polish are transliterated; emoji become '?'\"");
  jf(",\"icons\":[");
  for (uint8_t i = 0; i < aidoc::kIconCount; ++i)
    jf("%s\"%s\"", i ? "," : "", aidoc::kIcons[i].name);
  jf("],\"scenes\":[");
  for (uint8_t i = 0; i < kSceneCount; ++i) jf("%s\"%s\"", i ? "," : "", kScenes[i].name);
  jf("],\"feedDepth\":%u}", kFeedDepth);
  return replyJson(r);
}

// ONE DOCUMENT IN. Normalised against the panel before it is accepted, so a producer that
// overshoots gets a screen that fits rather than a rejection — see normalise().
esp_err_t push(PsychicRequest* r) {
  aidoc::DisplayDocument d;
  snprintf(d.id, sizeof(d.id), "%s", q(r, "id", "push").c_str());
  d.type = aidoc::docTypeFrom(q(r, "type", "info").c_str());
  snprintf(d.title, sizeof(d.title), "%s", q(r, "title").c_str());
  for (uint8_t i = 0; i < aidoc::kMaxLines; ++i) {
    char key[4]; snprintf(key, sizeof(key), "l%u", i);
    const String v = q(r, key);
    if (v.length()) snprintf(d.lines[d.lineCount++], aidoc::kTextMax, "%s", v.c_str());
  }
  // Items as one pipe-separated field: a list is one thing to a producer, and six numbered
  // parameters is a shape that invites off-by-one.
  const String items = q(r, "items");
  if (items.length()) {
    int from = 0;
    while (from <= items.length() && d.itemCount < aidoc::kMaxItems) {
      int bar = items.indexOf('|', from);
      if (bar < 0) bar = items.length();
      snprintf(d.items[d.itemCount++], aidoc::kTextMax, "%s",
               items.substring(from, bar).c_str());
      from = bar + 1;
    }
  }
  snprintf(d.icon,  sizeof(d.icon),  "%s", q(r, "icon").c_str());
  snprintf(d.scene, sizeof(d.scene), "%s", q(r, "scene").c_str());
  d.selected = static_cast<uint8_t>(q(r, "selected", "0").toInt());
  d.ttlMs    = static_cast<uint32_t>(q(r, "ttl", "30000").toInt());

  const bool ok = aidoc::normalise(d, g_display.panelGeometry()) && g_feed.push(d);
  jclear();
  jf("{\"ok\":%s,\"type\":\"%s\",\"queued\":%u,\"dropped\":%lu}",
     ok ? "true" : "false", aidoc::docTypeName(d.type), g_feed.count,
     static_cast<unsigned long>(g_feed.dropped));
  if (ok && g_feed.count == 1) g_feed.showUntil = 0;      // nothing on screen: draw it now
  return replyJson(r);
}

esp_err_t context(PsychicRequest* r) {
  const char* keys[] = { "track", "artist", "lat", "lon", "speed", "temp", "gear", "src" };
  for (const char* k : keys) if (r->hasParam(k)) g_ctx.set(k, q(r, k).c_str());
  if (r->hasParam("clear")) g_ctx.clear();
  jclear();
  jf("{\"count\":%u,\"ctx\":{", g_ctx.count);
  for (uint8_t i = 0; i < g_ctx.count; ++i)
    jf("%s\"%s\":\"%s\"", i ? "," : "", g_ctx.kv[i].k, g_ctx.kv[i].v);
  jf("}}");
  return replyJson(r);
}

esp_err_t state(PsychicRequest* r) {
  const affa::rtos::Status st = g_task.status();
  jclear();
  jf("{\"phase\":\"%s\",\"live\":%s", affa::phaseName(st.phase),
     g_link.isLive() ? "true" : "false");
  jf(",\"feed\":{\"queued\":%u,\"cur\":%u,\"served\":%lu,\"pushed\":%lu,\"dropped\":%lu}",
     g_feed.count, g_feed.cur, static_cast<unsigned long>(g_feed.served),
     static_cast<unsigned long>(g_feed.pushed), static_cast<unsigned long>(g_feed.dropped));
  jf(",\"last\":\"%s\",\"renders\":%lu,\"renderFail\":%lu", g_lastId,
     static_cast<unsigned long>(g_renders), static_cast<unsigned long>(g_renderFail));
  jf(",\"pane\":%lu", static_cast<unsigned long>(g_paneFrames));
  jf(",\"scene\":\"%s\"", g_scene < 0 ? "" : kScenes[g_scene].name);
  // The library's own diagnostics — see docs/REFACTOR-2.0.md §3.6 for why the callback is
  // named rather than merely counted.
  jf(",\"lost\":%lu,\"foreign\":%lu,\"posted\":%u,\"postDropped\":%lu",
     static_cast<unsigned long>(st.sessionsLost), static_cast<unsigned long>(st.foreignPolls),
     st.posted, static_cast<unsigned long>(st.postDropped));
  jf(",\"cbworst\":\"%s\",\"cbms\":%lu,\"cbover\":%lu", affa::cbName(st.slowestCb),
     static_cast<unsigned long>(st.slowestCbMs), static_cast<unsigned long>(st.cbOverruns));
  jf(",\"heap\":%lu,\"up\":%lu}", static_cast<unsigned long>(ESP.getFreeHeap()),
     static_cast<unsigned long>(::millis() / 1000));
  return replyJson(r);
}

void startWifi() {
  Preferences p;
  String ssid, pass;
  if (p.begin(kWifiNamespace, true)) {
    ssid = p.getString("ssid", ""); pass = p.getString("pass", ""); p.end();
  }
  if (ssid.length()) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());
    const uint32_t until = ::millis() + kStaJoinMs;
    while (WiFi.status() != WL_CONNECTED && static_cast<int32_t>(::millis() - until) < 0)
      delay(200);
  }
  // THE SOFTAP FALLBACK IS NOT A FAULT PATH, it is how the board stays reachable when the
  // credentials are wrong or the network moved. Losing OTA means needing a cable.
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(kApSsid, kApPass);
    Serial.printf("[net] SoftAP %s at %s\n", kApSsid, WiFi.softAPIP().toString().c_str());
  } else {
    Serial.printf("[net] %s\n", WiFi.localIP().toString().c_str());
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\nAffaDisplay 18_aiscreen");

  if (!g_link.begin(kRxPin, kTxPin, kBitrate)) Serial.println("[can] begin FAILED");

  g_display.onKey(&onKey, nullptr);
  g_display.onComplete(&onDone, nullptr);
  if (!g_display.begin()) Serial.println("[affa] begin FAILED");
  if (!g_task.start(g_display)) Serial.println("[affa] task start FAILED");

  startWifi();

  // EVERY ONE OF THESE IS A SCAR, copied from 17_mediascreen rather than left at the
  // library's defaults — the first draft of this file left them default and the board went
  // unreachable on the first OTA.
  //
  //   lru_purge_enable   PsychicHttp only sets it under ENABLE_ASYNC, which these builds do
  //                      not define. Without it, seven lingering sockets stop httpd calling
  //                      accept() and it NEVER resumes: ping answers, mDNS answers, OTA is
  //                      gone. A one-way door.
  //   stack_size         the httpd default is 4096 and every handler here builds JSON on it.
  //   max_uri_handlers   ElegantOTA registers several of its own; a full table silently
  //                      unregisters the LAST route added, and if that is /ota/upload the
  //                      board needs a cable.
  g_server.config.lru_purge_enable  = true;
  g_server.config.max_open_sockets  = 7;
  g_server.config.recv_wait_timeout = 3;
  g_server.config.send_wait_timeout = 3;
  g_server.config.max_uri_handlers  = 64;
  g_server.config.stack_size        = 10240;
  g_server.listen(80);
  // OTA FIRST — the only way back into a board with no cable.
  ElegantOTA.begin(&g_server);
  g_server.on("/api/caps",    HTTP_GET, caps);
  g_server.on("/api/push",    HTTP_GET, push);
  g_server.on("/api/context", HTTP_GET, context);
  g_server.on("/api/state",   HTTP_GET, state);
  g_server.on("/api/next",    HTTP_GET, [](PsychicRequest* r) {
    g_feed.advance(); jclear(); jf("{\"cur\":%u}", g_feed.cur); return replyJson(r);
  });
  g_server.on("/", HTTP_GET, [](PsychicRequest* r) {
    return r->reply(200, "text/plain",
                    "18_aiscreen\n"
                    "  /api/caps      what this panel can show\n"
                    "  /api/push?type=info&title=FACT&l0=...&l1=...&icon=gps&scene=stars\n"
                    "  /api/context?track=...&lat=...&lon=...\n"
                    "  /api/state     feed + link + diagnostics\n"
                    "  /api/next      advance\n"
                    "  /update        OTA\n");
  });
}

void loop() {
  // NO poll() HERE — the library owns its task. Every render below is called from THIS task
  // and crosses into the poll task as data, which is what makes it safe to write plainly.
  ElegantOTA.loop();

  const uint32_t now = ::millis();

  // The feed advances on the document's own TTL. Refilled from the deck when it runs dry, so
  // there is always something on the glass.
  if (g_display.phase() == affa::Phase::Ready &&
      static_cast<int32_t>(now - g_feed.showUntil) >= 0) {
    if (g_feed.count) g_feed.advance();
    if (g_feed.count < 2) refillFromDeck();
    showCurrent();
  }

  pushPaneFrame();
  delay(5);
}
