// 08_rows3 — three independent scrolling rows on the glass, on esp32_can.
//
// Built on 07_cantime's proven handshake (10:00 read off a real Carminat, session held for
// minutes at txErr 0 / rxErr 0). Everything up to and including registration is unchanged;
// what is new here is the two things a LIVE screen needs and a one-shot clock does not:
//
//   1. ISO-TP MULTI-FRAME TRANSMIT. A fullscreen is 98 bytes — one first frame plus thirteen
//      consecutive frames — and the display gates every single one behind a flow-control
//      frame. [CAP] `aknowledge on on display.csv`: 43 CFs, 43 `30 01 00` replies, BS=1
//      STmin=0, one FC per CF without exception. Bursting them does not work; the transfer
//      has to be pumped one frame per FC.
//
//   2. WHEN TO REPAINT. A fullscreen is ~14 ACKed frames, so the wire carries roughly five
//      screens a second. Painting on a timer queues faster than the wire drains and the
//      screen falls further behind reality every minute. Painting when the PREVIOUS screen
//      COMPLETED *and* the visible text has actually changed is self-limiting and cannot lag.
//
// Three rows, three independent scroll periods, so each one moves on its own schedule — the
// point of the example. Row 3 shows the uptime clock, which advances whether or not the text
// rows do, and is the easiest way to see at a glance that the screen is still live.
//
// A fullscreen needs NO teardown: the next fullscreen replaces it. Only a POPUP is a true
// overlay. WIRE-SPEC §8.6 had these the wrong way round until a bench session in July.
//
// Board: ESP32 DevKit V1, CRX -> GPIO5, CTX -> GPIO4.
//   pio run -e ex08_rows3 -t upload --upload-port COM5

#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <PsychicHttp.h>
#include <ElegantOTA.h>
#include <esp32_can.h>
#include <driver/twai.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

constexpr const char* kBuildVersion = "08_rows3 1.0.0";

#ifndef AFFA_CAN_RX
#  define AFFA_CAN_RX 5
#endif
#ifndef AFFA_CAN_TX
#  define AFFA_CAN_TX 4
#endif
constexpr gpio_num_t kRxPin   = static_cast<gpio_num_t>(AFFA_CAN_RX);
constexpr gpio_num_t kTxPin   = static_cast<gpio_num_t>(AFFA_CAN_TX);
constexpr uint32_t   kBitrate = 500000;

constexpr uint32_t kIdSyncTx  = 0x3AF;
constexpr uint32_t kIdSyncRx  = 0x3CF;
constexpr uint32_t kIdText    = 0x151;
constexpr uint32_t kIdNav     = 0x1F1;
constexpr uint32_t kIdPanel   = 0x1C1;
constexpr uint32_t kReplyFlag = 0x400;

constexpr uint8_t kAlive = 0xB9, kRequest = 0xBA, kRegister = 0x70, kAck = 0x74;
constexpr uint8_t kHello[8] = {0xB0, 0x14, 0x11, 0x00, 0x1F, 0x00, 0x00, 0x00};

constexpr uint32_t kHelloDelayMs = 31, kSettleMs = 400, kWarmUpMs = 50;
constexpr uint32_t kHeartbeatMs = 500, kAnnounceMs = 3000;
constexpr uint32_t kAckTimeoutMs = 2000, kRetryMs = 1500;

// Marquee geometry. 20 visible cells per row and a 6-space gap before the text wraps, so a
// short string does not jitter and a long one reads continuously.
constexpr size_t   kRowWidth = 20;
constexpr size_t   kRowGap   = 6;

// ---------------------------------------------------------------------------
// Log
// ---------------------------------------------------------------------------
struct LogRec { uint32_t ms = 0; char msg[96] = {0}; };
constexpr size_t kLogRing = 64;
LogRec       g_log[kLogRing];
size_t       g_logHead = 0;
portMUX_TYPE g_logMux = portMUX_INITIALIZER_UNLOCKED;

void logmsg(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void logmsg(const char* fmt, ...) {
  char buf[96];
  va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
  portENTER_CRITICAL(&g_logMux);
  LogRec& r = g_log[g_logHead];
  r.ms = millis();
  snprintf(r.msg, sizeof(r.msg), "%s", buf);
  g_logHead = (g_logHead + 1) % kLogRing;
  portEXIT_CRITICAL(&g_logMux);
  if (Serial) Serial.printf("[%lu] %s\n", static_cast<unsigned long>(r.ms), buf);
}

// ---------------------------------------------------------------------------
// Wire trace
// ---------------------------------------------------------------------------
struct WireRec {
  uint32_t firstMs = 0, lastMs = 0, count = 0;
  uint16_t id = 0; uint8_t tx = 0, d[8] = {0};
};
constexpr size_t kWireRing = 96;
WireRec      g_wire[kWireRing];
size_t       g_wireHead = 0;
uint32_t     g_wireSeq = 0;
portMUX_TYPE g_wireMux = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t g_rxCount = 0, g_txCount = 0, g_lastRxMs = 0;

void trace(uint16_t id, const uint8_t* d, bool tx) {
  portENTER_CRITICAL(&g_wireMux);
  ++g_wireSeq;
  WireRec& last = g_wire[(g_wireHead + kWireRing - 1) % kWireRing];
  if (last.count && last.tx == (tx ? 1 : 0) && last.id == id && memcmp(last.d, d, 8) == 0) {
    ++last.count; last.lastMs = millis();
  } else {
    WireRec& w = g_wire[g_wireHead];
    w.firstMs = w.lastMs = millis(); w.count = 1; w.id = id; w.tx = tx ? 1 : 0;
    memcpy(w.d, d, 8);
    g_wireHead = (g_wireHead + 1) % kWireRing;
  }
  portEXIT_CRITICAL(&g_wireMux);
}

// ---------------------------------------------------------------------------
// CAN
// ---------------------------------------------------------------------------
struct RxRec { uint32_t id; uint8_t len; uint8_t d[8]; };
constexpr size_t kRxRing = 64;
volatile RxRec g_rx[kRxRing];
volatile size_t g_rxHead = 0, g_rxTail = 0;
portMUX_TYPE g_rxMux = portMUX_INITIALIZER_UNLOCKED;

void onCanFrame(CAN_FRAME* f) {
  if (!f) return;
  portENTER_CRITICAL_ISR(&g_rxMux);
  const size_t next = (g_rxHead + 1) % kRxRing;
  if (next != g_rxTail) {
    g_rx[g_rxHead].id = f->id;
    g_rx[g_rxHead].len = f->length;
    for (uint8_t i = 0; i < 8; ++i) g_rx[g_rxHead].d[i] = f->data.uint8[i];
    g_rxHead = next;
  }
  portEXIT_CRITICAL_ISR(&g_rxMux);
}

bool rxPop(RxRec& out) {
  bool got = false;
  portENTER_CRITICAL(&g_rxMux);
  if (g_rxTail != g_rxHead) {
    out.id = g_rx[g_rxTail].id; out.len = g_rx[g_rxTail].len;
    for (uint8_t i = 0; i < 8; ++i) out.d[i] = g_rx[g_rxTail].d[i];
    g_rxTail = (g_rxTail + 1) % kRxRing;
    got = true;
  }
  portEXIT_CRITICAL(&g_rxMux);
  return got;
}

bool send(uint32_t id, const uint8_t* d, uint8_t n) {
  CAN_FRAME f;
  f.id = id; f.extended = 0; f.rtr = 0; f.length = 8;
  for (uint8_t i = 0; i < 8; ++i) f.data.uint8[i] = (i < n) ? d[i] : 0x00;
  const bool ok = CAN0.sendFrame(f);
  if (ok) { ++g_txCount; trace(static_cast<uint16_t>(id), f.data.uint8, true); }
  return ok;
}
bool send1(uint32_t id, uint8_t b) { return send(id, &b, 1); }

// ---------------------------------------------------------------------------
// ISO-TP transmit — one frame per flow control, BS=1
// ---------------------------------------------------------------------------
// [CAP] The display answers EVERY consecutive frame with `30 01 00` and only sends `74` when
// the declared length is satisfied. So this is not "send 14 frames": it is a pump that emits
// exactly one frame each time the display says continue. 43 CFs, 43 FCs, no exceptions.
struct IsoTpTx {
  uint8_t  payload[128];
  size_t   len      = 0;    // total bytes including the two PCI bytes
  size_t   sent     = 0;    // bytes already on the wire
  uint8_t  seq      = 0;    // ISO-TP continuation counter, wraps 0x2F -> 0x20
  bool     active   = false;
  bool     waitFc   = false;
  uint32_t deadline = 0;
} g_tx;

bool isoTpBegin(const uint8_t* p, size_t n) {
  if (g_tx.active || n > sizeof(g_tx.payload) || n < 8) return false;
  memcpy(g_tx.payload, p, n);
  // SN STARTS AT 1, NOT 0. ISO-TP numbers the first consecutive frame 0x21 and wraps
  // 0x2F -> 0x20 -> 0x21. Starting at 0x20 makes the very first CF out of sequence: the
  // display accepts the first frame, sends one flow control, and then simply stops
  // answering — the transfer stalls at byte 15 of 98 with no error anywhere. [CAP] every
  // multi-frame transfer in the captures opens `21 ..`.
  g_tx.len = n; g_tx.sent = 0; g_tx.seq = 1;
  g_tx.active = true; g_tx.waitFc = false;
  return true;
}

// Emits the next frame of the transfer. The first is the payload's own first 8 bytes (the
// builder already wrote the 0x10/len PCI into them); the rest are 0x2n + 7 data bytes.
bool isoTpPump(uint32_t now) {
  if (!g_tx.active || g_tx.waitFc) return false;
  uint8_t f[8];
  if (g_tx.sent == 0) {
    memcpy(f, g_tx.payload, 8);
    if (!send(kIdText, f, 8)) return false;
    g_tx.sent = 8;
  } else {
    f[0] = static_cast<uint8_t>(0x20 | (g_tx.seq & 0x0F));
    size_t k = 1;
    while (k < 8 && g_tx.sent < g_tx.len) f[k++] = g_tx.payload[g_tx.sent++];
    while (k < 8) f[k++] = 0x00;
    if (!send(kIdText, f, 8)) return false;
    g_tx.seq = static_cast<uint8_t>((g_tx.seq + 1) & 0x0F);
  }
  g_tx.waitFc   = true;                 // nothing more leaves until the display says so
  g_tx.deadline = now + kAckTimeoutMs;
  return true;
}

// ---------------------------------------------------------------------------
// The screen: three marquees, each on its own clock
// ---------------------------------------------------------------------------
struct Marquee {
  char     text[64] = {0};
  uint32_t periodMs = 0;     // 0 = static, never scrolls
  uint32_t nextAt   = 0;
  size_t   offset   = 0;
};
Marquee g_row[3];

// The visible window, wrapping through a gap so the text reads continuously.
void windowOf(const Marquee& m, char* out, size_t n) {
  const size_t textLen = strlen(m.text);
  if (textLen == 0) { out[0] = 0; return; }
  const size_t span = textLen + kRowGap;
  size_t w = 0;
  if (textLen <= n - 1 && m.periodMs == 0) {           // short and static: no scrolling
    snprintf(out, n, "%s", m.text);
    return;
  }
  while (w < n - 1 && w < kRowWidth) {
    const size_t i = (m.offset + w) % span;
    out[w] = (i < textLen) ? m.text[i] : ' ';
    ++w;
  }
  out[w] = 0;
}

// Byte-for-byte the library's showFullscreenText: 0x21 screen command, mode 0x05, the two
// leading spaces at content offset 32..33 that the OEM capture has, 0x0D between lines, the
// rest space-filled so unused cells render blank instead of as garbage.
size_t buildFullscreen(uint8_t* p, size_t cap, const char* l1, const char* l2, const char* l3) {
  constexpr size_t kTotal = 2 + 96;
  if (cap < kTotal) return 0;
  memset(p, 0x00, kTotal);
  p[0] = 0x10; p[1] = 0x60;            // ISO-TP FF, 96 content bytes
  p[2] = 0x21; p[3] = 0x05;            // screen command, fullscreen mode
  p[4] = 0xFF; p[5] = 0x00; p[6] = 0x00; p[7] = 0x40;
  for (size_t k = 2 + 32; k < kTotal; ++k) p[k] = 0x20;

  size_t q = 2 + 34;
  const auto putLine = [&](const char* s) {
    if (!s || !*s) return;             // an empty line is skipped entirely, separator too
    for (size_t i = 0; s[i] && q < 2 + 95; ++i) p[q++] = static_cast<uint8_t>(s[i]);
    if (q < 2 + 96) p[q++] = 0x0D;
  };
  putLine(l1); putLine(l2); putLine(l3);
  return kTotal;
}

// ---------------------------------------------------------------------------
// Handshake state machine — unchanged from 07_cantime, which is proven
// ---------------------------------------------------------------------------
enum class Step : uint8_t {
  Silent, SawRequest, Hello, WaitPanel, RegText, RegNav, Settle, PowerOn, WarmUp, Live
};

bool     g_announce = true;
constexpr const char* kCfgNamespace = "rows3";

const char* stepName(Step s) {
  switch (s) {
    case Step::Silent:     return g_announce ? "silent      announcing BA"
                                             : "silent      ANNOUNCE OFF";
    case Step::SawRequest: return "saw-request B0 burst scheduled";
    case Step::Hello:      return "hello       emitting B0 x3";
    case Step::WaitPanel:  return "wait-panel  waiting for the display's 1C1";
    case Step::RegText:    return "reg-151     waiting for 551 74";
    case Step::RegNav:     return "reg-1F1     waiting for 5F1 74";
    case Step::Settle:     return "settle      400 ms after registration";
    case Step::PowerOn:    return "power-on    waiting for 74";
    case Step::WarmUp:     return "warm-up     glass lighting";
    case Step::Live:       return "LIVE        painting three rows";
  }
  return "?";
}

Step     g_step = Step::Silent;
uint32_t g_stepAt = 0, g_helloIndex = 0, g_nextBeat = 0, g_nextAnnounce = 0;
bool     g_panelSeen = false, g_registered = false, g_announced = false;
uint32_t g_openings = 0, g_paints = 0, g_repaintFails = 0;
char     g_lastPainted[3][kRowWidth + 2] = {{0}, {0}, {0}};

bool expired(uint32_t now, uint32_t at) { return static_cast<int32_t>(now - at) >= 0; }

void toStep(Step s, uint32_t at, const char* why) {
  g_step = s; g_stepAt = at;
  logmsg("-> %s (%s)", stepName(s), why);
}

void restart(const char* why) {
  g_step = Step::Silent;
  g_helloIndex = 0; g_panelSeen = false; g_registered = false; g_announced = false;
  g_tx.active = false; g_tx.waitFc = false;
  for (auto& s : g_lastPainted) s[0] = 0;
  g_nextAnnounce = millis() + kAnnounceMs;
  logmsg("session reset: %s", why);
}

void handleRx(const RxRec& f, uint32_t now) {
  ++g_rxCount;
  g_lastRxMs = now ? now : 1;
  trace(static_cast<uint16_t>(f.id), f.d, false);

  if (f.id == kIdPanel) {                       // the mandatory 5C1 reflex
    send1(kIdPanel | kReplyFlag, kAck);
    if (f.len >= 1 && f.d[0] == kRegister && !g_panelSeen) {
      g_panelSeen = true;
      logmsg("display opened its own channel (1C1 70) - acked on 5C1");
    }
    return;
  }

  if (f.id != kIdSyncRx) return;

  if (f.len >= 3 && f.d[0] == 0x61 && f.d[1] == 0x11) {
    if (g_step == Step::Silent || g_step == Step::SawRequest) {
      // BA first; the NEXT request draws the burst. See 07_cantime for the capture.
      if (!g_announced) {
        g_announced = true;
        send1(kIdSyncTx, kRequest);
        logmsg("61 11 %02X -> BA sent; the next request gets B0", f.d[2]);
        return;
      }
      ++g_openings;
      g_helloIndex = 0;
      toStep(Step::SawRequest, now + kHelloDelayMs, "display asked again after our BA");
    } else if (g_registered) {
      restart("61 11 after registration - the display forgot us");
    }
  }
}

void handleReply(const RxRec& f, uint32_t now) {
  const uint32_t base = f.id & ~kReplyFlag;

  // FLOW CONTROL first: it is what drives a transfer in progress, and it arrives on the
  // same responder id as the terminal 74.
  if (g_tx.active && base == kIdText && f.len >= 3 && (f.d[0] & 0xF0) == 0x30) {
    if ((f.d[0] & 0x0F) == 0x00) {              // ContinueToSend
      g_tx.waitFc = false;
      return;
    }
    if ((f.d[0] & 0x0F) == 0x01) {              // Wait — hold, do not abort
      g_tx.deadline = now + kAckTimeoutMs;
      return;
    }
    logmsg("ISO-TP aborted by the display: FS=%02X", f.d[0] & 0x0F);
    g_tx.active = false;
    ++g_repaintFails;
    return;
  }

  if (f.len < 1 || f.d[0] != kAck) return;

  if (g_tx.active && base == kIdText) {         // the transfer completed
    g_tx.active = false;
    g_tx.waitFc = false;
    ++g_paints;
    return;
  }

  switch (g_step) {
    case Step::RegText:
      if (base != kIdText) return;
      if (send1(kIdNav, kRegister)) toStep(Step::RegNav, now + kAckTimeoutMs, "151 registered");
      break;
    case Step::RegNav:
      if (base != kIdNav) return;
      g_registered = true;
      g_nextBeat = now + kHeartbeatMs;
      toStep(Step::Settle, now + kSettleMs, "1F1 registered - session up");
      break;
    case Step::PowerOn:
      if (base != kIdText) return;
      toStep(Step::WarmUp, now + kWarmUpMs, "display ON acknowledged");
      break;
    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// Transmit side
// ---------------------------------------------------------------------------
void pumpRows(uint32_t now) {
  // Advance each marquee on its OWN clock. This is what makes the three rows independent.
  bool moved = false;
  for (auto& m : g_row) {
    if (m.periodMs == 0 || !expired(now, m.nextAt)) continue;
    m.nextAt = now + m.periodMs;
    const size_t span = strlen(m.text) + kRowGap;
    if (span) m.offset = (m.offset + 1) % span;
    moved = true;
  }

  if (g_tx.active) return;                      // one screen in flight at a time

  char w[3][kRowWidth + 2];
  for (int i = 0; i < 3; ++i) windowOf(g_row[i], w[i], sizeof(w[i]));

  // REPAINT ON CHANGE, NOT ON A TIMER. A fullscreen is ~14 ACKed frames; a timer would queue
  // faster than the wire drains and the screen would lag further behind every minute.
  const bool changed = strcmp(w[0], g_lastPainted[0]) || strcmp(w[1], g_lastPainted[1]) ||
                       strcmp(w[2], g_lastPainted[2]);
  if (!changed && !moved) return;
  if (!changed) return;

  uint8_t p[2 + 96];
  const size_t n = buildFullscreen(p, sizeof(p), w[0], w[1], w[2]);
  if (n && isoTpBegin(p, n)) {
    for (int i = 0; i < 3; ++i) snprintf(g_lastPainted[i], sizeof(g_lastPainted[i]), "%s", w[i]);
  }
}

void pump(uint32_t now) {
  if (g_registered && expired(now, g_nextBeat)) {
    g_nextBeat = now + kHeartbeatMs;
    uint8_t b9[2] = {kAlive, 0x00};
    send(kIdSyncTx, b9, 2);
  }

  // Drive a transfer in progress before anything else: the display is holding the wire open
  // for it, and a stalled transfer blocks every later screen.
  if (g_tx.active) {
    if (g_tx.waitFc) {
      if (expired(now, g_tx.deadline)) {
        logmsg("ISO-TP flow control timed out at byte %u/%u",
               static_cast<unsigned>(g_tx.sent), static_cast<unsigned>(g_tx.len));
        g_tx.active = false;
        ++g_repaintFails;
      }
      return;
    }
    if (g_tx.sent >= g_tx.len) {                // all bytes out; awaiting the terminal 74
      g_tx.waitFc = true;
      g_tx.deadline = now + kAckTimeoutMs;
      return;
    }
    isoTpPump(now);
    return;
  }

  switch (g_step) {
    case Step::Silent:
      if (!g_announce || !expired(now, g_nextAnnounce)) break;
      g_nextAnnounce = now + kAnnounceMs;
      send1(kIdSyncTx, kRequest);
      break;

    case Step::SawRequest:
      if (!expired(now, g_stepAt)) break;
      g_helloIndex = 0;
      g_step = Step::Hello;
      break;

    case Step::Hello:
      if (g_helloIndex && !expired(now, g_stepAt)) break;
      send(kIdSyncTx, kHello, 8);
      if (++g_helloIndex >= 3) { toStep(Step::WaitPanel, now + kAckTimeoutMs, "B0 x3 done"); break; }
      g_stepAt = now + kHelloDelayMs;
      break;

    case Step::WaitPanel:
      if (!g_panelSeen) {
        if (expired(now, g_stepAt)) restart("no 1C1 from the display after B0 x3");
        break;
      }
      if (send1(kIdText, kRegister)) toStep(Step::RegText, now + kAckTimeoutMs, "1C1 seen");
      break;

    case Step::RegText:
    case Step::RegNav:
      if (expired(now, g_stepAt)) restart("registration was not acknowledged");
      break;

    case Step::Settle:
      if (!expired(now, g_stepAt)) break;
      {
        const uint8_t on[5] = {0x03, 0x52, 0x09, 0x00, 0x00};
        if (send(kIdText, on, 5)) toStep(Step::PowerOn, now + kAckTimeoutMs, "settle done");
      }
      break;

    case Step::PowerOn:
      if (expired(now, g_stepAt)) toStep(Step::Settle, now + kRetryMs, "power unacked");
      break;

    case Step::WarmUp:
      if (!expired(now, g_stepAt)) break;
      toStep(Step::Live, 0, "glass warm - rows are live");
      break;

    case Step::Live:
      pumpRows(now);
      break;
  }
}

// ---------------------------------------------------------------------------
// HTTP — plain text, meta refresh
// ---------------------------------------------------------------------------
PsychicHttpServer g_server;

String twaiText() {
  twai_status_info_t st{};
  if (twai_get_status_info(&st) != ESP_OK) return String("twai   unavailable\n");
  const char* s = st.state == TWAI_STATE_STOPPED ? "STOPPED"
                : st.state == TWAI_STATE_RUNNING ? "RUNNING"
                : st.state == TWAI_STATE_BUS_OFF ? "BUS-OFF" : "RECOVERING";
  char line[200];
  snprintf(line, sizeof(line),
           "twai   %s  txErr %lu rxErr %lu busErr %lu arbLost %lu  qRx %lu qTx %lu",
           s, static_cast<unsigned long>(st.tx_error_counter),
           static_cast<unsigned long>(st.rx_error_counter),
           static_cast<unsigned long>(st.bus_error_count),
           static_cast<unsigned long>(st.arb_lost_count),
           static_cast<unsigned long>(st.msgs_to_rx),
           static_cast<unsigned long>(st.msgs_to_tx));
  return String(line);
}

void routes() {
  g_server.on("/", HTTP_GET, [](PsychicRequest* r) {
    const uint32_t now = millis();
    String out;
    out.reserve(4096);
    char hdr[640];
    snprintf(hdr, sizeof(hdr),
             "%s   %s %s   img %.8s\nup %lus\n\n"
             "step   %s\n"
             "panel  1C1 %s  registered %s  openings %lu\n"
             "screen paints %lu  failed %lu  inFlight %s (%u/%u)\n"
             "counts rx %lu tx %lu  lastRx %lus ago\n"
             "pins   rx=GPIO%d tx=GPIO%d @ %lu   (esp32_can)\n"
             "%s\n\n"
             "rows   [0] %-20s  %lu ms\n"
             "       [1] %-20s  %lu ms\n"
             "       [2] %-20s  %lu ms\n\n"
             "[ /restart ]  [ /update = OTA ]\n\n---- log ----\n",
             kBuildVersion, __DATE__, __TIME__, ESP.getSketchMD5().c_str(),
             static_cast<unsigned long>(now / 1000), stepName(g_step),
             g_panelSeen ? "yes" : "no", g_registered ? "yes" : "no",
             static_cast<unsigned long>(g_openings),
             static_cast<unsigned long>(g_paints),
             static_cast<unsigned long>(g_repaintFails),
             g_tx.active ? "yes" : "no",
             static_cast<unsigned>(g_tx.sent), static_cast<unsigned>(g_tx.len),
             static_cast<unsigned long>(g_rxCount), static_cast<unsigned long>(g_txCount),
             static_cast<unsigned long>(g_lastRxMs ? (now - g_lastRxMs) / 1000 : 0),
             static_cast<int>(kRxPin), static_cast<int>(kTxPin),
             static_cast<unsigned long>(kBitrate), twaiText().c_str(),
             g_lastPainted[0], static_cast<unsigned long>(g_row[0].periodMs),
             g_lastPainted[1], static_cast<unsigned long>(g_row[1].periodMs),
             g_lastPainted[2], static_cast<unsigned long>(g_row[2].periodMs));

    out += "<!doctype html><meta charset=utf-8><meta http-equiv=refresh content=2>"
           "<title>AffaRows3</title><body style='background:#111;color:#ddd'><pre>";
    out += hdr;
    char line[128];
    portENTER_CRITICAL(&g_logMux);
    for (size_t i = 0; i < kLogRing; ++i) {
      const LogRec& rec = g_log[(g_logHead + i) % kLogRing];
      if (!rec.ms && !rec.msg[0]) continue;
      snprintf(line, sizeof(line), "%8lu  %s\n", static_cast<unsigned long>(rec.ms), rec.msg);
      out += line;
    }
    portEXIT_CRITICAL(&g_logMux);
    out += "</pre>";
    return r->reply(200, "text/html", out.c_str());
  });

  g_server.on("/restart", HTTP_GET, [](PsychicRequest* r) {
    restart("console");
    return r->reply(200, "text/plain", "sequence restarted\n");
  });
}

void startWifi() {
  Preferences p;
  String ssid, pass;
  if (p.begin("megaopen", true)) {
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
    const uint32_t until = millis() + 15000;
    while (millis() < until && WiFi.status() != WL_CONNECTED) delay(100);
    sta = WiFi.status() == WL_CONNECTED;
  }
  if (!sta) { WiFi.mode(WIFI_AP); WiFi.softAP("AffaRows3", "affarows"); }
  const String ip = sta ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  Serial.printf("[wifi] %s  http://%s/  OTA http://%s/update\n",
                sta ? "STA" : "AP", ip.c_str(), ip.c_str());
}

}  // namespace

void setup() {
  pinMode(kTxPin, OUTPUT);
  digitalWrite(kTxPin, HIGH);          // release the bus: recessive is HIGH
  delay(2000);                         // MeganeCAN opens setup() with exactly this

  Serial.begin(115200);
  delay(300);
  Serial.printf("\n%s  %s %s\n", kBuildVersion, __DATE__, __TIME__);

  { Preferences p;
    if (p.begin(kCfgNamespace, true)) { g_announce = p.getBool("announce", true); p.end(); } }

  // Three rows, three different periods. Row 2 is the clock and never scrolls.
  snprintf(g_row[0].text, sizeof(g_row[0].text), "AFFA DISPLAY - ROW ONE SCROLLING");
  g_row[0].periodMs = 220;
  snprintf(g_row[1].text, sizeof(g_row[1].text), "SECOND ROW MOVES SLOWER");
  g_row[1].periodMs = 380;
  snprintf(g_row[2].text, sizeof(g_row[2].text), "UP 0s");
  g_row[2].periodMs = 0;

  CAN0.setCANPins(kRxPin, kTxPin);
  CAN0.begin(kBitrate);
  CAN0.setGeneralCallback(&onCanFrame);
  CAN0.watchFor();
  Serial.printf("[can] up at %lu bit/s, rx=GPIO%d tx=GPIO%d\n",
                static_cast<unsigned long>(kBitrate),
                static_cast<int>(kRxPin), static_cast<int>(kTxPin));

  startWifi();
  g_server.config.max_uri_handlers = 32;
  g_server.config.stack_size = 8192;
  g_server.config.lru_purge_enable = true;
  g_server.listen(80);
  ElegantOTA.begin(&g_server);
  routes();

  g_nextAnnounce = millis() + kAnnounceMs;
  logmsg("%s ready - three independent rows", kBuildVersion);
}

void loop() {
  const uint32_t now = millis();

  RxRec f;
  for (uint8_t i = 0; i < 32 && rxPop(f); ++i) {
    handleRx(f, now);
    if (f.id & kReplyFlag) handleReply(f, now);
  }

  // Keep the controller on the bus: after bus-off it stays STOPPED for ever while esp32_can
  // happily accepts sends into its software queue. See 07_cantime.
  static uint32_t s_nextCanCheck = 0;
  if (expired(now, s_nextCanCheck)) {
    s_nextCanCheck = now + 1000;
    twai_status_info_t st{};
    if (twai_get_status_info(&st) == ESP_OK) {
      if (st.state == TWAI_STATE_BUS_OFF) twai_initiate_recovery();
      else if (st.state == TWAI_STATE_STOPPED) twai_start();
    }
  }

  // The clock row, once a second. It advances whether or not the text rows do, which makes a
  // frozen screen obvious at a glance.
  static uint32_t s_nextClock = 0;
  if (expired(now, s_nextClock)) {
    s_nextClock = now + 1000;
    snprintf(g_row[2].text, sizeof(g_row[2].text), "UP %lus  PAINTS %lu",
             static_cast<unsigned long>(now / 1000), static_cast<unsigned long>(g_paints));
  }

  pump(now);

  static uint32_t s_nextStatus = 0;
  if (expired(now, s_nextStatus)) {
    s_nextStatus = now + 3000;
    Serial.printf("[%lu] %s | rx %lu tx %lu paints %lu fail %lu | %s\n",
                  static_cast<unsigned long>(now), stepName(g_step),
                  static_cast<unsigned long>(g_rxCount), static_cast<unsigned long>(g_txCount),
                  static_cast<unsigned long>(g_paints),
                  static_cast<unsigned long>(g_repaintFails), twaiText().c_str());
  }

  ElegantOTA.loop();
  delay(2);
}
