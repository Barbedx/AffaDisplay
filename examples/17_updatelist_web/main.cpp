// 17_updatelist_web — UpdateList/AFFA2: text from a browser, and the clock that is NOT here.
//
// The SECOND of three sibling examples (16 Carminat, 17 UpdateList, 18 cluster) that do the
// same two things on each family. Same shape as 16, same endpoints, one difference — and it
// is the whole reason to read this file:
//
//   THIS FAMILY HAS NO CLOCK COMMAND. supports(Feature::Time) is false and setTime()
//   returns Result::NotSupported. /api/time is wired up anyway and returns exactly that,
//   because the extracted code this library replaced returned NoError from setTime() and
//   put nothing on the bus — a silent no-op is the failure mode the Result enum exists to
//   make impossible, and an endpoint that says NotSupported out loud is the demonstration.
//
// The panel DOES have a clock box; it just is not ours to set. It blinks while unset and
// counts up from power-on. The only candidate ever found is the OEM cluster's raw
// `3EF A6 <hh> <mm>` (PROTOCOL-NOTES §9.4), and on 2026-07-28 it was tried on this panel and
// DID NOT set the clock (BENCH-VERIFIED). /api/rawclock sends it so the result is
// reproducible, and it is labelled as the failed candidate it is. Seven further candidates
// are in examples/15_updatelist_modes as /api/sweep?n=2..8 and are still untried.
//
// THE LCD VARIANT. UpdateListMenuDisplay emits the `10 1C 7F ..` text-plus-icons form
// (WIRE-SPEC §9.2) at location 0x03; the 8-segment panel's `10 19 76 ..` form (§9.1) is
// UpdateListDisplay, and swapping the class is the only change needed. For the mode zoo —
// NORMAL vs FULLSCREEN vs MENU rows, built by hand — read examples/15_updatelist_modes.
//
// Endpoints:
//   GET /                       the UI
//   GET /api/status             link, sync, marquee and controller counters
//   GET /api/text?t=HELLO       setText (stops the marquee first — one owner per window)
//   GET /api/scroll?t=..&run=1  the marquee: a string and a play bit
//   GET /api/time?h=10&m=00     setTime — returns NotSupported, ON PURPOSE. See above
//   GET /api/rawclock?h=&m=     raw `3EF A6 hh mm`, no framing. Known to FAIL here
//   GET /api/power?on=0|1       setPower — MANDATORY before anything renders
//   GET /api/reboot
//   GET /update                 ElegantOTA

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <PsychicHttp.h>
#include <ElegantOTA.h>
#include <AffaDisplay.h>

#if !AFFA_PANEL_UPDATELIST_MENU || !AFFA_ENABLE_MARQUEE
#  error "17_updatelist_web needs AFFA_PANEL_UPDATELIST_MENU=1 and AFFA_ENABLE_MARQUEE=1"
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
constexpr const char* kMdnsName     = "affaupdatelist";
constexpr uint32_t    kStaJoinMs    = 15000;

affa::Esp32CanLink          g_hw;
ArduinoClock                g_clock;
affa::UpdateListMenuDisplay g_display(g_hw, g_clock);
PsychicHttpServer           g_server;

bool g_canUp = false;

// ---------------------------------------------------------------------------
// Command mailbox — the library only ever sees the loop task
// ---------------------------------------------------------------------------
struct Cmd {
  volatile bool pending = false;
  char op[10]   = {0};
  char s1[48]   = {0};
  long a = 0, b = 0;
  char reply[112] = {0};
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
    // One owner per window: a static render under a running marquee is overwritten on the
    // next scroll step, which looks like "setText did nothing" and is not.
    g_display.setScrollActive(false);
    r = g_display.setText(g_cmd.s1);
  } else if (!strcmp(g_cmd.op, "scroll")) {
    if (g_cmd.s1[0]) g_display.setScrollText(g_cmd.s1);
    g_display.setScrollActive(g_cmd.a != 0);
  } else if (!strcmp(g_cmd.op, "time")) {
    // NotSupported, and that is the point of the endpoint. Nothing goes on the wire.
    char hhmm[5];
    snprintf(hhmm, sizeof(hhmm), "%02ld%02ld", g_cmd.a % 24, g_cmd.b % 60);
    r = g_display.setTime(hhmm);
  } else if (!strcmp(g_cmd.op, "rawclock")) {
    // A RAW frame, straight to the link. `3EF A6 <hh> <mm>` at DLC 3 has no PCI byte and no
    // SF_DL, so it is not an AFFA message at all — enqueue() would frame it and corrupt it.
    // ICanLink::send() is the escape hatch that exists for exactly this.
    //
    // KNOWN TO FAIL ON THIS PANEL (BENCH-VERIFIED, 2026-07-28): the bus accepts the frame
    // and the clock does not change. Kept so the negative result stays reproducible.
    affa::Frame f{};
    f.id  = 0x3EF;
    f.len = 3;
    f.data[0] = 0xA6;
    f.data[1] = static_cast<uint8_t>(g_cmd.a);
    f.data[2] = static_cast<uint8_t>(g_cmd.b);
    r = g_hw.send(f) ? affa::Result::Ok : affa::Result::SendFailed;
  } else if (!strcmp(g_cmd.op, "power")) {
    r = g_display.setPower(g_cmd.a != 0);
  }

  snprintf(g_cmd.reply, sizeof(g_cmd.reply),
           "{\"op\":\"%s\",\"result\":\"%s\",\"scrolling\":%s}",
           g_cmd.op, resultName(r), g_display.scrollActive() ? "true" : "false");
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
<title>UpdateList</title><style>
body{font:15px system-ui;margin:0;padding:16px;background:#111;color:#eee}
fieldset{border:1px solid #333;border-radius:8px;margin:0 0 14px;padding:10px}
legend{color:#7cf;padding:0 6px}input,button{font:14px system-ui;padding:7px;margin:3px 0;border-radius:6px;border:1px solid #444;background:#1c1c1c;color:#eee;box-sizing:border-box}
input{width:100%}button{background:#2a4;border:0;color:#fff;cursor:pointer;width:100%;padding:9px;font-weight:600}
button.alt{background:#357}button.warn{background:#a53}small{color:#999}
#log{white-space:pre-wrap;font:12px ui-monospace;color:#9c9;margin-top:10px;min-height:2em}
#st{font:12px ui-monospace;color:#9ac}
</style>
<h1 style="font-size:17px">UpdateList / AFFA2 &mdash; text (and the clock we do not have)</h1>
<div id=st>&hellip;</div>
<fieldset><legend>Text &nbsp;<small>0x121 `10 1C 7F ..`, LCD form, 12 visible cells</small></legend>
<input id=t maxlength=12 value="AFFA OK">
<button onclick="go('text','t='+e(t.value))">setText</button></fieldset>
<fieldset><legend>Running text &nbsp;<small>the marquee owns the window while it runs</small></legend>
<input id=s maxlength=40 value="AFFA2 UPDATE LIST - RUNNING TEXT">
<button onclick="go('scroll','run=1&t='+e(s.value))">Start</button>
<button class=alt onclick="go('scroll','run=0')">Stop</button></fieldset>
<fieldset><legend>Clock &nbsp;<small>THIS FAMILY HAS NO setTime &mdash; both buttons are expected to fail</small></legend>
<input id=h type=number min=0 max=23 value=10 style="width:5em"> :
<input id=m type=number min=0 max=59 value=0 style="width:5em">
<button class=warn onclick="go('time','h='+h.value+'&m='+m.value)">setTime &rarr; NotSupported</button>
<button class=warn onclick="go('rawclock','h='+h.value+'&m='+m.value)">raw 3EF A6 hh mm &rarr; failed on the bench</button>
<p><small>The panel blinks the clock while it is unset; a steady clock means something
worked. Candidates 2&ndash;8 (BCD, swapped operands, `05 56` on 0x121/0x1B1, DLC 8, 0x3DF,
cmd 0x26) live in examples/15_updatelist_modes as /api/sweep?n=</small></p></fieldset>
<fieldset><legend>Display power &nbsp;<small>MANDATORY: without it every frame is ACKed and none drawn</small></legend>
<button class=alt onclick="go('power','on=1')">Power ON</button>
<button class=alt onclick="go('power','on=0')">Power OFF</button></fieldset>
<p><a href="/update" style="color:#7cf">/update</a> &mdash; OTA</p>
<div id=log></div>
<script>
const e=encodeURIComponent;
async function go(op,q){const r=await fetch('/api/'+op+'?'+q);log.textContent=op+' -> '+await r.text();}
setInterval(async()=>{try{const s=await(await fetch('/api/status')).json();
st.textContent='can='+s.canUp+' sync=0x'+s.syncState.toString(16)+' registered='+s.registered+
' scroll='+s.scrolling+' rx='+s.rxFrames+' tx='+s.txFrames;}catch(x){st.textContent='offline';}},2000);
</script>)HTML";

void routes() {
  g_server.on("/", HTTP_GET, [](PsychicRequest* r) {
    return r->reply(200, "text/html", kPage);
  });
  g_server.on("/api/status", HTTP_GET, [](PsychicRequest* r) {
    const affa::Stats s = g_display.stats();
    const auto        d = g_hw.driverState();
    char b[560];
    snprintf(b, sizeof(b),
      "{\"uptimeMs\":%lu,\"heap\":%lu,\"ip\":\"%s\",\"canUp\":%s,"
      "\"syncState\":%u,\"synced\":%s,\"registered\":%s,\"scrolling\":%s,"
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
      g_display.scrollActive() ? "true" : "false",
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
  g_server.on("/api/scroll", HTTP_GET, [](PsychicRequest* r) {
    return post(r, "scroll", qs(r, "t").c_str(), qs(r, "run", "1").toInt(), 0);
  });
  g_server.on("/api/time", HTTP_GET, [](PsychicRequest* r) {
    return post(r, "time", "", qs(r, "h", "10").toInt(), qs(r, "m", "0").toInt());
  });
  g_server.on("/api/rawclock", HTTP_GET, [](PsychicRequest* r) {
    return post(r, "rawclock", "", qs(r, "h", "10").toInt(), qs(r, "m", "0").toInt());
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

  Serial.printf("\n[wifi] %s ip=%s  OTA http://%s/update\n", sta ? "STA" : "AP",
                (sta ? WiFi.localIP() : WiFi.softAPIP()).toString().c_str(),
                (sta ? WiFi.localIP() : WiFi.softAPIP()).toString().c_str());
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);

  // CAN BEFORE WIFI, and on this family it is load-bearing rather than tidy: the panel is
  // UNIVERSAL and decides at ITS power-up which radio it is hearing. Fifteen seconds of
  // WiFi join is fifteen seconds of silence, by the end of which it has settled into its
  // default family and every AFFA2 frame afterwards is ACKed and not rendered.
  g_canUp = g_hw.begin(kPins, kBitrate, /*forceRecoveryMs=*/250);
  g_display.begin();
  for (uint8_t i = 0; i < 40; ++i) { g_display.poll(); delay(5); }

  // MANDATORY. `0x1B1 04 52 02 FF FF` (WIRE-SPEC §9.3). Without it the panel acknowledges
  // every frame and renders none of them — a healthy link drawing to a dark screen.
  (void)g_display.setPower(true);
  (void)g_display.setText("AFFA WEB");
  for (uint8_t i = 0; i < 40; ++i) { g_display.poll(); delay(5); }

  startNetwork();
  g_server.config.max_uri_handlers = 16;
  g_server.listen(80);
  routes();
  ElegantOTA.onStart([]() { g_hw.setTxEnabled(false); });
  ElegantOTA.begin(&g_server);

  if (!g_canUp) Serial.println("[can ] begin() failed — network is up for OTA");
  Serial.println("[updatelist] web UI on /, OTA on /update");
}

void loop() {
  // poll() is also the KEEP-ALIVE. UpdateList drops the link if the 0x3DF heartbeat stops,
  // so a loop that blocks does not merely delay a render — it disconnects the display.
  g_display.poll();
  runCmd();
  ElegantOTA.loop();
}
