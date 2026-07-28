// 14_updatelist_bench — examples/13_updatelist_text, plus the way back in.
//
// Identical AFFA2 behaviour to 13: an UpdateList LCD, a running text row, the 10:00 link
// clock. The ONLY additions are WiFi, ElegantOTA and a handful of HTTP endpoints, and they
// exist for one reason: the bench board has no serial cable and no buttons, so OTA is the
// only way to flash it again. A firmware without a network on that board is a brick.
//
// If you are reading this to learn the library, read 13 instead — it is the same thing
// with none of the plumbing. This file is 13 with a safety net.
//
// Endpoints:
//   GET /api/status                 link, sync, marquee and controller counters
//   GET /api/text?t=HELLO           one static render (stops the marquee)
//   GET /api/scroll?t=...&run=1     set the scrolling text / start / stop
//   GET /api/state?on=0|1           display power — MANDATORY before anything renders
//   GET /api/time?hhmm=1000         set the clock
//   GET /api/reboot
//   GET /update                     ElegantOTA

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <PsychicHttp.h>
#include <ElegantOTA.h>
#include <AffaDisplay.h>

#if !AFFA_PANEL_UPDATELIST_MENU || !AFFA_ENABLE_MARQUEE
#  error "14_updatelist_bench needs AFFA_PANEL_UPDATELIST_MENU=1 and AFFA_ENABLE_MARQUEE=1"
#endif

namespace {

struct ArduinoClock final : affa::IClock {
  uint32_t millis() const override { return ::millis(); }
};

constexpr affa::CanPins kPins{ .rx = GPIO_NUM_4, .tx = GPIO_NUM_3 };
constexpr uint32_t      kBitrate = 500000;

constexpr const char* kNvsNamespace = "megaopen";   // read-only; never written here
constexpr const char* kApSsid       = "AffaBench";
constexpr const char* kApPass       = "affabench";
constexpr const char* kMdnsName     = "affabench";
constexpr uint32_t    kStaJoinMs    = 15000;

constexpr const char* kText      = "AFFA2 UPDATE LIST - RUNNING TEXT DEMO";

affa::Esp32CanLink          g_hw;
ArduinoClock                g_clock;
affa::UpdateListMenuDisplay g_display(g_hw, g_clock);
PsychicHttpServer           g_server;

bool g_canUp = false;

// THERE IS NO CLOCK IN THIS FAMILY. The Carminat examples set 10:00 on every sync as a
// link light; UpdateList has no setTime wire operation — supports(Feature::Time) is false
// and the call returns NotSupported. The idea does not port, and the copy of it that was
// here retried NotSupported on every loop pass for ever.

// ---------------------------------------------------------------------------
// Frame ring — Layer 0
// ---------------------------------------------------------------------------
// EVERY diagnosis on this bench has come down to reading the wire; leaving this out of the
// first cut cost an hour of guessing. 48 frames, direction stamped, pushed from the tap.
struct Rec { uint32_t ms; uint8_t dir; uint16_t id; uint8_t d[8]; };
constexpr uint8_t kRing = 48;
Rec g_ring[kRing];
uint8_t g_head = 0;

void onTap(const affa::Frame& f, affa::Direction dir, void*) {
  Rec& r = g_ring[g_head];
  r.ms   = millis();
  r.dir  = (dir == affa::Direction::Tx) ? 2 : 1;
  r.id   = static_cast<uint16_t>(f.id);
  for (uint8_t i = 0; i < 8; ++i) r.d[i] = f.data[i];
  g_head = static_cast<uint8_t>((g_head + 1) % kRing);
}

// Commands run on the LOOP task, never on the HTTP task, so the library only ever sees one
// caller and needs no locking. The HTTP handler posts and waits for the flag to clear.
struct Cmd {
  volatile bool pending = false;
  char          op[12]  = {0};
  char          s1[48]  = {0};
  long          a       = 0;
  char          reply[96] = {0};
} g_cmd;


const char* resultName(affa::Result r) {
  switch (r) {
    case affa::Result::Ok:           return "Ok";
    case affa::Result::UnknownFunc:  return "UnknownFunc";
    case affa::Result::NoSync:       return "NoSync";
    case affa::Result::LinkDown:     return "LinkDown";
    case affa::Result::QueueFull:    return "QueueFull";
    case affa::Result::Timeout:      return "Timeout";
    case affa::Result::SendFailed:   return "SendFailed";
    case affa::Result::TooLong:      return "TooLong";
    case affa::Result::BadArgument:  return "BadArgument";
    case affa::Result::NotSupported: return "NotSupported";
    case affa::Result::Cancelled:    return "Cancelled";
    case affa::Result::Aborted:      return "Aborted";
  }
  return "?";
}

// Executed on the loop task.
void runCmd() {
  if (!g_cmd.pending) return;
  affa::Result r = affa::Result::Ok;

  if (!strcmp(g_cmd.op, "text")) {
    g_display.setScrollActive(false);
    r = g_display.setText(g_cmd.s1);
  } else if (!strcmp(g_cmd.op, "scroll")) {
    if (g_cmd.s1[0]) g_display.setScrollText(g_cmd.s1);
    g_display.setScrollActive(g_cmd.a != 0);
  } else if (!strcmp(g_cmd.op, "state")) {
    r = g_display.setPower(g_cmd.a != 0);
  } else if (!strcmp(g_cmd.op, "time")) {
    r = g_display.setTime(g_cmd.s1);
  }

  snprintf(g_cmd.reply, sizeof(g_cmd.reply),
           "{\"op\":\"%s\",\"result\":\"%s\",\"scrolling\":%s}",
           g_cmd.op, resultName(r), g_display.scrollActive() ? "true" : "false");
  g_cmd.pending = false;
}

esp_err_t post(PsychicRequest* req, const char* op, const char* s1, long a) {
  strncpy(g_cmd.op, op, sizeof(g_cmd.op) - 1);
  strncpy(g_cmd.s1, s1 ? s1 : "", sizeof(g_cmd.s1) - 1);
  g_cmd.a       = a;
  g_cmd.reply[0] = '\0';
  g_cmd.pending = true;
  const uint32_t t0 = millis();
  while (g_cmd.pending && millis() - t0 < 3000) delay(5);
  return req->reply(200, "application/json",
                    g_cmd.reply[0] ? g_cmd.reply : "{\"error\":\"timeout\"}");
}

String qs(PsychicRequest* r, const char* k, const char* dflt = "") {
  return r->hasParam(k) ? r->getParam(k)->value() : String(dflt);
}

void routes() {
  g_server.on("/api/status", HTTP_GET, [](PsychicRequest* r) {
    const affa::Stats s = g_display.stats();
    const auto        d = g_hw.driverState();
    char b[600];
    snprintf(b, sizeof(b),
      "{\"uptimeMs\":%lu,\"heap\":%lu,\"ip\":\"%s\",\"canUp\":%s,"
      "\"sync\":{\"state\":%u,\"synced\":%s,\"registered\":%s},"
      "\"scrolling\":%s,"
      "\"link\":{\"rxFrames\":%lu,\"txFrames\":%lu,\"ringOverflow\":%lu},"
      "\"drv\":{\"valid\":%s,\"state\":%u,\"txErr\":%lu,\"rxErr\":%lu,\"busErr\":%lu}}",
      static_cast<unsigned long>(millis()),
      static_cast<unsigned long>(ESP.getFreeHeap()),
      (WiFi.isConnected() ? WiFi.localIP() : WiFi.softAPIP()).toString().c_str(),
      g_canUp ? "true" : "false",
      static_cast<unsigned>(g_display.syncState()),
      g_display.synced() ? "true" : "false",
      g_display.registered() ? "true" : "false",
      g_display.scrollActive() ? "true" : "false",
      static_cast<unsigned long>(s.rxFrames), static_cast<unsigned long>(s.txFrames),
      static_cast<unsigned long>(s.ringOverflow),
      d.valid ? "true" : "false", static_cast<unsigned>(d.state),
      static_cast<unsigned long>(d.txErr), static_cast<unsigned long>(d.rxErr),
      static_cast<unsigned long>(d.busErr));
    return r->reply(200, "application/json", b);
  });

  g_server.on("/api/text", HTTP_GET, [](PsychicRequest* r) {
    return post(r, "text", qs(r, "t", "HELLO").c_str(), 0);
  });
  g_server.on("/api/scroll", HTTP_GET, [](PsychicRequest* r) {
    return post(r, "scroll", qs(r, "t").c_str(), qs(r, "run", "1").toInt());
  });
  g_server.on("/api/state", HTTP_GET, [](PsychicRequest* r) {
    return post(r, "state", "", qs(r, "on", "1").toInt());
  });
  g_server.on("/api/time", HTTP_GET, [](PsychicRequest* r) {
    return post(r, "time", qs(r, "hhmm", "1000").c_str(), 0);
  });
  // /api/raw?id=<dec>&hex=<payload> — enqueue an ARBITRARY payload on an ARBITRARY id.
  //
  // The bench tool this session kept needing. enqueue() is public API, so an application
  // can put any bytes on any function id and let the transport do the ISO-TP framing, the
  // registration and the ACK matching. That makes it possible to try an encoding this
  // firmware was not built for — a different panel family's setText, say — without a
  // reflash per experiment.
  g_server.on("/api/raw", HTTP_GET, [](PsychicRequest* r) {
    const uint16_t id  = static_cast<uint16_t>(qs(r, "id", "289").toInt());
    const String   hex = qs(r, "hex");
    uint8_t  buf[AFFA_MAX_PAYLOAD];
    uint8_t  n = 0;
    for (size_t i = 0; i + 1 < hex.length() && n < sizeof(buf); i += 2) {
      auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
      };
      const int hi = nib(hex[i]), lo = nib(hex[i + 1]);
      if (hi < 0 || lo < 0) break;
      buf[n++] = static_cast<uint8_t>((hi << 4) | lo);
    }
    if (n == 0) return r->reply(400, "application/json", "{\"error\":\"bad hex\"}");

    affa::TxOptions opt;
    opt.slot     = affa::RenderSlot::None;   // never coalesced
    opt.coalesce = false;
    const affa::TxTicket t = g_display.enqueue(id, buf, n, opt);
    char b[128];
    snprintf(b, sizeof(b), "{\"id\":%u,\"len\":%u,\"ticket\":%lu,\"result\":\"%s\"}",
             static_cast<unsigned>(id), static_cast<unsigned>(n),
             static_cast<unsigned long>(t),
             t == affa::kNoTicket ? resultName(g_display.lastResult()) : "Queued");
    return r->reply(200, "application/json", b);
  });

  g_server.on("/api/frames", HTTP_GET, [](PsychicRequest* r) {
    String out = "{\"f\":[";
    const uint8_t head = g_head;
    bool first = true;
    for (uint8_t i = 0; i < kRing; ++i) {
      const Rec& e = g_ring[(head + i) % kRing];
      if (e.id == 0) continue;
      char b[96];
      snprintf(b, sizeof(b),
               "%s[%lu,%u,%u,\"%02X%02X%02X%02X%02X%02X%02X%02X\"]",
               first ? "" : ",", static_cast<unsigned long>(e.ms),
               static_cast<unsigned>(e.dir), static_cast<unsigned>(e.id),
               e.d[0], e.d[1], e.d[2], e.d[3], e.d[4], e.d[5], e.d[6], e.d[7]);
      out += b;
      first = false;
    }
    out += "]}";
    return r->reply(200, "application/json", out.c_str());
  });

  g_server.on("/api/reboot", HTTP_GET, [](PsychicRequest* r) {
    r->reply(200, "application/json", "{\"reboot\":true}");
    delay(200);
    ESP.restart();
    return ESP_OK;
  });
  g_server.on("/", HTTP_GET, [](PsychicRequest* r) {
    return r->reply(200, "text/plain",
                    "AffaDisplay UpdateList bench\n"
                    "/api/status /api/text?t= /api/scroll?t=&run= /api/state?on= "
                    "/api/time?hhmm= /api/reboot /update\n");
  });
}

void startNetwork() {
  // READ-ONLY. An NVS write stops CAN reception outright — the TWAI ISR is not in IRAM —
  // and these credentials belong to another project.
  Preferences p;
  String ssid, pass;
  if (p.begin(kNvsNamespace, /*readOnly=*/true)) {
    ssid = p.getString("ssid", "");
    pass = p.getString("pass", "");
    p.end();
  }

  WiFi.persistent(false);
  WiFi.setSleep(true);      // the C3's single radio interleaves WiFi with CAN timing

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

  Serial.printf("\n[wifi] %s ip=%s  OTA http://%s/update\n", sta ? "STA" : "AP",
                (sta ? WiFi.localIP() : WiFi.softAPIP()).toString().c_str(),
                (sta ? WiFi.localIP() : WiFi.softAPIP()).toString().c_str());
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);

  // CAN BEFORE WIFI, AND THIS ORDER IS THE WHOLE POINT OF THIS FILE.
  //
  // The Carminat panel is UNIVERSAL: attached to an UpdateList radio it behaves as an
  // UpdateList panel. It decides which it is at ITS power-up, from the radio it can hear —
  // and the panel shares a supply with this board, so the two boot together. Bringing WiFi
  // up first costs up to kStaJoinMs of SILENCE on the bus, during which the panel hears no
  // radio at all and settles into its default family. By the time we start sending 0x3DF it
  // has already chosen, and every AFFA2 frame after that is acknowledged and not rendered.
  //
  // Every other example in this repository says NETWORK FIRST, ALWAYS, and means it: on a
  // board with no serial cable, OTA is the only way back in. The exception is safe here for
  // one reason — Esp32CanLink::begin() is BOUNDED. It installs a driver, reads a status
  // word and returns; there is no join, no retry loop and nothing to wait for. It cannot
  // hang the way a WiFi join can, so it cannot cost us the network.
  g_canUp = g_hw.begin(kPins, kBitrate, /*forceRecoveryMs=*/250);

  g_display.onFrame(&onTap, nullptr);
  g_display.begin();

  // Start the heartbeat IMMEDIATELY, before the network, so the panel hears an UpdateList
  // radio within a few ms of its own boot rather than fifteen seconds later.
  for (uint8_t i = 0; i < 40; ++i) { g_display.poll(); delay(5); }

  // MANDATORY. `0x1B1 04 52 02 FF FF` (WIRE-SPEC §9.3). Without it the panel acknowledges
  // every frame and renders none of them — a healthy link drawing to a dark screen.
  (void)g_display.setPower(true);
  g_display.setScrollText(kText);
  g_display.setScrollActive(true);
  for (uint8_t i = 0; i < 60; ++i) { g_display.poll(); delay(5); }

  // NOW the network, so there is still a way back in.
  startNetwork();
  g_server.config.max_uri_handlers = 24;
  g_server.listen(80);
  routes();
  // An OTA write stalls CAN reception (the TWAI ISR is not in IRAM), so gate our own
  // transmitter off rather than shout at a bus we cannot hear. Expect a resync afterwards.
  ElegantOTA.onStart([]() { g_hw.setTxEnabled(false); });
  ElegantOTA.begin(&g_server);

  if (!g_canUp) Serial.println("[can ] begin() failed — network is up for OTA");
  Serial.println("[bench] updatelist: running text, OTA at /update");
}

void loop() {
  // poll() is also the KEEP-ALIVE. UpdateList drops the link if the 0x3DF heartbeat stops,
  // so blocking here does not merely delay a render — it disconnects the display.
  g_display.poll();
  runCmd();
  ElegantOTA.loop();
}
