// 16_navlab — a bench for the one channel nobody has ever written to: Carminat `0x1F1`.
//
// ############################################################################
// # STATUS: UNPROVEN ON GLASS. Nothing below has been sent to a real panel.  #
// #                                                                          #
// # What IS solid is the input: docs/captures/"aknowledge on on display.csv" #
// # holds the OEM head unit's complete 0x1F1 transfer — 44 frames, sequence  #
// # 21..2F 20..2F 20..2B, zero gaps, 302 declared bytes. It decodes cleanly   #
// # as a 14-byte header plus a 48x48 monochrome bitmap, and the bitmap draws  #
// # the idle navigation globe. That decode is why this example exists.        #
// #                                                                          #
// # What is NOT known: whether the panel accepts that message from US, what   #
// # the four unexplained header bytes do, and whether the seven-byte string   #
// # slot at offset 4 reaches the glass. Those are questions for the panel,    #
// # not for a reader, and this example is the instrument for asking them.     #
// ############################################################################
//
// THE POINT OF A LAB IS THAT YOU CHANGE ONE THING. Everything the OEM sent is reproduced
// here byte for byte and every byte is editable from a browser, one at a time, with the
// wire bytes shown before and after. The sweep tool walks a single header byte through a
// range and sends the image once per step, so an unknown field can be characterised in one
// pass instead of one rebuild per guess.
//
//   node tools/gen_navicons.js            regenerate nav_images.h from the capture
//   pio run -e ex16_navlab -t upload --upload-port COM5
//   then http://<ip>/            and       http://<ip>/update for OTA
//
// ---------------------------------------------------------------------------
// THE 302 BYTES, AS FAR AS THEY ARE UNDERSTOOD
// ---------------------------------------------------------------------------
//
//   21 0B 00 25 | 41 42 43 44 45 46 00 | 01 | 30 30 | <288 bytes of bitmap>
//   ^^ ^^ ^^ ^^   "A  B  C  D  E  F" \0   ^^   ^^ ^^
//   |  |  |  |    a seven-byte string     |    48, 48 — the bitmap geometry, and the only
//   |  |  |  |    slot, at a placeholder  |    two header bytes with better than a guess
//   |  |  |  |    because the captured    |    behind them: 48*48/8 is exactly the 288
//   |  |  |  |    car had no nav CD       |    bytes that follow.
//   |  |  |  |                            +--- 0x01. Format? Bit depth? Unknown.
//   |  |  |  +--- 0x25. Unknown.
//   |  |  +------ 0x00. Unknown; with the byte before it, possibly one 16-bit field.
//   |  +--------- 0x0B. Unknown.
//   +------------ 0x21. Command, by analogy with 0x151 where byte 0 is 0x52/0x54/0x77.
//
// The bitmap is row-major, 6 bytes per row, MSB-first: bit 7 of byte 0 is the top-left
// pixel. That orientation is INFERRED from the globe rendering right way up and from the
// image having exactly three blank rows at top and bottom; kBmpChecker exists to confirm
// it against the glass, because it is the only image here that is not symmetric.
//
// ---------------------------------------------------------------------------
// WHY enqueueExternal() AND NOT enqueue()
// ---------------------------------------------------------------------------
//
// 304 wire bytes do not fit AFFA_MAX_PAYLOAD, and raising that ceiling would widen the
// inline buffer in EVERY one of AFFA_TX_QUEUE_DEPTH slots plus finishJob()'s stack copy —
// about 1.1 kB, paid by every consumer of the library, for one message that is sent out of
// a static image. enqueueExternal() borrows the caller's pointer instead: 4 bytes a slot.
//
// THE PRICE IS THAT THE BUFFER IS OURS TO KEEP STILL. `g_wire` is borrowed by the transmit
// FSM from enqueueExternal() until the ticket completes, so a second send that overwrote it
// mid-transfer would corrupt the message already on the wire. g_navBusy is what stops that,
// and it is cleared in onComplete — the only place that knows the library is finished with
// the bytes. Short control messages go through the ordinary copying enqueue() and are
// deliberately NOT gated by it, so the display can still be turned off during a long send.

#include <Arduino.h>
#include <AffaDisplay.h>
#include <ESPmDNS.h>
#include <ElegantOTA.h>
#include <Preferences.h>
#include <PsychicHttp.h>
#include <WiFi.h>

#include "nav_images.h"

#if !AFFA_PANEL_CARMINAT
#  error "16_navlab is a Carminat example: build with -D AFFA_PANEL_CARMINAT=1"
#endif

namespace {

// ###########################################################################
// #  CAN STACK: collin80/esp32_can + collin80/can_common. THE STANDARD ONE.  #
// #                                                                         #
// #  affa::CanCommonLink is a thin ICanLink shim OVER that library — it is   #
// #  not a driver and it does not reimplement one. Same stack as 09..15,     #
// #  same stack as MeganeCAN, and the one proven end to end on real glass on #
// #  2026-08-04 with every error counter at zero.                            #
// #                                                                         #
// #  Do NOT swap this for Esp32CanLink (direct TWAI). That path survives in  #
// #  01/03/04 only because those examples predate the switch.                #
// ###########################################################################
//
// Pins and board follow 13/14/15 — the esp32dev rig, rx = GPIO5, tx = GPIO4. The C3 SuperMini
// modules are soldered mirrored between boards, so check yours before blaming the bus.
constexpr gpio_num_t kRxPin   = GPIO_NUM_5;
constexpr gpio_num_t kTxPin   = GPIO_NUM_4;
constexpr uint32_t   kBitrate = 500000;

constexpr const char* kWifiNamespace = "megaopen";   // ssid/pass live here already
constexpr const char* kApSsid  = "AffaNavLab";
constexpr const char* kApPass  = "affa1234";
constexpr const char* kMdnsName = "navlab";
constexpr uint32_t    kStaJoinMs = 15000;

struct ArduinoClock final : affa::IClock {
  uint32_t millis() const override { return ::millis(); }
};

affa::CanCommonLink   g_link;
ArduinoClock          g_clock;
affa::CarminatDisplay g_display(g_link, g_clock);
PsychicHttpServer     g_server;

// ---------------------------------------------------------------------------
// The message under test
// ---------------------------------------------------------------------------
constexpr uint16_t kHdrMax  = 32;
constexpr uint16_t kBmpLen  = navlab::kBitmapBytes;    // 288
constexpr uint16_t kWireMax = 2 + kHdrMax + kBmpLen;   // PCI + header + bitmap

uint8_t  g_hdr[kHdrMax];
uint16_t g_hdrLen = sizeof(navlab::kOemHeader);        // 14
uint8_t  g_bmp[kBmpLen];

// BORROWED BY THE LIBRARY between enqueueExternal() and onComplete. See the header comment.
uint8_t  g_wire[kWireMax];
uint16_t g_wireLen = 0;
volatile bool  g_navBusy = false;
affa::TxTicket g_navTicket = affa::kNoTicket;

// Counters the console reads back. Written from the library task, read from the HTTP task;
// both are 32-bit aligned scalars and the console tolerates a torn read of a statistic.
volatile uint32_t g_navSent = 0, g_navOk = 0, g_navFail = 0;
volatile uint32_t g_fcSeen = 0, g_ackSeen = 0;
char g_lastResult[32] = "-";

// ---------------------------------------------------------------------------
// A small log ring, shared with the HTTP task
// ---------------------------------------------------------------------------
portMUX_TYPE g_logMux = portMUX_INITIALIZER_UNLOCKED;
constexpr int kLogLines = 48;
char     g_log[kLogLines][96];
uint16_t g_logHead = 0;
uint32_t g_logSeq  = 0;

void logmsg(const char* fmt, ...) {
  char line[96];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  portENTER_CRITICAL(&g_logMux);
  snprintf(g_log[g_logHead], sizeof(g_log[0]), "%8lu %s", (unsigned long)::millis(), line);
  g_logHead = (g_logHead + 1) % kLogLines;
  ++g_logSeq;
  portEXIT_CRITICAL(&g_logMux);
}

// ---------------------------------------------------------------------------
// Hex helpers
// ---------------------------------------------------------------------------
int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Parses hex into `out`, skipping any separator. Returns the byte count, or -1 on a bad
// character or an odd number of nibbles — a truncated paste must not be sent as a short
// message that happens to parse.
int parseHex(const String& s, uint8_t* out, uint16_t cap) {
  uint16_t n = 0;
  int hi = -1;
  for (size_t i = 0; i < s.length(); ++i) {
    const int v = hexNibble(s[i]);
    if (v < 0) {
      if (s[i] == ' ' || s[i] == ',' || s[i] == ':' || s[i] == '\n' || s[i] == '\r') continue;
      return -1;
    }
    if (hi < 0) { hi = v; continue; }
    if (n >= cap) return -1;
    out[n++] = static_cast<uint8_t>((hi << 4) | v);
    hi = -1;
  }
  return (hi >= 0) ? -1 : static_cast<int>(n);
}

void appendHex(String& s, const uint8_t* d, uint16_t n) {
  static const char* kHex = "0123456789ABCDEF";
  s.reserve(s.length() + n * 2 + 4);
  for (uint16_t i = 0; i < n; ++i) { s += kHex[d[i] >> 4]; s += kHex[d[i] & 0xF]; }
}

// ---------------------------------------------------------------------------
// Transmit — ALL OF THIS RUNS ON loop(), never on an HTTP handler
// ---------------------------------------------------------------------------

// Frames a message the way the OEM does and hands it to the borrowing path.
//
// The library adds NO PCI: frame 0 carries eight raw payload bytes and the caller owns the
// ISO-TP header, which is why the single-frame length nibble and the first-frame 12-bit
// length are both built here. Anything over seven bytes is a first frame; `11 2E` for the
// nav screen falls straight out of that.
const char* sendFramed(uint16_t id, const uint8_t* msg, uint16_t len) {
  if (len == 0) return "empty";
  if (g_navBusy) return "busy: a transfer still owns the buffer";
  if (len > kHdrMax + kBmpLen) return "too long";

  uint16_t n = 0;
  if (len <= 7) {
    g_wire[n++] = static_cast<uint8_t>(len);                    // SF
  } else {
    g_wire[n++] = static_cast<uint8_t>(0x10 | (len >> 8));      // FF, high nibble of len
    g_wire[n++] = static_cast<uint8_t>(len & 0xFF);
  }
  memcpy(g_wire + n, msg, len);
  n += len;
  g_wireLen = n;

  const affa::TxTicket t = g_display.enqueueExternal(id, g_wire, n);
  if (t == affa::kNoTicket) {
    snprintf(g_lastResult, sizeof(g_lastResult), "reject:%d",
             static_cast<int>(g_display.lastResult()));
    logmsg("0x%03X REJECTED (%s)", id, g_lastResult);
    return "rejected — see /api/state";
  }
  g_navBusy   = true;
  g_navTicket = t;
  ++g_navSent;
  logmsg("0x%03X send %u payload + %u pci = %u wire, %u frames", id, len, n - len, n,
         1 + (n > 8 ? (n - 8 + 6) / 7 : 0));
  return nullptr;
}

// Short control messages take the ORDINARY copying path, so they are never blocked by a nav
// transfer holding g_wire — turning the display off must work at any moment.
const char* sendShort(uint16_t id, const uint8_t* msg, uint8_t len) {
  uint8_t buf[16];
  if (len == 0 || len > 7) return "control messages are single-frame only";
  buf[0] = len;
  memcpy(buf + 1, msg, len);
  const affa::TxTicket t = g_display.enqueue(id, buf, static_cast<uint8_t>(len + 1));
  if (t == affa::kNoTicket) {
    logmsg("0x%03X control REJECTED (%d)", id, static_cast<int>(g_display.lastResult()));
    return "rejected — see /api/state";
  }
  logmsg("0x%03X control %02X %02X %02X", id, msg[0], len > 1 ? msg[1] : 0,
         len > 2 ? msg[2] : 0);
  return nullptr;
}

// Builds header + bitmap into one contiguous message and sends it.
const char* sendNav() {
  static uint8_t msg[kHdrMax + kBmpLen];
  memcpy(msg, g_hdr, g_hdrLen);
  memcpy(msg + g_hdrLen, g_bmp, kBmpLen);
  return sendFramed(affa::carminat::kIdNav, msg, static_cast<uint16_t>(g_hdrLen + kBmpLen));
}

// ---------------------------------------------------------------------------
// The OEM's text message, verbatim
// ---------------------------------------------------------------------------
// From the same capture that carries the globe, 9 ms before it:
//
//   151  10 0E 77 09 55 FF 31 01 | 21 20 20 20 31 30 35 36 | 22 20 ...
//        = 14 bytes: 77 09 55 FF 31 01 "   1056 "
//
// Command 0x77 — NOT the 0x76 / 0x7E this library's setText uses — then five header bytes
// nobody has explained, then EXACTLY EIGHT characters. "   1056 " is the radio showing
// 105.6 FM. Sent here byte for byte because the point of the lab is to reproduce what the
// OEM did, not what we would have done.
const char* sendOemText(const String& t) {
  uint8_t msg[14] = { 0x77, 0x09, 0x55, 0xFF, 0x31, 0x01,
                      ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ' };
  for (int i = 0; i < 8; ++i)
    if (i < static_cast<int>(t.length())) msg[6 + i] = static_cast<uint8_t>(t[i]);
  return sendFramed(affa::carminat::kIdSetText, msg, sizeof(msg));
}

// ---------------------------------------------------------------------------
// Replay of the whole OEM opening
// ---------------------------------------------------------------------------
// THE NAV PANE IS PROBABLY NOT A SCREEN OF ITS OWN. In the capture the globe does not
// arrive into a blank panel — it arrives 9 ms after a text message, on a display switched
// on 25 ms earlier, in this order:
//
//   52 09 00   display ON        (the "off" captures send 52 00 00 and nothing follows)
//   54 01                        unexplained, always sent
//   54 03                        unexplained, always immediately after 54 01
//   77 ...     text              "   1056 "
//   0x1F1      the nav bitmap
//
// So a bare 0x1F1 into a panel that has been sent nothing may legitimately draw nothing,
// and that would say nothing about whether the message is right. This replays the sequence
// in the captured order so the bitmap is judged in the state the OEM put the panel in.
struct Replay {
  bool     active = false;
  uint8_t  step   = 0;
  uint32_t nextMs = 0;
  String   text;
} g_replay;

// ---------------------------------------------------------------------------
// The header-byte sweep — the reason this is a lab and not a viewer
// ---------------------------------------------------------------------------
// One unknown byte, stepped across a range, one send per step, paced far enough apart that
// each screen is on the glass long enough to read. Everything else is held still, which is
// the only way an unknown field tells you anything.
struct Sweep {
  bool     active = false;
  uint8_t  index  = 0;      // which header byte
  uint16_t from = 0, to = 0, step = 1, cur = 0;
  uint32_t periodMs = 1500, nextMs = 0;
  uint8_t  saved = 0;
} g_sweep;

void sweepStop(const char* why) {
  if (!g_sweep.active) return;
  g_hdr[g_sweep.index] = g_sweep.saved;      // always leave the header as we found it
  g_sweep.active = false;
  logmsg("sweep: stopped (%s), byte %u restored to 0x%02X", why, g_sweep.index, g_sweep.saved);
}

void sweepPoll() {
  if (!g_sweep.active) return;
  const uint32_t now = ::millis();
  if (static_cast<int32_t>(now - g_sweep.nextMs) < 0) return;
  if (g_navBusy) return;                     // never step while the buffer is lent out
  if (g_sweep.cur > g_sweep.to) { sweepStop("range complete"); return; }

  g_hdr[g_sweep.index] = static_cast<uint8_t>(g_sweep.cur);
  logmsg("sweep: hdr[%u] = 0x%02X", g_sweep.index, g_sweep.cur);
  const char* err = sendNav();
  if (err) { sweepStop(err); return; }
  g_sweep.cur += g_sweep.step;
  g_sweep.nextMs = now + g_sweep.periodMs;
}

void replayPoll() {
  if (!g_replay.active) return;
  if (static_cast<int32_t>(::millis() - g_replay.nextMs) < 0) return;
  if (g_navBusy) return;                   // never step while the buffer is lent out

  switch (g_replay.step) {
    case 0: {
      const uint8_t on[3] = { 0x52, 0x09, 0x00 };
      sendShort(affa::carminat::kIdDisplayCtrl, on, 3);
      logmsg("replay 1/5: display ON (52 09 00)");
      break;
    }
    case 1: {
      const uint8_t p[2] = { 0x54, 0x01 };
      sendShort(affa::carminat::kIdSetText, p, 2);
      logmsg("replay 2/5: 54 01");
      break;
    }
    case 2: {
      const uint8_t p[2] = { 0x54, 0x03 };
      sendShort(affa::carminat::kIdSetText, p, 2);
      logmsg("replay 3/5: 54 03");
      break;
    }
    case 3:
      // The ORDINARY library text, the way a radio sets it. The OEM's own 0x77 form is on
      // /api/oemtext for when the question is specifically about that command byte.
      (void)g_display.setText(g_replay.text.c_str());
      logmsg("replay 4/5: setText \"%s\"", g_replay.text.c_str());
      break;
    case 4:
      logmsg("replay 5/5: the nav bitmap");
      sendNav();
      break;
    default:
      g_replay.active = false;
      logmsg("replay: done — now read the glass");
      return;
  }
  ++g_replay.step;
  g_replay.nextMs = ::millis() + 250;
}

// ---------------------------------------------------------------------------
// Library callbacks — these run on the LIBRARY'S task, not on loop()
// ---------------------------------------------------------------------------
void onTap(const affa::Frame& f, affa::Direction d, void*) {
  if (d != affa::Direction::Rx) return;
  // The reply channel for both functions we own. 0x30 is flow control, 0x74 is done.
  if (f.id == (affa::carminat::kIdNav | 0x400) ||
      f.id == (affa::carminat::kIdSetText | 0x400)) {
    if (f.len >= 1 && (f.data[0] & 0xF0) == 0x30) ++g_fcSeen;
    else if (f.len >= 1 && f.data[0] == 0x74)     ++g_ackSeen;
  }
}

void onDone(affa::TxTicket t, affa::Result r, void*) {
  if (t != g_navTicket) return;
  // CLEAR THE FLAG FIRST. Everything after this line may be preempted, and a lab that
  // latches "busy" on an unexpected result is a lab that needs a reboot to continue.
  g_navBusy = false;
  if (r == affa::Result::Ok) ++g_navOk; else ++g_navFail;
  snprintf(g_lastResult, sizeof(g_lastResult), "%s", r == affa::Result::Ok ? "Ok" : "FAILED");
  logmsg("nav ticket %u -> %s (%d)", static_cast<unsigned>(t),
         r == affa::Result::Ok ? "Ok" : "FAILED", static_cast<int>(r));
  if (r != affa::Result::Ok) sweepStop("send failed");
}

void onSyncChanged(affa::SyncState s, void*) {
  logmsg("sync: 0x%02X%s", static_cast<unsigned>(s),
         affa::hasFlag(s, affa::SyncState::Failed) ? " (FAILED)" : "");
}

// ---------------------------------------------------------------------------
// Presets
// ---------------------------------------------------------------------------
struct Preset { const char* name; const uint8_t* bytes; };
const Preset kPresets[] = {
  { "globe",       navlab::kBmpGlobe },
  { "renault",     navlab::kBmpRenault },
  { "tryzub",      navlab::kBmpTryzub },
  { "tryzubclock", navlab::kBmpTryzubClock },
  { "clock",       navlab::kBmpClock },
  { "temp",        navlab::kBmpTemp },
  { "volts",       navlab::kBmpVolts },
  { "checker",     navlab::kBmpChecker },
};
constexpr int kPresetCount = sizeof(kPresets) / sizeof(kPresets[0]);

const uint8_t* presetByName(const String& n) {
  for (int i = 0; i < kPresetCount; ++i) if (n == kPresets[i].name) return kPresets[i].bytes;
  return nullptr;
}

#include "web_ui.h"     // kIndexHtml, kept out of the way of the logic

// ---------------------------------------------------------------------------
// Routes
// ---------------------------------------------------------------------------
void routes() {
  g_server.on("/", HTTP_GET, [](PsychicRequest* r) {
    PsychicResponse res(r);
    res.setContentType("text/html; charset=utf-8");
    res.setContent(kIndexHtml);
    return res.send();
  });

  g_server.on("/api/state", HTTP_GET, [](PsychicRequest* r) {
    String j("{");
    j += "\"phase\":\"";  j += affa::phaseName(g_display.phase());  j += "\"";
    j += ",\"live\":";    j += g_link.isLive() ? "true" : "false";
    j += ",\"busy\":";    j += g_navBusy ? "true" : "false";
    j += ",\"sent\":";    j += g_navSent;
    j += ",\"ok\":";      j += g_navOk;
    j += ",\"fail\":";    j += g_navFail;
    j += ",\"fc\":";      j += g_fcSeen;
    j += ",\"ack\":";     j += g_ackSeen;
    j += ",\"last\":\"";  j += g_lastResult; j += "\"";
    j += ",\"hdrlen\":";  j += g_hdrLen;
    j += ",\"hdr\":\"";   appendHex(j, g_hdr, g_hdrLen); j += "\"";
    j += ",\"wirelen\":"; j += g_wireLen;
    j += ",\"heap\":";    j += (uint32_t)ESP.getFreeHeap();
    j += ",\"sweep\":";   j += g_sweep.active ? "true" : "false";
    if (g_sweep.active) {
      j += ",\"sweepidx\":"; j += g_sweep.index;
      j += ",\"sweepcur\":"; j += g_sweep.cur;
    }
    j += "}";
    PsychicResponse res(r);
    res.setContentType("application/json");
    res.setContent(j.c_str());
    return res.send();
  });

  // The presets, as hex, so the browser canvas can load one without embedding them twice.
  g_server.on("/api/img", HTTP_GET, [](PsychicRequest* r) {
    const String n = r->hasParam("n") ? r->getParam("n")->value() : String("globe");
    const uint8_t* p = presetByName(n);
    if (!p) return r->reply(404, "text/plain", "no such preset");
    String s;
    appendHex(s, p, kBmpLen);
    PsychicResponse res(r);
    res.setContentType("text/plain");
    res.setContent(s.c_str());
    return res.send();
  });

  // The OEM header, for the browser's RESET button.
  g_server.on("/api/oemhdr", HTTP_GET, [](PsychicRequest* r) {
    String s;
    appendHex(s, navlab::kOemHeader, sizeof(navlab::kOemHeader));
    return r->reply(200, "text/plain", s.c_str());
  });

  // POST body: "<header hex>|<bitmap hex>". Sent on 0x1F1 with the ISO-TP header built here.
  g_server.on("/api/nav", HTTP_POST, [](PsychicRequest* r) {
    const String& b = r->body();
    const int bar = b.indexOf('|');
    if (bar < 0) return r->reply(400, "text/plain", "expected <hdr>|<bmp>");

    uint8_t hdr[kHdrMax], bmp[kBmpLen];
    const int hn = parseHex(b.substring(0, bar), hdr, kHdrMax);
    const int bn = parseHex(b.substring(bar + 1), bmp, kBmpLen);
    if (hn <= 0) return r->reply(400, "text/plain", "bad header hex");
    if (bn != static_cast<int>(kBmpLen))
      return r->reply(400, "text/plain", "bitmap must be exactly 288 bytes");

    memcpy(g_hdr, hdr, hn);
    g_hdrLen = static_cast<uint16_t>(hn);
    memcpy(g_bmp, bmp, kBmpLen);

    const char* err = sendNav();
    if (err) return r->reply(409, "text/plain", err);
    String s("sent ");
    s += g_wireLen; s += " wire bytes";
    return r->reply(200, "text/plain", s.c_str());
  });

  // Display on/off — the discriminator found in the capture: three "display off" traces all
  // send `52 00 00` and nothing follows; the one "display on" trace sends `52 09 00` and the
  // text and the nav screen follow within 25 ms.
  g_server.on("/api/power", HTTP_GET, [](PsychicRequest* r) {
    const bool on = !r->hasParam("on") || r->getParam("on")->value() != "0";
    const uint8_t msg[3] = { 0x52, static_cast<uint8_t>(on ? 0x09 : 0x00), 0x00 };
    const char* err = sendShort(affa::carminat::kIdDisplayCtrl, msg, 3);
    return r->reply(err ? 409 : 200, "text/plain", err ? err : (on ? "display ON" : "display OFF"));
  });

  // THE ORDINARY TEXT CHANNEL — setText(), exactly as any application would call it, so the
  // panel is in the normal text mode the nav pane appears alongside. This is the one to use;
  // /api/oemtext below is for when the question is specifically about the OEM's command byte.
  g_server.on("/api/text", HTTP_GET, [](PsychicRequest* r) {
    const String t = r->hasParam("t") ? r->getParam("t")->value() : String("RENAULT");
    const affa::Result res = g_display.setText(t.c_str());
    logmsg("setText \"%s\" -> %d", t.c_str(), static_cast<int>(res));
    return r->reply(res == affa::Result::Ok ? 200 : 409, "text/plain",
                    res == affa::Result::Ok ? "text queued" : "setText refused");
  });

  // The radio's own text message from the capture: 77 09 55 FF 31 01 + exactly 8 characters.
  g_server.on("/api/oemtext", HTTP_GET, [](PsychicRequest* r) {
    const String t = r->hasParam("t") ? r->getParam("t")->value() : String("   1056 ");
    const char* err = sendOemText(t);
    return r->reply(err ? 409 : 200, "text/plain", err ? err : "OEM 0x77 text sent");
  });

  g_server.on("/api/time", HTTP_GET, [](PsychicRequest* r) {
    const String t = r->hasParam("t") ? r->getParam("t")->value() : String("1056");
    const affa::Result res = g_display.setTime(t.c_str());
    logmsg("setTime \"%s\" -> %d", t.c_str(), static_cast<int>(res));
    return r->reply(res == affa::Result::Ok ? 200 : 409, "text/plain",
                    res == affa::Result::Ok ? "time queued" : "setTime refused");
  });

  // The whole captured opening, in order, so the bitmap is judged in the state the OEM put
  // the panel in rather than in whatever state it happens to be.
  g_server.on("/api/replay", HTTP_GET, [](PsychicRequest* r) {
    if (g_replay.active) return r->reply(409, "text/plain", "a replay is already running");
    g_replay.text   = r->hasParam("t") ? r->getParam("t")->value() : String("RENAULT");
    g_replay.step   = 0;
    g_replay.nextMs = ::millis();
    g_replay.active = true;
    logmsg("replay: OEM opening, text \"%s\"", g_replay.text.c_str());
    return r->reply(200, "text/plain", "replaying the OEM opening");
  });

  // Everything else the OEM put on 0x151 during init, as one-click probes.
  g_server.on("/api/probe", HTTP_GET, [](PsychicRequest* r) {
    const String w = r->hasParam("w") ? r->getParam("w")->value() : String();
    const uint8_t p5401[2] = { 0x54, 0x01 };
    const uint8_t p5403[2] = { 0x54, 0x03 };
    const uint8_t p25[4]   = { 0x25, 0x00, 0x00, 0x00 };
    const char* err = nullptr;
    if      (w == "5401") err = sendShort(affa::carminat::kIdSetText, p5401, 2);
    else if (w == "5403") err = sendShort(affa::carminat::kIdSetText, p5403, 2);
    else if (w == "25")   err = sendShort(affa::carminat::kIdSetText, p25, 4);
    else return r->reply(400, "text/plain", "w must be 5401, 5403 or 25");
    return r->reply(err ? 409 : 200, "text/plain", err ? err : "sent");
  });

  // Arbitrary payload on either function id. `pci=0` means the hex already contains the
  // ISO-TP header and is sent exactly as given — the escape hatch for probing a frame shape
  // this example does not model.
  g_server.on("/api/tx", HTTP_POST, [](PsychicRequest* r) {
    const uint16_t id = r->hasParam("id")
        ? static_cast<uint16_t>(strtoul(r->getParam("id")->value().c_str(), nullptr, 16))
        : affa::carminat::kIdSetText;
    const bool addPci = !r->hasParam("pci") || r->getParam("pci")->value() != "0";

    static uint8_t buf[kHdrMax + kBmpLen];
    const int n = parseHex(r->body(), buf, sizeof(buf));
    if (n <= 0) return r->reply(400, "text/plain", "bad hex");

    const char* err;
    if (addPci) {
      err = sendFramed(id, buf, static_cast<uint16_t>(n));
    } else {
      if (g_navBusy) err = "busy";
      else {
        memcpy(g_wire, buf, n);
        g_wireLen = static_cast<uint16_t>(n);
        const affa::TxTicket t = g_display.enqueueExternal(id, g_wire, g_wireLen);
        if (t == affa::kNoTicket) err = "rejected — see /api/state";
        else { g_navBusy = true; g_navTicket = t; ++g_navSent; err = nullptr; }
      }
    }
    return r->reply(err ? 409 : 200, "text/plain", err ? err : "sent");
  });

  g_server.on("/api/sweep", HTTP_GET, [](PsychicRequest* r) {
    if (r->hasParam("stop")) { sweepStop("console"); return r->reply(200, "text/plain", "stopped"); }
    const auto num = [&](const char* k, uint32_t dflt) -> uint32_t {
      return r->hasParam(k) ? strtoul(r->getParam(k)->value().c_str(), nullptr, 0) : dflt;
    };
    const uint32_t idx = num("i", 1);
    if (idx >= g_hdrLen) return r->reply(400, "text/plain", "byte index past the header");
    if (g_sweep.active)  return r->reply(409, "text/plain", "a sweep is already running");
    g_sweep.index    = static_cast<uint8_t>(idx);
    g_sweep.saved    = g_hdr[idx];
    g_sweep.from     = static_cast<uint16_t>(num("from", 0));
    g_sweep.to       = static_cast<uint16_t>(num("to", 255));
    g_sweep.step     = static_cast<uint16_t>(num("step", 1) ? num("step", 1) : 1);
    g_sweep.periodMs = num("ms", 1500);
    g_sweep.cur      = g_sweep.from;
    g_sweep.nextMs   = ::millis();
    g_sweep.active   = true;
    logmsg("sweep: hdr[%u] %u..%u step %u every %u ms", g_sweep.index, g_sweep.from,
           g_sweep.to, g_sweep.step, g_sweep.periodMs);
    return r->reply(200, "text/plain", "sweeping");
  });

  g_server.on("/api/log", HTTP_GET, [](PsychicRequest* r) {
    // STATIC, not a local: esp_http_server's task stack does not hold 4 kB of log.
    static char out[kLogLines * 96 + 64];
    size_t at = 0;
    portENTER_CRITICAL(&g_logMux);
    for (int i = 0; i < kLogLines; ++i) {
      const char* line = g_log[(g_logHead + i) % kLogLines];
      if (!line[0]) continue;
      const size_t n = strlen(line);
      if (at + n + 2 >= sizeof(out)) break;
      memcpy(out + at, line, n); at += n;
      out[at++] = '\n';
    }
    portEXIT_CRITICAL(&g_logMux);
    out[at] = 0;
    PsychicResponse res(r);
    res.setContentType("text/plain; charset=utf-8");
    res.setContent(out);
    return res.send();
  });

  g_server.on("/api/reboot", HTTP_GET, [](PsychicRequest* r) {
    r->reply(200, "text/plain", "rebooting");
    delay(200);
    ESP.restart();
    return ESP_OK;
  });
}

// ---------------------------------------------------------------------------
void startWifi() {
  Preferences p;
  String ssid, pass;
  if (p.begin(kWifiNamespace, /*readOnly=*/true)) {
    ssid = p.getString("ssid", "");
    pass = p.getString("pass", "");
    p.end();
  }
  WiFi.persistent(false);
  WiFi.setSleep(true);            // one radio, shared with a 500 kbit/s CAN session

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
  Serial.printf("\n[wifi] %s ip=%s  lab http://%s/  OTA http://%s/update  mdns %s.local\n",
                sta ? "STA" : "AP (fallback)", ip.c_str(), ip.c_str(), ip.c_str(), kMdnsName);
}

void startHttp() {
  // A full socket table is PERMANENT without lru_purge_enable: httpd stops calling accept()
  // and never resumes, while ping and mDNS keep answering. OTA goes with it.
  g_server.config.lru_purge_enable  = true;
  g_server.config.max_open_sockets  = 7;
  g_server.config.recv_wait_timeout = 3;
  g_server.config.send_wait_timeout = 3;
  // The URI table is a fixed array and registering past its end fails SILENTLY. Nine routes
  // here plus ElegantOTA's three; the headroom is deliberate.
  g_server.config.max_uri_handlers  = 64;
  g_server.config.stack_size        = 10240;   // /api/nav parses a 604-character body

  g_server.listen(80);

  // OTA FIRST, ALWAYS — before any route of ours can crowd its three registrations off the
  // end of that table. It is the only way back into a board with no cable.
  ElegantOTA.onStart([]() {
    g_link.setTxGate(false);          // an OTA write stalls CAN reception outright
    sweepStop("ota");
    logmsg("ota started - CAN TX gated");
  });
  ElegantOTA.onEnd([](bool ok) {
    if (!ok) g_link.setTxGate(true);  // clear on failure or every command is ignored
    logmsg("ota %s", ok ? "ok, rebooting" : "FAILED - TX ungated");
  });
  ElegantOTA.begin(&g_server);

  routes();
}

}  // namespace

void setup() {
  // RELEASE THE BUS BEFORE ANYTHING ELSE. The CAN TX pin is a floating input through reset
  // and early boot; TXD low is DOMINANT and jams the bus for that whole window.
  pinMode(kTxPin, OUTPUT);
  digitalWrite(kTxPin, HIGH);

  Serial.begin(115200);
  delay(300);
  Serial.println("\n[navlab] 16_navlab — the 0x1F1 bench");

  memcpy(g_hdr, navlab::kOemHeader, sizeof(navlab::kOemHeader));
  memcpy(g_bmp, navlab::kBmpGlobe, kBmpLen);

  // CAN BEFORE WIFI. A blocking WiFi.begin() with the controller already up banks hundreds
  // of thousands of bogus bus errors; bring the link up first and let it settle.
  if (!g_link.begin(kRxPin, kTxPin, kBitrate)) Serial.println("[navlab] CAN begin FAILED");
  g_display.onFrame(&onTap, nullptr);
  g_display.onComplete(&onDone, nullptr);
  g_display.onSync(&onSyncChanged, nullptr);
  if (!g_display.begin()) Serial.println("[navlab] display begin FAILED");

  startWifi();
  startHttp();
  logmsg("boot: header and globe loaded, %u bitmap bytes", kBmpLen);
}

void loop() {
  // OWNED BY loop(), like 14_demoreel and 15_iphone: AFFA_ENABLE_TASK=0. The HTTP handlers
  // call the display directly the way 04_rows does, so keep this loop free of blocking work.
  g_display.poll();
  ElegantOTA.loop();
  sweepPoll();
  replayPoll();
}
