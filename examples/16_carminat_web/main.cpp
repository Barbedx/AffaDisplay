// 16_carminat_web — Carminat/AFFA3: text and clock, driven from a browser, with OTA.
//
// The FIRST of three sibling examples that do the same two things on all three panel
// families this library knows, so the differences between the families are the only thing
// that differs between the files:
//
//   16_carminat_web    Carminat/AFFA3   setText ✅   setTime ✅ (`0x151 05 56 H H M M`)
//   17_updatelist_web  UpdateList/AFFA2 setText ✅   setTime ❌ — the family has none
//   18_cluster_web     dashboard cluster setText ❌   clock ✅? raw `3EF A6 hh mm`, UNTESTED
//
// This one is the easy case: BOTH operations are real wire commands on this panel and BOTH
// have been seen on glass (docs/BENCH-VERIFIED.md — `AFFA OK` rendered, clock set to 10:00
// and free-running an hour later). The other two files are where the honesty is needed.
//
// WHY A WEB SERVER AND NOT A HARD-CODED STRING. examples/02_carminat_text already is the
// hard-coded string, and it is the file to read to learn the library. This one exists to be
// FLASHED: the bench board at 192.168.100.85 has no serial cable and no buttons, so a
// firmware without a network on it cannot be replaced. OTA is the way back in, and the text
// box is what makes the panel worth looking at while you are there.
//
// Endpoints:
//   GET /                       the UI
//   GET /api/status             link, sync and controller counters, plus supports() flags
//   GET /api/text?t=HELLO       setText  — the panel renders about the first 7 characters
//   GET /api/time?h=10&m=00     setTime  — four ASCII digits on the wire, "HHMM"
//   GET /api/power?on=0|1       setPower — 0x151 `03 52 09|00 FF FF`
//   GET /api/reboot
//   GET /update                 ElegantOTA

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <PsychicHttp.h>
#include <ElegantOTA.h>
#include <AffaDisplay.h>

#if !AFFA_PANEL_CARMINAT
#  error "16_carminat_web needs -D AFFA_PANEL_CARMINAT=1"
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
constexpr const char* kMdnsName     = "affacarminat";
constexpr uint32_t    kStaJoinMs    = 15000;

affa::Esp32CanLink    g_hw;
ArduinoClock          g_clock;
affa::CarminatDisplay g_display(g_hw, g_clock);
PsychicHttpServer     g_server;

bool g_canUp = false;

// ---------------------------------------------------------------------------
// Command mailbox — the library only ever sees the loop task
// ---------------------------------------------------------------------------
// Every render runs on the LOOP task, never on the HTTP task, so the library has exactly
// one caller and needs no locking (docs/API.md §4). The handler posts and waits.
struct Cmd {
  volatile bool pending = false;
  char op[8]    = {0};
  char s1[32]   = {0};
  long a = 0, b = 0;
  char reply[96] = {0};
} g_cmd;

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

void runCmd() {
  if (!g_cmd.pending) return;
  affa::Result r = affa::Result::Ok;

  if (!strcmp(g_cmd.op, "text")) {
    r = g_display.setText(g_cmd.s1);
  } else if (!strcmp(g_cmd.op, "time")) {
    // The wire wants four ASCII digits, "HHMM", and setTime() returns BadArgument for a
    // short string rather than reading past it. Build them here so the browser can send
    // two ordinary numbers.
    char hhmm[5];
    snprintf(hhmm, sizeof(hhmm), "%02ld%02ld", g_cmd.a % 24, g_cmd.b % 60);
    r = g_display.setTime(hhmm);
  } else if (!strcmp(g_cmd.op, "power")) {
    r = g_display.setPower(g_cmd.a != 0);
  }

  // The ticket is read IMMEDIATELY after the enqueue: the next one overwrites it. It is
  // reported so the UI can tell "queued" from "drawn" — the Result is only an ACCEPTANCE
  // verdict, and the delivery verdict arrives later on onComplete().
  snprintf(g_cmd.reply, sizeof(g_cmd.reply),
           "{\"op\":\"%s\",\"result\":\"%s\",\"ticket\":%lu}", g_cmd.op, resultName(r),
           static_cast<unsigned long>(g_display.lastEnqueued()));
  g_cmd.pending = false;
}

esp_err_t post(PsychicRequest* req, const char* op, const char* s1, long a, long b) {
  strncpy(g_cmd.op, op, sizeof(g_cmd.op) - 1);
  strncpy(g_cmd.s1, s1 ? s1 : "", sizeof(g_cmd.s1) - 1);
  g_cmd.a = a;
  g_cmd.b = b;
  g_cmd.reply[0] = '\0';
  g_cmd.pending  = true;
  const uint32_t t0 = millis();
  while (g_cmd.pending && millis() - t0 < 3000) delay(5);
  return req->reply(200, "application/json",
                    g_cmd.reply[0] ? g_cmd.reply : "{\"error\":\"timeout\"}");
}

String qs(PsychicRequest* r, const char* k, const char* dflt = "") {
  return r->hasParam(k) ? r->getParam(k)->value() : String(dflt);
}

const char kPage[] PROGMEM = R"HTML(<!doctype html><meta name=viewport content="width=device-width,initial-scale=1">
<title>Carminat</title><style>
body{font:15px system-ui;margin:0;padding:16px;background:#111;color:#eee}
fieldset{border:1px solid #333;border-radius:8px;margin:0 0 14px;padding:10px}
legend{color:#7cf;padding:0 6px}input,button{font:14px system-ui;padding:7px;margin:3px 0;border-radius:6px;border:1px solid #444;background:#1c1c1c;color:#eee;box-sizing:border-box}
input{width:100%}button{background:#2a4;border:0;color:#fff;cursor:pointer;width:100%;padding:9px;font-weight:600}
button.alt{background:#357}small{color:#999}#log{white-space:pre-wrap;font:12px ui-monospace;color:#9c9;margin-top:10px;min-height:2em}
#st{font:12px ui-monospace;color:#9ac}
</style>
<h1 style="font-size:17px">Carminat / AFFA3 &mdash; text &amp; clock</h1>
<div id=st>&hellip;</div>
<fieldset><legend>Text &nbsp;<small>0x151 cmd 0x77 &mdash; the panel shows ~7 characters</small></legend>
<input id=t maxlength=14 value="AFFA OK">
<button onclick="go('text','t='+e(t.value))">setText</button></fieldset>
<fieldset><legend>Clock &nbsp;<small>0x151 `05 56 H H M M` &mdash; free-running afterwards</small></legend>
<input id=h type=number min=0 max=23 value=10 style="width:5em"> :
<input id=m type=number min=0 max=59 value=0 style="width:5em">
<button onclick="go('time','h='+h.value+'&m='+m.value)">setTime</button></fieldset>
<fieldset><legend>Display power &nbsp;<small>a powered-off panel ACKs everything and draws nothing</small></legend>
<button class=alt onclick="go('power','on=1')">Power ON</button>
<button class=alt onclick="go('power','on=0')">Power OFF</button></fieldset>
<p><a href="/update" style="color:#7cf">/update</a> &mdash; OTA</p>
<div id=log></div>
<script>
const e=encodeURIComponent;
async function go(op,q){const r=await fetch('/api/'+op+'?'+q);log.textContent=op+' -> '+await r.text();}
setInterval(async()=>{try{const s=await(await fetch('/api/status')).json();
st.textContent='can='+s.canUp+' sync=0x'+s.syncState.toString(16)+' registered='+s.registered+
' rx='+s.rxFrames+' tx='+s.txFrames;}catch(x){st.textContent='offline';}},2000);
</script>)HTML";

void routes() {
  g_server.on("/", HTTP_GET, [](PsychicRequest* r) {
    return r->reply(200, "text/html", kPage);
  });
  g_server.on("/api/status", HTTP_GET, [](PsychicRequest* r) {
    const affa::Stats s = g_display.stats();
    const auto        d = g_hw.driverState();
    char b[520];
    snprintf(b, sizeof(b),
      "{\"uptimeMs\":%lu,\"heap\":%lu,\"ip\":\"%s\",\"canUp\":%s,"
      "\"syncState\":%u,\"synced\":%s,\"registered\":%s,"
      "\"supports\":{\"text\":%s,\"time\":%s,\"power\":%s},"
      "\"rxFrames\":%lu,\"txFrames\":%lu,\"ringOverflow\":%lu,"
      "\"drv\":{\"valid\":%s,\"state\":%u,\"txErr\":%lu,\"rxErr\":%lu,\"busErr\":%lu}}",
      static_cast<unsigned long>(millis()),
      static_cast<unsigned long>(ESP.getFreeHeap()),
      (WiFi.isConnected() ? WiFi.localIP() : WiFi.softAPIP()).toString().c_str(),
      g_canUp ? "true" : "false",
      static_cast<unsigned>(g_display.syncState()),
      g_display.synced() ? "true" : "false",
      g_display.registered() ? "true" : "false",
      g_display.supports(affa::Feature::Text)  ? "true" : "false",
      g_display.supports(affa::Feature::Time)  ? "true" : "false",
      g_display.supports(affa::Feature::Power) ? "true" : "false",
      static_cast<unsigned long>(s.rxFrames), static_cast<unsigned long>(s.txFrames),
      static_cast<unsigned long>(s.ringOverflow),
      d.valid ? "true" : "false", static_cast<unsigned>(d.state),
      static_cast<unsigned long>(d.txErr), static_cast<unsigned long>(d.rxErr),
      static_cast<unsigned long>(d.busErr));
    return r->reply(200, "application/json", b);
  });
  g_server.on("/api/text", HTTP_GET, [](PsychicRequest* r) {
    return post(r, "text", qs(r, "t", "AFFA OK").c_str(), 0, 0);
  });
  g_server.on("/api/time", HTTP_GET, [](PsychicRequest* r) {
    return post(r, "time", "", qs(r, "h", "10").toInt(), qs(r, "m", "0").toInt());
  });
  g_server.on("/api/power", HTTP_GET, [](PsychicRequest* r) {
    return post(r, "power", "", qs(r, "on", "1").toInt(), 0);
  });
  g_server.on("/api/reboot", HTTP_GET, [](PsychicRequest* r) {
    r->reply(200, "application/json", "{\"reboot\":true}");
    delay(200);
    ESP.restart();
    return ESP_OK;
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

  // CAN BEFORE WIFI. The panel is universal and picks its family at ITS power-up from the
  // radio it can hear; both share a supply, so fifteen seconds of WiFi join is fifteen
  // seconds during which it hears no radio at all. Safe because Esp32CanLink::begin() is
  // BOUNDED — it installs a driver, reads a status word and returns.
  g_canUp = g_hw.begin(kPins, kBitrate, /*forceRecoveryMs=*/250);
  g_display.begin();
  for (uint8_t i = 0; i < 40; ++i) { g_display.poll(); delay(5); }

  (void)g_display.setPower(true);          // 0x151 `03 52 09 FF FF`
  (void)g_display.setText("AFFA WEB");
  for (uint8_t i = 0; i < 40; ++i) { g_display.poll(); delay(5); }

  startNetwork();
  g_server.config.max_uri_handlers = 16;
  g_server.listen(80);
  routes();
  // An OTA write stalls CAN reception (the TWAI ISR is not in IRAM), so gate our own
  // transmitter off rather than shout at a bus we cannot hear. Expect a resync afterwards.
  ElegantOTA.onStart([]() { g_hw.setTxEnabled(false); });
  ElegantOTA.begin(&g_server);

  if (!g_canUp) Serial.println("[can ] begin() failed — network is up for OTA");
  Serial.println("[carminat] web UI on /, OTA on /update");
}

void loop() {
  g_display.poll();   // also the keep-alive: stop polling and the panel drops the link
  runCmd();
  ElegantOTA.loop();
}
