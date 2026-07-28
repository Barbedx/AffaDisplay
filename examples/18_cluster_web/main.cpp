// 18_cluster_web — the dashboard cluster: the clock it DOES have, and the text nobody knows.
//
// The THIRD of three sibling examples (16 Carminat, 17 UpdateList, 18 cluster). Same shape,
// same endpoints — and the honest inversion of 16:
//
//   16 Carminat    setText ✅ seen on glass   setTime ✅ seen on glass
//   17 UpdateList  setText ✅ seen on glass   no clock command exists in the family
//   18 cluster     setText ❌ ENCODING UNKNOWN   clock: raw `3EF A6 hh mm`, from ITS OWN capture
//
// NOTHING IN THIS FILE HAS EVER BEEN RUN AGAINST HARDWARE. src/cluster/ is transcribed from
// ONE capture of an OEM radio talking to an instrument cluster (docs/PROTOCOL-NOTES.md §9,
// supplied 2026-07-28); no cluster was on the bench. Everything below is either quoted from
// that capture, or marked [GUESS] where it is not. Read /api/frames before believing any of
// it: on an unfamiliar panel the wire is the only witness.
//
// WHAT THE LIBRARY CAN ALREADY DO HERE, with no new code — only a header of data:
//   * the handshake, with a THIRD sync byte pair (0x59/0x5A) on Carminat's id 0x3AF,
//   * the lazy 0x70 registration walk on 0x121 and 0x1B1, ACKed on 0x521 / 0x5B1,
//   * answering the cluster's own registration of 0x1C1 through the generic auto-ACK,
//   * setPower — `1B1 03 52 <02|00> 00`, where only the OFF value is in the capture.
//
// THE CLOCK IS NOT A RENDER CALL AND IS NOT ON THE DISPLAY CLASS. `3EF A6 <hh> <mm>` is
// three bytes at DLC 3 with no PCI and no SF_DL — it is not an AFFA message at all, so
// enqueue() would frame it and corrupt it. It goes out through ICanLink::send(), raw. This
// is the family the frame was captured on, so of the three panels this is the one where it
// is expected to work; on the UpdateList panel it was tried and failed (BENCH-VERIFIED).
//
// THE TEXT ENCODING IS GENUINELY UNKNOWN. The capture contains no text frame — the radio
// never draws in the sample — so ClusterDisplay::setText() returns NotSupported and
// supports(Feature::Text) is FALSE. A capability lie there would be worse than a missing
// feature. What this example adds instead is /api/probe: the two text encodings we DO know,
// put on the cluster's registered text id 0x121, so somebody with the hardware can find out
// in a minute instead of guessing. Each is a [GUESS] and is labelled as one in the UI.
//
// Endpoints:
//   GET /                       the UI
//   GET /api/status             sync, registration, driver and controller counters
//   GET /api/text?t=HELLO       setText — returns NotSupported. That is the honest answer
//   GET /api/probe?form=N&t=..  candidate text encodings on 0x121. N: 1 Carminat, 2 LCD,
//                               3 segment. ALL [GUESS]
//   GET /api/time?h=12&m=03     raw `3EF A6 hh mm`, exactly as captured
//   GET /api/power?on=0|1       `1B1 03 52 02|00 00` — the ON value is inferred, not seen
//   GET /api/frame?id=&hex=     any raw frame, no framing applied
//   GET /api/frames             the last 48 frames, both directions — READ THIS FIRST
//   GET /api/reboot
//   GET /update                 ElegantOTA

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <PsychicHttp.h>
#include <ElegantOTA.h>
#include <AffaDisplay.h>

#if !AFFA_PANEL_CLUSTER
#  error "18_cluster_web needs -D AFFA_PANEL_CLUSTER=1"
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
constexpr const char* kMdnsName     = "affacluster";
constexpr uint32_t    kStaJoinMs    = 15000;

affa::Esp32CanLink   g_hw;
ArduinoClock         g_clock;
affa::ClusterDisplay g_display(g_hw, g_clock);
PsychicHttpServer    g_server;

bool g_canUp = false;

// ---------------------------------------------------------------------------
// Frame ring — Layer 0. On this panel it is not a debugging aid, it is the experiment.
// ---------------------------------------------------------------------------
// 48 frames, direction stamped, pushed from the tap. Whether the cluster answers 0x3AF at
// all, which filler it pads with, whether it registers 0x1C1 to us — none of that is known
// from one capture, and all of it is visible here.
struct Rec { uint32_t ms; uint8_t dir; uint16_t id; uint8_t d[8]; };
constexpr uint8_t kRing = 48;
Rec     g_ring[kRing];
uint8_t g_head = 0;

void onTap(const affa::Frame& f, affa::Direction dir, void*) {
  Rec& r = g_ring[g_head];
  r.ms  = millis();
  r.dir = (dir == affa::Direction::Tx) ? 2 : 1;
  r.id  = static_cast<uint16_t>(f.id);
  for (uint8_t i = 0; i < 8; ++i) r.d[i] = f.data[i];
  g_head = static_cast<uint8_t>((g_head + 1) % kRing);
}

// ---------------------------------------------------------------------------
// Candidate text encodings — every one of them a [GUESS]
// ---------------------------------------------------------------------------
// The cluster registers 0x121 as a function, so it expects SOMETHING there; the two
// families we can build put different bytes on that id. These are those bytes, unchanged,
// with nothing invented. If one of them draws, the encoding is settled and ClusterDisplay
// gets a real setText(); if none does, the ring shows whether they were even ACKed, which
// is a different and equally useful answer.
constexpr uint8_t kFormCarminat = 1;   // 0x151's `10 0E 77 55 55 FF 60 01 <14>`  (WIRE-SPEC §8.1)
constexpr uint8_t kFormLcd      = 2;   // 0x121's `10 1C 7F 55 55 FF 60 03 ..`    (§9.2)
constexpr uint8_t kFormSegment  = 3;   // 0x121's `10 19 76 7A 01 ..`             (§9.1)

void fillCells(const char* src, uint8_t* dst, uint8_t cells) {
  uint8_t i = 0;
  if (src) while (i < cells && src[i]) { dst[i] = static_cast<uint8_t>(src[i]); ++i; }
  while (i < cells) dst[i++] = 0x00;    // NUL, as in every golden vector of both families
}

affa::Result probeText(uint8_t form, const char* text) {
  char t[AFFA_TEXT_MAX];
  affa::toAscii(text, t, sizeof(t));    // UTF-8 on the wire is garbage on the glass

  uint8_t d[32];
  uint8_t n = 0;
  switch (form) {
    case kFormCarminat:
      d[n++] = 0x10; d[n++] = 0x0E; d[n++] = 0x77;
      d[n++] = 0x55; d[n++] = 0x55; d[n++] = 0xFF; d[n++] = 0x60; d[n++] = 0x01;
      fillCells(t, &d[n], 14); n = static_cast<uint8_t>(n + 14);
      break;
    case kFormLcd:
      d[n++] = 0x10; d[n++] = 0x1C; d[n++] = 0x7F;
      d[n++] = 0x55; d[n++] = 0x55; d[n++] = 0xFF; d[n++] = 0x60; d[n++] = 0x03;
      fillCells(t, &d[n], 8);  n = static_cast<uint8_t>(n + 8);
      d[n++] = 0x10;
      fillCells(t, &d[n], 12); n = static_cast<uint8_t>(n + 12);
      d[n++] = 0x00;
      break;
    case kFormSegment:
      d[n++] = 0x10; d[n++] = 0x19; d[n++] = 0x76; d[n++] = 0x7A; d[n++] = 0x01;
      fillCells(t, &d[n], 8);  n = static_cast<uint8_t>(n + 8);
      d[n++] = 0x10;
      fillCells(t, &d[n], 12); n = static_cast<uint8_t>(n + 12);
      d[n++] = 0x00; d[n++] = 0x81; d[n++] = 0x81;
      break;
    default:
      return affa::Result::BadArgument;
  }

  affa::TxOptions opt;
  opt.slot     = affa::RenderSlot::None;   // three different shapes; never coalesce them
  opt.coalesce = false;
  return g_display.enqueue(affa::cluster::kIdSetText, d, n, opt) == affa::kNoTicket
             ? g_display.lastResult()
             : affa::Result::Ok;
}

// ---------------------------------------------------------------------------
// Command mailbox — the library only ever sees the loop task
// ---------------------------------------------------------------------------
struct Cmd {
  volatile bool pending = false;
  char op[10]   = {0};
  char s1[32]   = {0};
  long a = 0, b = 0, c = 0;
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
    // NotSupported, and the endpoint exists to say so out loud rather than to pretend.
    r = g_display.setText(g_cmd.s1);
  } else if (!strcmp(g_cmd.op, "probe")) {
    r = probeText(static_cast<uint8_t>(g_cmd.a), g_cmd.s1);
  } else if (!strcmp(g_cmd.op, "time")) {
    // `3EF A6 <hh> <mm>`, DLC 3, both operands plain binary — the capture's own bytes.
    // Raw, because there is no PCI byte in it and the transport must not add one.
    affa::Frame f{};
    f.id  = affa::cluster::kIdClock;
    f.len = 3;
    f.data[0] = affa::cluster::kClockCmd;
    f.data[1] = static_cast<uint8_t>(g_cmd.a);
    f.data[2] = static_cast<uint8_t>(g_cmd.b);
    r = g_hw.send(f) ? affa::Result::Ok : affa::Result::SendFailed;
  } else if (!strcmp(g_cmd.op, "power")) {
    r = g_display.setPower(g_cmd.a != 0);
  } else if (!strcmp(g_cmd.op, "frame")) {
    affa::Frame f{};
    f.id  = static_cast<uint32_t>(g_cmd.a);
    f.len = static_cast<uint8_t>(g_cmd.b ? g_cmd.b : 8);
    for (uint8_t i = 0; i < 8; ++i) f.data[i] = static_cast<uint8_t>(g_cmd.s1[i]);
    r = g_hw.send(f) ? affa::Result::Ok : affa::Result::SendFailed;
  }

  snprintf(g_cmd.reply, sizeof(g_cmd.reply),
           "{\"op\":\"%s\",\"result\":\"%s\",\"registered\":%s}", g_cmd.op, resultName(r),
           g_display.registered() ? "true" : "false");
  g_cmd.pending = false;
}

esp_err_t post(PsychicRequest* req, const char* op, const char* s1, long a, long b, long c) {
  strncpy(g_cmd.op, op, sizeof(g_cmd.op) - 1);
  memset(g_cmd.s1, 0, sizeof(g_cmd.s1));
  if (s1) strncpy(g_cmd.s1, s1, sizeof(g_cmd.s1) - 1);
  g_cmd.a = a; g_cmd.b = b; g_cmd.c = c;
  g_cmd.reply[0] = '\0';
  g_cmd.pending  = true;
  const uint32_t t0 = millis();
  while (g_cmd.pending && millis() - t0 < 4000) delay(5);
  return req->reply(200, "application/json",
                    g_cmd.reply[0] ? g_cmd.reply : "{\"error\":\"timeout\"}");
}

String qs(PsychicRequest* r, const char* k, const char* dflt = "") {
  return r->hasParam(k) ? r->getParam(k)->value() : String(dflt);
}

const char kPage[] PROGMEM = R"HTML(<!doctype html><meta name=viewport content="width=device-width,initial-scale=1">
<title>Cluster</title><style>
body{font:15px system-ui;margin:0;padding:16px;background:#111;color:#eee}
fieldset{border:1px solid #333;border-radius:8px;margin:0 0 14px;padding:10px}
legend{color:#7cf;padding:0 6px}input,button{font:14px system-ui;padding:7px;margin:3px 0;border-radius:6px;border:1px solid #444;background:#1c1c1c;color:#eee;box-sizing:border-box}
input{width:100%}button{background:#2a4;border:0;color:#fff;cursor:pointer;width:100%;padding:9px;font-weight:600}
button.alt{background:#357}button.warn{background:#a53}small{color:#999}
#log,#ring{white-space:pre-wrap;font:12px ui-monospace;color:#9c9;margin-top:10px}
#st{font:12px ui-monospace;color:#9ac}.note{color:#e96}
</style>
<h1 style="font-size:17px">Dashboard cluster &mdash; UNTESTED HARDWARE</h1>
<p class=note><small>Everything here is transcribed from ONE capture and has never been run
against a cluster. Read the frame ring at the bottom before believing any button.</small></p>
<div id=st>&hellip;</div>
<fieldset><legend>Clock &nbsp;<small>3EF A6 hh mm, DLC 3, raw &mdash; captured on THIS family</small></legend>
<input id=h type=number min=0 max=23 value=12 style="width:5em"> :
<input id=m type=number min=0 max=59 value=3 style="width:5em">
<button onclick="go('time','h='+h.value+'&m='+m.value)">Set clock</button></fieldset>
<fieldset><legend>Text &nbsp;<small>the capture has NO text frame &mdash; the encoding is unknown</small></legend>
<input id=t maxlength=14 value="AFFA OK">
<button class=warn onclick="go('text','t='+e(t.value))">setText &rarr; NotSupported</button>
<p><small>Candidate encodings on the cluster's registered text id 0x121. All three are
[GUESS]; if one draws, we have the answer.</small></p>
<button class=alt onclick="go('probe','f=1&t='+e(t.value))">Probe 1 &mdash; Carminat `10 0E 77 ..`</button>
<button class=alt onclick="go('probe','f=2&t='+e(t.value))">Probe 2 &mdash; UpdateList LCD `10 1C 7F ..`</button>
<button class=alt onclick="go('probe','f=3&t='+e(t.value))">Probe 3 &mdash; UpdateList segment `10 19 76 ..`</button></fieldset>
<fieldset><legend>Display power &nbsp;<small>OFF is captured; ON (0x02) is inferred from the other families</small></legend>
<button class=alt onclick="go('power','on=1')">Power ON</button>
<button class=alt onclick="go('power','on=0')">Power OFF</button></fieldset>
<fieldset><legend>Raw frame &nbsp;<small>no ISO-TP framing at all</small></legend>
<input id=rid value="1007" style="width:7em" title="CAN id, decimal"> id(dec)
<input id=rhex value="A60C03" placeholder="hex bytes">
<button class=alt onclick="go('frame','id='+rid.value+'&hex='+rhex.value)">Send raw frame</button></fieldset>
<fieldset><legend>Frame ring &nbsp;<small>last 48, 1=rx 2=tx</small></legend>
<button class=alt onclick="ring()">Refresh</button><div id=ring></div></fieldset>
<p><a href="/update" style="color:#7cf">/update</a> &mdash; OTA</p>
<div id=log></div>
<script>
const e=encodeURIComponent;
async function go(op,q){const r=await fetch('/api/'+op+'?'+q);log.textContent=op+' -> '+await r.text();}
async function ring(){const j=await(await fetch('/api/frames')).json();
document.getElementById('ring').textContent=j.f.map(x=>x[0]+(x[1]==2?' TX ':' RX ')+x[2].toString(16).toUpperCase()+' '+x[3]).join('\n');}
setInterval(async()=>{try{const s=await(await fetch('/api/status')).json();
st.textContent='can='+s.canUp+' sync=0x'+s.syncState.toString(16)+' synced='+s.synced+
' registered='+s.registered+' rx='+s.rxFrames+' tx='+s.txFrames;}catch(x){st.textContent='offline';}},2000);
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
    return post(r, "text", qs(r, "t", "AFFA OK").c_str(), 0, 0, 0);
  });
  g_server.on("/api/probe", HTTP_GET, [](PsychicRequest* r) {
    return post(r, "probe", qs(r, "t", "AFFA OK").c_str(), qs(r, "f", "1").toInt(), 0, 0);
  });
  g_server.on("/api/time", HTTP_GET, [](PsychicRequest* r) {
    return post(r, "time", nullptr, qs(r, "h", "12").toInt(), qs(r, "m", "3").toInt(), 0);
  });
  g_server.on("/api/power", HTTP_GET, [](PsychicRequest* r) {
    return post(r, "power", nullptr, qs(r, "on", "1").toInt(), 0, 0);
  });
  g_server.on("/api/frame", HTTP_GET, [](PsychicRequest* r) {
    const String hex = qs(r, "hex");
    char    raw[9] = {0};
    uint8_t n = 0;
    for (size_t i = 0; i + 1 < hex.length() && n < 8; i += 2) {
      auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
      };
      const int hi = nib(hex[i]), lo = nib(hex[i + 1]);
      if (hi < 0 || lo < 0) break;
      raw[n++] = static_cast<char>((hi << 4) | lo);
    }
    if (n == 0) return r->reply(400, "application/json", "{\"error\":\"bad hex\"}");
    return post(r, "frame", raw, qs(r, "id", "1007").toInt(),
                qs(r, "dlc", "0").toInt() ? qs(r, "dlc").toInt() : n, 0);
  });
  g_server.on("/api/frames", HTTP_GET, [](PsychicRequest* r) {
    String  out  = "{\"f\":[";
    const uint8_t head = g_head;
    bool    first = true;
    for (uint8_t i = 0; i < kRing; ++i) {
      const Rec& e = g_ring[(head + i) % kRing];
      if (e.id == 0) continue;
      char b[96];
      snprintf(b, sizeof(b), "%s[%lu,%u,%u,\"%02X%02X%02X%02X%02X%02X%02X%02X\"]",
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
}

void startNetwork() {
  // READ-ONLY. An NVS write stops CAN reception outright — the TWAI ISR is not in IRAM.
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

  // CAN BEFORE WIFI: bounded, and it puts our handshake on the bus within milliseconds of
  // the cluster's own power-up rather than fifteen seconds later.
  g_canUp = g_hw.begin(kPins, kBitrate, /*forceRecoveryMs=*/250);
  g_display.onFrame(&onTap, nullptr);
  g_display.begin();
  for (uint8_t i = 0; i < 40; ++i) { g_display.poll(); delay(5); }

  // The ON value here is the least certain byte this library ships — 0x02, inferred from
  // the other two families; only `03 52 00 00` (OFF) is in the capture.
  (void)g_display.setPower(true);
  for (uint8_t i = 0; i < 40; ++i) { g_display.poll(); delay(5); }

  startNetwork();
  g_server.config.max_uri_handlers = 20;
  g_server.listen(80);
  routes();
  ElegantOTA.onStart([]() { g_hw.setTxEnabled(false); });
  ElegantOTA.begin(&g_server);

  if (!g_canUp) Serial.println("[can ] begin() failed — network is up for OTA");
  Serial.println("[cluster] web UI on /, OTA on /update — nothing here is bench-verified");
}

void loop() {
  g_display.poll();   // also the keep-alive
  runCmd();
  ElegantOTA.loop();
}
