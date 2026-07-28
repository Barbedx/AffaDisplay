// 15_updatelist_modes — the three things an AFFA2 LCD can actually draw, with a web UI.
//
//   NORMAL      one line of text, the panel's own clock box, and the icon row
//   FULLSCREEN  free text across the whole glass, 8 characters per addressed row
//   MENU        up to 8 list rows with one selected (inverted) and a scrollbar
//
// WHY THIS EXAMPLE EXISTS. The library models the AFFA2 text encodings as two PANEL
// CLASSES — UpdateListDisplay emitting `10 19 76 ..` and UpdateListMenuDisplay emitting
// `10 1C 7F ..` — and that is a misreading of the reference implementation. Those two forms
// are not two panels. They are the SAME message to the SAME display, and the difference is
// purely whether the icon header is being resent:
//
//     if (icons != old_icons || mode != old_mode)  ->  1C 7F <icons> 55 <mode>   (long)
//     else                                         ->  19 76                     (short)
//
// [REF] MeganeCAN/notes/archive_mhroczny/affa3.c, affa3_do_set_text(). Sending the long
// form every time is always correct and three bytes dearer, which is what this file does.
//
// WHAT ACTUALLY SELECTS THE MODE is the LOCATION byte, and the library hard-codes it:
//
//     AFFA3_LOCATION(max,idx) = ((max & 7) << 5) | ((idx & 7) << 2)
//     | SELECTED   0x01        the row is highlighted (inverted on the glass)
//     | FULLSCREEN 0x02        take the whole screen rather than the text line
//
//   NORMAL       loc = SELECTED                                    = 0x01
//   FULLSCREEN   loc = LOCATION(packets-1, i) | SELECTED | FULLSCREEN
//   MENU row     loc = LOCATION(max-1, idx)   | (selected ? SELECTED : 0)
//
// So FULLSCREEN gets more than one line by sending ONE MESSAGE PER 8-CHARACTER CHUNK, each
// addressed as row i of n. That is the whole trick, and it is why the display looked like
// "8 characters, one row" until you address the rows.
//
// A pleasing cross-check fell out of it: a 3-item menu computes rows 0x41, 0x44, 0x48 —
// byte for byte the Carminat info-popup row slots already in carminat::kInfoOffset0/1/2.
// The two families share this row addressing, which is corroboration nobody was looking for.
//
// PADDING IS SPACES HERE, NOT NUL. The reference pads both fields with ' ' and this is the
// form the panel was photographed rendering. WIRE-SPEC §9.1/§9.2 record NUL from captures
// of a different sender; do not "unify" them without a panel in front of you.

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <PsychicHttp.h>
#include <ElegantOTA.h>
#include <AffaDisplay.h>

#if !AFFA_PANEL_UPDATELIST
#  error "15_updatelist_modes needs AFFA_PANEL_UPDATELIST=1"
#endif

namespace {

struct ArduinoClock final : affa::IClock {
  uint32_t millis() const override { return ::millis(); }
};

constexpr affa::CanPins kPins{ .rx = GPIO_NUM_4, .tx = GPIO_NUM_3 };
constexpr uint32_t      kBitrate = 500000;

constexpr const char* kNvsNamespace = "megaopen";
constexpr const char* kApSsid       = "AffaBench";
constexpr const char* kApPass       = "affabench";
constexpr const char* kMdnsName     = "affamodes";
constexpr uint32_t    kStaJoinMs    = 15000;

// ---- the AFFA2 text payload, one builder for all three modes -----------------
constexpr uint8_t kIconsNone   = 0x55;  // NO_NEWS|NO_TRAFFIC|NO_AFRDS|NO_MODE
constexpr uint8_t kIconSep     = 0x55;  // literal, not a second icon set
constexpr uint8_t kIconModeOff = 0xFF;  // AFFA3_ICON_MODE_NONE
constexpr uint8_t kLocSelected   = 0x01;
constexpr uint8_t kLocFullscreen = 0x02;
constexpr uint8_t kOldCells = 8, kNewCells = 12, kPayload = 30;

inline uint8_t affaLocation(uint8_t maxIdx, uint8_t idx) {
  return static_cast<uint8_t>(((maxIdx & 7) << 5) | ((idx & 7) << 2));
}

affa::Esp32CanLink       g_hw;
ArduinoClock             g_clock;
affa::UpdateListDisplay  g_display(g_hw, g_clock);
PsychicHttpServer        g_server;
bool                     g_canUp = false;

// THE ICON HEADER IS A DIFFERENTIAL AND IT IS NOT OPTIONAL TO GET RIGHT.
//
// The reference resends `1C 7F <icons> 55 <mode>` only when icons or mode CHANGED, and uses
// the short `19 76` otherwise. This file originally sent the long form every time on the
// reasoning that it is "always correct and three bytes dearer". Measured on a real panel,
// that is WRONG: fullscreen text rendered a single row instead of three, because each
// message carrying the icon header appears to start a NEW screen and discards the rows
// addressed before it. The differential is load-bearing, not an optimisation.
uint8_t g_lastIcons = 0xFF;   // deliberately not kIconsNone, so the first call sends long
uint8_t g_lastMode  = 0x00;

void resetIconState() { g_lastIcons = 0xFF; g_lastMode = 0x00; }

// Build and enqueue either
//   long : 10 1C 7F <icons> 55 <mode> <0x60|chan> <loc> old[8] 10 new[12] 00   (30 bytes)
//   short: 10 19 76                  <0x60|chan> <loc> old[8] 10 new[12] 00   (27 bytes)
affa::Result sendText(uint8_t icons, uint8_t mode, uint8_t chan, uint8_t loc,
                      const char* oldStr, const char* newStr) {
  uint8_t d[kPayload];
  uint8_t n = 0;
  d[n++] = 0x10;
  if (icons != g_lastIcons || mode != g_lastMode) {
    d[n++] = 0x1C;
    d[n++] = 0x7F;
    d[n++] = icons;
    d[n++] = kIconSep;
    d[n++] = mode;
    g_lastIcons = icons;
    g_lastMode  = mode;
  } else {
    d[n++] = 0x19;
    d[n++] = 0x76;
  }
  d[n++] = static_cast<uint8_t>(0x60 | (chan & 7));
  d[n++] = loc;
  for (uint8_t i = 0; i < kOldCells; ++i)
    d[n++] = (oldStr && oldStr[i]) ? static_cast<uint8_t>(oldStr[i]) : ' ';
  d[n++] = 0x10;                                    // separator
  for (uint8_t i = 0; i < kNewCells; ++i)
    d[n++] = (newStr && newStr[i]) ? static_cast<uint8_t>(newStr[i]) : ' ';
  d[n++] = 0x00;                                    // terminator

  affa::TxOptions opt;
  opt.slot     = affa::RenderSlot::None;   // each row is its own message; never coalesce
  opt.coalesce = false;
  return g_display.enqueue(affa::updatelist::kIdSetText, d, n, opt) == affa::kNoTicket
             ? g_display.lastResult()
             : affa::Result::Ok;
}

// One line plus the panel's clock box and icon row.
affa::Result showNormal(const char* text, uint8_t icons, uint8_t mode, uint8_t chan) {
  return sendText(icons, mode, chan, kLocSelected, text, text);
}

// Free text across the whole glass. ONE MESSAGE PER 8 CHARACTERS, each addressed as row i
// of n — that is how it gets more than one line. Max 7 rows (the field is 3 bits).
affa::Result showFullscreen(const char* text) {
  const size_t len = strlen(text);
  uint8_t packets = static_cast<uint8_t>(len / 8 + ((len % 8) ? 1 : 0));
  if (packets == 0) packets = 1;
  if (packets > 7)  return affa::Result::TooLong;

  // Force the FIRST chunk to carry the icon header and every later one to use the short
  // form — which is what makes the rows accumulate into one screen rather than each
  // replacing the last.
  resetIconState();

  for (uint8_t i = 0; i < packets; ++i) {
    char chunk[kNewCells + 1];
    for (uint8_t j = 0; j < kNewCells; ++j) {
      const size_t k = static_cast<size_t>(i) * 8 + j;
      chunk[j] = (j < 8 && k < len) ? text[k] : ' ';
    }
    chunk[kNewCells] = '\0';
    const uint8_t loc = static_cast<uint8_t>(affaLocation(packets - 1, i) |
                                             kLocSelected | kLocFullscreen);
    const affa::Result r = sendText(kIconsNone, kIconModeOff, 0, loc, chunk, chunk);
    if (r != affa::Result::Ok) return r;
  }
  return affa::Result::Ok;
}

// A list row. `count` is how many rows the menu has; `idx` which this is; `selected`
// inverts it. Rows of a 3-item menu land on 0x41 / 0x44 / 0x48.
affa::Result showMenuItem(uint8_t count, uint8_t idx, bool selected, const char* text) {
  if (count == 0 || count > 8 || idx >= count) return affa::Result::BadArgument;
  uint8_t loc = affaLocation(static_cast<uint8_t>(count - 1), idx);
  if (selected) loc |= kLocSelected;
  return sendText(kIconsNone, kIconModeOff, 0, loc, text, text);
}

// ---------------------------------------------------------------------------
// Command mailbox — the library only ever sees the loop task
// ---------------------------------------------------------------------------
struct Cmd {
  volatile bool pending = false;
  char op[10] = {0};
  char s[4][20] = {{0}};
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

  if (!strcmp(g_cmd.op, "normal")) {
    r = showNormal(g_cmd.s[0], static_cast<uint8_t>(g_cmd.a),
                   static_cast<uint8_t>(g_cmd.b), static_cast<uint8_t>(g_cmd.c));
  } else if (!strcmp(g_cmd.op, "full")) {
    r = showFullscreen(g_cmd.s[0]);
  } else if (!strcmp(g_cmd.op, "menu")) {
    const uint8_t n = static_cast<uint8_t>(g_cmd.a ? g_cmd.a : 3);
    for (uint8_t i = 0; i < n && r == affa::Result::Ok; ++i)
      r = showMenuItem(n, i, i == static_cast<uint8_t>(g_cmd.b), g_cmd.s[i]);
  } else if (!strcmp(g_cmd.op, "power")) {
    r = g_display.setPower(g_cmd.a != 0);
  } else if (!strcmp(g_cmd.op, "frame")) {
    // A RAW frame, straight to the link — no ISO-TP, no SF_DL, no registration, no ACK
    // matching. Needed because not everything on this bus is an AFFA message: the OEM
    // cluster's time command is `3EF A6 <hh> <mm>` at DLC 3 with no PCI byte at all, so
    // enqueue() would frame it and corrupt it. Runs on the loop task like every other
    // command, so the one-task rule still holds.
    affa::Frame f{};
    f.id  = static_cast<uint32_t>(g_cmd.a);
    f.len = static_cast<uint8_t>(g_cmd.b ? g_cmd.b : 8);
    for (uint8_t i = 0; i < 8; ++i) f.data[i] = static_cast<uint8_t>(g_cmd.s[0][i]);
    r = g_hw.send(f) ? affa::Result::Ok : affa::Result::SendFailed;
  } else if (!strcmp(g_cmd.op, "sweep")) {
    // CLOCK BRUTE FORCE, made identifiable rather than blind. Each candidate sets a
    // DIFFERENT hour equal to its own index, so whatever the panel ends up displaying
    // names the encoding that worked — no need to watch during the sweep. Minutes are
    // fixed at 34 so a half-working candidate (hour only) is still distinguishable.
    //
    // Candidates, in order:
    //   1  3EF  A6 hh mm            the cluster capture, binary          (already failed)
    //   2  3EF  A6 BCD(hh) BCD(mm)  same frame, BCD instead of binary
    //   3  3EF  A6 mm hh            same frame, operands swapped
    //   4  121  05 56 'h','h','m','m'  Carminat's setTime shape on the UpdateList text id
    //   5  1B1  05 56 'h','h','m','m'  ditto on the control id
    //   6  3EF  A6 hh mm 00 00 00 00   padded to DLC 8 rather than 3
    //   7  3DF  A6 hh mm            our own sync id instead of 3EF
    //   8  3EF  26 hh mm            command byte without the top bit
    const uint8_t idx = static_cast<uint8_t>(g_cmd.a);
    const uint8_t hh  = idx;                 // hour == candidate number
    const uint8_t mm  = 34;
    auto bcd = [](uint8_t v) -> uint8_t {
      return static_cast<uint8_t>(((v / 10) << 4) | (v % 10));
    };
    affa::Frame f{};
    bool useEnqueue = false;
    uint8_t payload[8]; uint8_t plen = 0; uint16_t pid = 0;

    switch (idx) {
      case 1: f.id=0x3EF; f.len=3; f.data[0]=0xA6; f.data[1]=hh;      f.data[2]=mm;      break;
      case 2: f.id=0x3EF; f.len=3; f.data[0]=0xA6; f.data[1]=bcd(hh); f.data[2]=bcd(mm); break;
      case 3: f.id=0x3EF; f.len=3; f.data[0]=0xA6; f.data[1]=mm;      f.data[2]=hh;      break;
      case 4: case 5: {
        pid = (idx == 4) ? 0x121 : 0x1B1;
        payload[plen++] = 0x05; payload[plen++] = 0x56;
        payload[plen++] = static_cast<uint8_t>('0' + (hh / 10));
        payload[plen++] = static_cast<uint8_t>('0' + (hh % 10));
        payload[plen++] = static_cast<uint8_t>('0' + (mm / 10));
        payload[plen++] = static_cast<uint8_t>('0' + (mm % 10));
        useEnqueue = true;
        break;
      }
      case 6: f.id=0x3EF; f.len=8; f.data[0]=0xA6; f.data[1]=hh; f.data[2]=mm; break;
      case 7: f.id=0x3DF; f.len=3; f.data[0]=0xA6; f.data[1]=hh; f.data[2]=mm; break;
      case 8: f.id=0x3EF; f.len=3; f.data[0]=0x26; f.data[1]=hh; f.data[2]=mm; break;
      default: r = affa::Result::BadArgument; break;
    }

    if (r == affa::Result::Ok) {
      if (useEnqueue) {
        affa::TxOptions o; o.slot = affa::RenderSlot::None; o.coalesce = false;
        r = g_display.enqueue(pid, payload, plen, o) == affa::kNoTicket
                ? g_display.lastResult() : affa::Result::Ok;
      } else {
        r = g_hw.send(f) ? affa::Result::Ok : affa::Result::SendFailed;
      }
    }
  } else if (!strcmp(g_cmd.op, "time")) {
    // The candidate from the OEM cluster capture: 0x3EF, DLC 3, `A6 <hours> <minutes>`,
    // both plain binary. Unverified on our panel — that is what this endpoint is for.
    affa::Frame f{};
    f.id  = 0x3EF;
    f.len = 3;
    f.data[0] = 0xA6;
    f.data[1] = static_cast<uint8_t>(g_cmd.a);
    f.data[2] = static_cast<uint8_t>(g_cmd.b);
    r = g_hw.send(f) ? affa::Result::Ok : affa::Result::SendFailed;
  }

  snprintf(g_cmd.reply, sizeof(g_cmd.reply), "{\"op\":\"%s\",\"result\":\"%s\"}",
           g_cmd.op, resultName(r));
  g_cmd.pending = false;
}

String qs(PsychicRequest* r, const char* k, const char* d = "") {
  return r->hasParam(k) ? r->getParam(k)->value() : String(d);
}

esp_err_t post(PsychicRequest* req, const char* op,
               const char* s0, const char* s1, const char* s2, const char* s3,
               long a, long b, long c) {
  strncpy(g_cmd.op, op, sizeof(g_cmd.op) - 1);
  const char* src[4] = {s0, s1, s2, s3};
  for (uint8_t i = 0; i < 4; ++i) {
    memset(g_cmd.s[i], 0, sizeof(g_cmd.s[i]));
    if (src[i]) strncpy(g_cmd.s[i], src[i], sizeof(g_cmd.s[i]) - 1);
  }
  g_cmd.a = a; g_cmd.b = b; g_cmd.c = c;
  g_cmd.reply[0] = '\0';
  g_cmd.pending  = true;
  const uint32_t t0 = millis();
  while (g_cmd.pending && millis() - t0 < 4000) delay(5);
  return req->reply(200, "application/json",
                    g_cmd.reply[0] ? g_cmd.reply : "{\"error\":\"timeout\"}");
}

const char kPage[] PROGMEM = R"HTML(<!doctype html><meta name=viewport content="width=device-width,initial-scale=1">
<title>AFFA2 modes</title><style>
body{font:15px system-ui;margin:0;padding:16px;background:#111;color:#eee}
h2{font-size:15px;margin:18px 0 6px;color:#7cf}fieldset{border:1px solid #333;border-radius:8px;margin:0 0 14px;padding:10px}
legend{color:#7cf;padding:0 6px}input,button,select{font:14px system-ui;padding:7px;margin:3px 0;border-radius:6px;border:1px solid #444;background:#1c1c1c;color:#eee;box-sizing:border-box}
input{width:100%}button{background:#2a4;border:0;color:#fff;cursor:pointer;width:100%;padding:9px;font-weight:600}
button.alt{background:#357}#log{white-space:pre-wrap;font:12px ui-monospace;color:#9c9;margin-top:10px;min-height:2em}
</style>
<h1 style="font-size:17px">AFFA2 / UpdateList — three modes</h1>
<fieldset><legend>1 · NORMAL &nbsp;<small>loc 0x01 — text + panel clock</small></legend>
<input id=n1 maxlength=12 value="Pozdrawiam!">
<button onclick="go('normal','t='+e(n1.value))">Show normal</button></fieldset>
<fieldset><legend>2 · FULLSCREEN &nbsp;<small>loc LOCATION(n-1,i)|03 — 8 chars per row</small></legend>
<input id=f1 maxlength=56 value="Pozdrawiam #error i forum megane.com.pl">
<button onclick="go('full','t='+e(f1.value))">Show fullscreen</button></fieldset>
<fieldset><legend>3 · MENU &nbsp;<small>loc LOCATION(n-1,idx)|selected</small></legend>
<input id=m0 maxlength=12 value="MENU 1"><input id=m1 maxlength=12 value="MENU 2"><input id=m2 maxlength=12 value="MENU 3">
Selected row: <select id=ms><option value=0>1</option><option value=1>2</option><option value=2>3</option></select>
<button onclick="go('menu','n=3&sel='+ms.value+'&i0='+e(m0.value)+'&i1='+e(m1.value)+'&i2='+e(m2.value))">Show menu</button></fieldset>
<fieldset><legend>4 · CLOCK &nbsp;<small>3EF A6 hh mm — from an OEM cluster capture, UNVERIFIED</small></legend>
<input id=th type=number min=0 max=23 value=10 style="width:5em"> :
<input id=tm type=number min=0 max=59 value=0 style="width:5em">
<button onclick="go('time','h='+th.value+'&m='+tm.value)">Set clock</button></fieldset>
<fieldset><legend>Raw frame &nbsp;<small>no ISO-TP framing at all</small></legend>
<input id=rid value="1007" style="width:7em" title="CAN id, decimal"> id(dec)
<input id=rhex value="A60C03" placeholder="hex bytes">
<button class=alt onclick="go('frame','id='+rid.value+'&hex='+rhex.value)">Send raw frame</button></fieldset>
<fieldset><legend>Display power</legend>
<button class=alt onclick="go('power','on=1')">Power ON</button>
<button class=alt onclick="go('power','on=0')">Power OFF</button></fieldset>
<div id=log></div>
<script>
const e=encodeURIComponent;
async function go(op,q){const r=await fetch('/api/'+op+'?'+q);log.textContent=op+' -> '+await r.text();}
setInterval(async()=>{try{const s=await(await fetch('/api/status')).json();
document.title='AFFA2 '+(s.registered?'OK':'...');}catch(x){}},3000);
</script>)HTML";

void routes() {
  g_server.on("/", HTTP_GET, [](PsychicRequest* r) {
    return r->reply(200, "text/html", kPage);
  });
  g_server.on("/api/status", HTTP_GET, [](PsychicRequest* r) {
    const affa::Stats s = g_display.stats();
    char b[320];
    snprintf(b, sizeof(b),
      "{\"uptimeMs\":%lu,\"canUp\":%s,\"synced\":%s,\"registered\":%s,"
      "\"rxFrames\":%lu,\"txFrames\":%lu}",
      static_cast<unsigned long>(millis()), g_canUp ? "true" : "false",
      g_display.synced() ? "true" : "false", g_display.registered() ? "true" : "false",
      static_cast<unsigned long>(s.rxFrames), static_cast<unsigned long>(s.txFrames));
    return r->reply(200, "application/json", b);
  });
  g_server.on("/api/normal", HTTP_GET, [](PsychicRequest* r) {
    return post(r, "normal", qs(r, "t", "HELLO").c_str(), nullptr, nullptr, nullptr,
                qs(r, "icons", "85").toInt(), qs(r, "mode", "255").toInt(),
                qs(r, "ch", "0").toInt());
  });
  g_server.on("/api/full", HTTP_GET, [](PsychicRequest* r) {
    return post(r, "full", qs(r, "t", "HELLO").c_str(), nullptr, nullptr, nullptr, 0, 0, 0);
  });
  g_server.on("/api/menu", HTTP_GET, [](PsychicRequest* r) {
    return post(r, "menu", qs(r, "i0", "MENU 1").c_str(), qs(r, "i1", "MENU 2").c_str(),
                qs(r, "i2", "MENU 3").c_str(), qs(r, "i3").c_str(),
                qs(r, "n", "3").toInt(), qs(r, "sel", "0").toInt(), 0);
  });
  g_server.on("/api/power", HTTP_GET, [](PsychicRequest* r) {
    return post(r, "power", nullptr, nullptr, nullptr, nullptr,
                qs(r, "on", "1").toInt(), 0, 0);
  });
  // /api/time?h=12&m=03  — the OEM cluster's clock command, 3EF A6 <hh> <mm>
  g_server.on("/api/time", HTTP_GET, [](PsychicRequest* r) {
    return post(r, "time", nullptr, nullptr, nullptr, nullptr,
                qs(r, "h", "12").toInt(), qs(r, "m", "0").toInt(), 0);
  });
  // /api/sweep?n=1..8 — one clock candidate; the hour it sets IS its candidate number
  g_server.on("/api/sweep", HTTP_GET, [](PsychicRequest* r) {
    return post(r, "sweep", nullptr, nullptr, nullptr, nullptr, qs(r, "n", "1").toInt(), 0, 0);
  });
  // /api/frame?id=1007&dlc=3&hex=A60C03 — arbitrary RAW frame, no framing applied
  g_server.on("/api/frame", HTTP_GET, [](PsychicRequest* r) {
    const String hex = qs(r, "hex");
    char raw[9] = {0};
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
    return post(r, "frame", raw, nullptr, nullptr, nullptr,
                qs(r, "id", "1007").toInt(), qs(r, "dlc", "0").toInt() ? qs(r, "dlc").toInt() : n, 0);
  });

  g_server.on("/api/reboot", HTTP_GET, [](PsychicRequest* r) {
    r->reply(200, "application/json", "{\"reboot\":true}");
    delay(200); ESP.restart(); return ESP_OK;
  });
}

void startNetwork() {
  Preferences p;
  String ssid, pass;
  if (p.begin(kNvsNamespace, /*readOnly=*/true)) {
    ssid = p.getString("ssid", ""); pass = p.getString("pass", ""); p.end();
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
  Serial.printf("\n[wifi] %s ip=%s\n", sta ? "STA" : "AP",
                (sta ? WiFi.localIP() : WiFi.softAPIP()).toString().c_str());
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);

  // CAN BEFORE WIFI. The panel is universal and picks its family at ITS power-up from the
  // radio it can hear; both share a supply, so fifteen seconds of WiFi join is fifteen
  // seconds of silence during which it decides. Safe because Esp32CanLink::begin() is
  // bounded — it installs a driver, reads a status word and returns.
  g_canUp = g_hw.begin(kPins, kBitrate, /*forceRecoveryMs=*/250);
  g_display.begin();
  for (uint8_t i = 0; i < 40; ++i) { g_display.poll(); delay(5); }

  // MANDATORY: `1B1 04 52 02 FF FF`. Without it every frame is acknowledged and none drawn.
  (void)g_display.setPower(true);
  for (uint8_t i = 0; i < 40; ++i) { g_display.poll(); delay(5); }
  (void)showNormal("AFFA2 MODES", kIconsNone, kIconModeOff, 0);

  startNetwork();
  g_server.config.max_uri_handlers = 24;
  g_server.listen(80);
  routes();
  ElegantOTA.onStart([]() { g_hw.setTxEnabled(false); });
  ElegantOTA.begin(&g_server);

  Serial.println("[modes] web UI on /, OTA on /update");
}

void loop() {
  g_display.poll();   // also the keep-alive: stop polling and the panel drops the link
  runCmd();
  ElegantOTA.loop();
}
