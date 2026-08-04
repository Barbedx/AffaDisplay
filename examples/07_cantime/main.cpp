// 07_cantime — the Carminat handshake and the clock, on esp32_can, in one small file.
//
// WHY THIS EXISTS. Every other example either strips the library out to prove a point
// (02_canspy, 06_authclock) or pulls the whole of src/ in (03_hello, 04_rows). This one is
// the middle: the protocol written plainly against collin80's esp32_can, with no AffaDisplay
// and no ESP-IDF TWAI calls, so the handshake can be read top to bottom without chasing a
// state machine through five files.
//
// THE HANDSHAKE, exactly as the four OEM captures in docs/captures/ show it. Direction is
// readable from the padding byte: 0xA3 is the display, 0x00 is the radio. We are the radio.
//
//   silence      -> 3AF BA 00 ...              we announce; nothing else, and slowly
//   3CF 61 11 xx -> (wait 31 ms) 3AF B0 14 11 00 1F   x3, 31 ms apart. xx may be 00 OR 01
//   1C1 <any>    -> 5C1 74 00 ...              MANDATORY reflex, every frame, ~0.5 ms
//   after 1C1 and B0#3 -> 151 70, wait 551 74, then 1F1 70, wait 5F1 74
//   +400 ms      -> 151 03 52 09 00            display ON
//   then         -> 151 05 56 '1''0''0''0'     clock 10:00
//   registered   -> 3AF B9 00 ... every 500 ms, free-running. NOT a reply to 69.
//
// The four rules that are easy to get wrong, all of them measured:
//   * `61 11 01` is the SAME request as `61 11 00`. One capture completes an entire session
//     on 01 with zero 00 frames. Waiting for a 00 that a warm display never sends is a hang.
//   * The B0 burst is timed from the `61 11`, not from our BA. Anchored on the request the
//     spread across four captures is 0.79 ms; anchored on BA it is 80 ms.
//   * The display registers ITS channel first. `1C1 70` lands 0.81-1.55 ms after B0#1 and
//     our `151 70` follows ~61 ms later. Registering before it is out of order.
//   * B9 is a heartbeat, not a pong. It free-runs at 500.08 ms while the display's 69 drifts
//     past it. Do not answer pings with it.
//
// UI: one page, plain text, meta-refresh. No websockets, no JSON, no JavaScript beyond the
// links. ElegantOTA is included ONLY because this bench has no reliable USB; without it a
// bad flash costs a cable.
//
// Build:
//   pio run -e ex07_cantime -t upload      first flash over USB
//   thereafter                             http://<ip>/  and  http://<ip>/update

#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <PsychicHttp.h>
#include <ElegantOTA.h>
#include <esp32_can.h>
// esp32_can drives the standard ESP-IDF TWAI peripheral underneath, so its status registers
// are readable directly. Without them a silent bus and a bus we cannot DECODE look identical
// — both are rx 0 — and telling them apart is the whole diagnosis.
#include <driver/twai.h>

#include <cstdarg>
#include <cstdio>

namespace {

constexpr const char* kBuildVersion = "07_cantime 1.0.0";

// ---------------------------------------------------------------------------
// Wiring and protocol constants
// ---------------------------------------------------------------------------
// Pins come from the build so one file serves both boards. The names are the TRANSCEIVER's:
// CRX is the transceiver's R output and must reach the MCU's CAN *RX*; CTX is its D input.
// Swapping them produces a bus that never errors and never receives, which reads as a dead
// display rather than as miswiring — that cost a full bench session.
//   ESP32-C3 SuperMini : CRX -> GPIO3,  CTX -> GPIO4
//   ESP32 DevKit V1    : CRX -> GPIO5,  CTX -> GPIO4
#ifndef AFFA_CAN_RX
#  define AFFA_CAN_RX 3
#endif
#ifndef AFFA_CAN_TX
#  define AFFA_CAN_TX 4
#endif
constexpr gpio_num_t kRxPin   = static_cast<gpio_num_t>(AFFA_CAN_RX);
constexpr gpio_num_t kTxPin   = static_cast<gpio_num_t>(AFFA_CAN_TX);
constexpr uint32_t   kBitrate = 500000;

constexpr uint32_t kIdSyncTx   = 0x3AF;   // we transmit here
constexpr uint32_t kIdSyncRx   = 0x3CF;   // the display transmits here
constexpr uint32_t kIdText     = 0x151;   // our text/control channel   -> acked on 0x551
constexpr uint32_t kIdNav      = 0x1F1;   // our second function        -> acked on 0x5F1
constexpr uint32_t kIdPanel    = 0x1C1;   // the display's own channel  -> WE ack on 0x5C1
constexpr uint32_t kReplyFlag  = 0x400;   // responder id = requester id + 0x400

constexpr uint8_t kAlive    = 0xB9;       // heartbeat, ours
constexpr uint8_t kRequest  = 0xBA;       // "is anyone there", ours
constexpr uint8_t kRegister = 0x70;       // open/register a function
constexpr uint8_t kAck      = 0x74;       // acknowledged

constexpr uint8_t kHello[8] = {0xB0, 0x14, 0x11, 0x00, 0x1F, 0x00, 0x00, 0x00};

constexpr uint32_t kHelloDelayMs   = 31;    // 61 11 -> B0#1, and B0 to B0. Measured 30.7-31.6
constexpr uint32_t kSettleMs       = 400;   // last registration ACK -> first payload
constexpr uint32_t kWarmUpMs       = 50;    // display ON -> clock
constexpr uint32_t kHeartbeatMs    = 500;   // B9, free-running, after registration only
constexpr uint32_t kAnnounceMs     = 3000;  // BA into a silent bus
constexpr uint32_t kAckTimeoutMs   = 2000;  // waiting for a 74
constexpr uint32_t kRetryMs        = 1500;  // between attempts after a failure

constexpr char kClock[5] = "1000";          // HHMM -> 10:00

// The BA announce switch, declared early because the step label reads it.
constexpr const char* kCfgNamespace = "cantime";
bool g_announce = true;

// ---------------------------------------------------------------------------
// Log — a plain ring, read over HTTP as text
// ---------------------------------------------------------------------------
struct LogRec { uint32_t ms = 0; char msg[96] = {0}; };
constexpr size_t kLogRing = 80;
LogRec       g_log[kLogRing];
size_t       g_logHead = 0;
portMUX_TYPE g_logMux = portMUX_INITIALIZER_UNLOCKED;

void logmsg(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void logmsg(const char* fmt, ...) {
  char buf[96];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  portENTER_CRITICAL(&g_logMux);
  LogRec& r = g_log[g_logHead];
  r.ms = millis();
  snprintf(r.msg, sizeof(r.msg), "%s", buf);
  g_logHead = (g_logHead + 1) % kLogRing;
  portEXIT_CRITICAL(&g_logMux);
  if (Serial) Serial.printf("[%lu] %s\n", static_cast<unsigned long>(r.ms), buf);
}

// ---------------------------------------------------------------------------
// Wire trace — every frame both ways, newest last, identical frames coalesced
// ---------------------------------------------------------------------------
struct WireRec {
  uint32_t firstMs = 0, lastMs = 0, count = 0;
  uint16_t id = 0;
  uint8_t  tx = 0, len = 0, d[8] = {0};
};
constexpr size_t kWireRing = 96;
WireRec      g_wire[kWireRing];
size_t       g_wireHead = 0;
uint32_t     g_wireSeq = 0;
portMUX_TYPE g_wireMux = portMUX_INITIALIZER_UNLOCKED;

// Counters, read by the page. Written from the CAN callback and from loop().
volatile uint32_t g_rxCount = 0, g_txCount = 0, g_lastRxMs = 0;

void trace(uint16_t id, const uint8_t* d, uint8_t len, bool tx) {
  portENTER_CRITICAL(&g_wireMux);
  ++g_wireSeq;
  WireRec& last = g_wire[(g_wireHead + kWireRing - 1) % kWireRing];
  const bool same = last.count && last.tx == (tx ? 1 : 0) && last.id == id &&
                    last.len == len && memcmp(last.d, d, 8) == 0;
  if (same) {
    ++last.count;
    last.lastMs = millis();
  } else {
    WireRec& w = g_wire[g_wireHead];
    w.firstMs = w.lastMs = millis();
    w.count = 1;
    w.id = id;
    w.tx = tx ? 1 : 0;
    w.len = len;
    memcpy(w.d, d, 8);
    g_wireHead = (g_wireHead + 1) % kWireRing;
  }
  portEXIT_CRITICAL(&g_wireMux);
}

// ---------------------------------------------------------------------------
// CAN
// ---------------------------------------------------------------------------

// Received frames are handed over by esp32_can's own task. Keep this to a ring push: the
// protocol runs in loop(), never in the callback.
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
    g_rx[g_rxHead].id  = f->id;
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
    out.id  = g_rx[g_rxTail].id;
    out.len = g_rx[g_rxTail].len;
    for (uint8_t i = 0; i < 8; ++i) out.d[i] = g_rx[g_rxTail].d[i];
    g_rxTail = (g_rxTail + 1) % kRxRing;
    got = true;
  }
  portEXIT_CRITICAL(&g_rxMux);
  return got;
}

// EVERY frame on this bus is DLC 8 and we pad with 0x00 — that padding is what identifies
// the radio, exactly as 0xA3 identifies the display. Never pad with anything else.
bool send(uint32_t id, const uint8_t* d, uint8_t n) {
  CAN_FRAME f;
  f.id       = id;
  f.extended = 0;
  f.rtr      = 0;
  f.length   = 8;
  for (uint8_t i = 0; i < 8; ++i) f.data.uint8[i] = (i < n) ? d[i] : 0x00;
  const bool ok = CAN0.sendFrame(f);
  if (ok) {
    ++g_txCount;
    trace(static_cast<uint16_t>(id), f.data.uint8, 8, /*tx=*/true);
  }
  return ok;
}

bool send1(uint32_t id, uint8_t b) { return send(id, &b, 1); }

// ---------------------------------------------------------------------------
// The handshake state machine
// ---------------------------------------------------------------------------
enum class Step : uint8_t {
  Silent,        // nothing heard ever; announce BA slowly
  SawRequest,    // a 61 11 xx arrived; B0 burst is scheduled
  Hello,         // emitting B0 #1..#3
  WaitPanel,     // B0 done; waiting for the display's own 1C1
  RegText,       // 151 70 sent, waiting for 551 74
  RegNav,        // 1F1 70 sent, waiting for 5F1 74
  Settle,        // 400 ms quiet after the last registration ACK
  PowerOn,       // 151 03 52 09 00 sent, waiting for 551 74
  WarmUp,        // short pause before the clock
  SetClock,      // 151 05 56 "1000" sent, waiting for 551 74
  Done
};

const char* stepName(Step s) {
  switch (s) {
    // The label must not claim we are announcing when the switch is off — a status line that
    // describes the code's intent rather than its behaviour is worse than none.
    case Step::Silent:     return g_announce ? "silent      announcing BA, nothing heard yet"
                                             : "silent      ANNOUNCE OFF - not transmitting";
    case Step::SawRequest: return "saw-request 61 11 seen; B0 burst scheduled";
    case Step::Hello:      return "hello       emitting B0 x3";
    case Step::WaitPanel:  return "wait-panel  B0 done; waiting for the display's 1C1";
    case Step::RegText:    return "reg-151     sent 70, waiting for 551 74";
    case Step::RegNav:     return "reg-1F1     sent 70, waiting for 5F1 74";
    case Step::Settle:     return "settle      400 ms after registration";
    case Step::PowerOn:    return "power-on    sent 03 52 09, waiting for 74";
    case Step::WarmUp:     return "warm-up     glass lighting";
    case Step::SetClock:   return "set-clock   sent 05 56 1000, waiting for 74";
    case Step::Done:       return "DONE        clock acknowledged - check the glass";
  }
  return "?";
}

Step     g_step        = Step::Silent;
uint32_t g_stepAt      = 0;      // deadline for the current step
uint32_t g_helloIndex  = 0;
bool     g_panelSeen   = false;  // the display's 1C1 has been observed
bool     g_registered  = false;
// Our BA has gone out in this session. The display answers BA with its next `61 11`, and
// THAT is the one the B0 burst replies to. See handleRx.
bool     g_announced   = false;
uint32_t g_nextBeat    = 0;
uint32_t g_nextAnnounce = 0;
uint32_t g_openings    = 0, g_acks = 0, g_panelAcks = 0;

// The BA announce, switchable and REMEMBERED ACROSS REBOOTS. Announcing into a silent bus is
// how a sleeping display gets woken, but it is also the only frame we put on a bus we may be
// sharing — so it has to be possible to shut it off and have it STAY off through the reset
// that a flash implies. Default on.

void loadConfig() {
  Preferences p;
  if (p.begin(kCfgNamespace, /*readOnly=*/true)) {
    g_announce = p.getBool("announce", true);
    p.end();
  }
}

// WRITES ONLY ON AN ACTUAL CHANGE. An NVS commit stalls CAN reception outright for the
// duration of the flash write, because the TWAI ISR is not in IRAM. A console toggle is rare
// and deliberate; a write on every boot or every poll would not be.
void saveAnnounce(bool on) {
  if (on == g_announce) return;
  g_announce = on;
  Preferences p;
  if (p.begin(kCfgNamespace, /*readOnly=*/false)) {
    p.putBool("announce", on);
    p.end();
  }
  logmsg("BA announce %s (saved; survives reboot)", on ? "ON" : "OFF");
}

bool expired(uint32_t now, uint32_t at) { return static_cast<int32_t>(now - at) >= 0; }

void toStep(Step s, uint32_t at, const char* why) {
  g_step   = s;
  g_stepAt = at;
  logmsg("-> %s (%s)", stepName(s), why);
}

void restart(const char* why) {
  g_step        = Step::Silent;
  g_helloIndex  = 0;
  g_panelSeen   = false;
  g_registered  = false;
  g_announced   = false;
  g_nextAnnounce = millis() + kAnnounceMs;
  logmsg("session reset: %s", why);
}

// ---------------------------------------------------------------------------
// Receive
// ---------------------------------------------------------------------------
void handleRx(const RxRec& f, uint32_t now) {
  ++g_rxCount;
  g_lastRxMs = now ? now : 1;
  trace(static_cast<uint16_t>(f.id), f.d, f.len, /*tx=*/false);

  // THE REFLEX, AND IT IS UNCONDITIONAL. Every 1C1 gets a 5C1 74, at any phase — the
  // display opens its channel BETWEEN B0#1 and B0#2, before any authorization is complete.
  if (f.id == kIdPanel) {
    if (send1(kIdPanel | kReplyFlag, kAck)) ++g_panelAcks;
    if (f.len >= 1 && f.d[0] == kRegister && !g_panelSeen) {
      g_panelSeen = true;
      logmsg("display registered its own channel (1C1 70) - acked on 5C1");
    }
    return;
  }

  if (f.id != kIdSyncRx) return;

  // `61 11 xx` — the display's request. BOTH 00 and 01 are the same request; the low bit
  // reports the display's own state, it is not an authorization grade.
  if (f.len >= 3 && f.d[0] == 0x61 && f.d[1] == 0x11) {
    if (g_step == Step::Silent || g_step == Step::SawRequest) {
      // BA FIRST, AND THE B0 BURST ONLY ON THE NEXT REQUEST. [CAP] "cONNECT OT POWER": the
      // display is already repeating `61 11 01` when the radio appears; the radio answers
      // with BA, and it is the FOLLOWING `61 11 01` — 81 ms later — that draws the B0 burst
      // 30.75 ms after it. Jumping straight from the first request to B0 skips the exchange
      // the display is actually waiting on, and it then never opens its 1C1 channel.
      if (!g_announced) {
        g_announced = true;
        send1(kIdSyncTx, kRequest);
        logmsg("61 11 %02X seen -> BA sent; the NEXT request gets the B0 burst", f.d[2]);
        return;
      }
      ++g_openings;
      g_helloIndex = 0;
      toStep(Step::SawRequest, now + kHelloDelayMs, "display asked again after our BA");
      logmsg("   request byte was %02X - either value opens the session", f.d[2]);
    } else if (g_registered) {
      // A registered display never sends 61 11 in any capture. One arriving means it
      // forgot us, so the session is void rather than merely stale.
      restart("61 11 after registration - the display forgot us");
    }
    return;
  }

  // `69` — liveness only. It is NOT answered: B9 free-runs.
  if (f.len >= 1 && f.d[0] == 0x69) return;
}

void handleAck(const RxRec& f) {
  if (f.len < 1 || f.d[0] != kAck) return;
  ++g_acks;
  const uint32_t base = f.id & ~kReplyFlag;
  const uint32_t now  = millis();

  switch (g_step) {
    case Step::RegText:
      if (base != kIdText) return;
      logmsg("151 registered");
      if (send1(kIdNav, kRegister)) toStep(Step::RegNav, now + kAckTimeoutMs, "151 acked");
      break;
    case Step::RegNav:
      if (base != kIdNav) return;
      g_registered = true;
      g_nextBeat = now + kHeartbeatMs;     // the keep-alive starts HERE, not earlier
      logmsg("1F1 registered - session up; B9 heartbeat starts now");
      toStep(Step::Settle, now + kSettleMs, "registration complete");
      break;
    case Step::PowerOn:
      if (base != kIdText) return;
      toStep(Step::WarmUp, now + kWarmUpMs, "display ON acknowledged");
      break;
    case Step::SetClock:
      if (base != kIdText) return;
      logmsg(">>> clock acknowledged for %c%c:%c%c - CHECK THE GLASS <<<",
             kClock[0], kClock[1], kClock[2], kClock[3]);
      toStep(Step::Done, 0, "clock set");
      break;
    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// Transmit side of the state machine
// ---------------------------------------------------------------------------
void pump(uint32_t now) {
  // The heartbeat, once there is a session to keep alive. Free-running: it is deliberately
  // not re-armed by anything the display does.
  if (g_registered && expired(now, g_nextBeat)) {
    g_nextBeat = now + kHeartbeatMs;
    uint8_t b9[2] = {kAlive, 0x00};
    send(kIdSyncTx, b9, 2);
  }

  switch (g_step) {
    case Step::Silent:
      // BA ONLY. B9 is the heartbeat of an established session and has no business on a bus
      // where the handshake has not started.
      if (!g_announce) break;              // switched off, and the setting is persistent
      if (!expired(now, g_nextAnnounce)) break;
      g_nextAnnounce = now + kAnnounceMs;
      if (send1(kIdSyncTx, kRequest)) {
        // Log it. "Is it actually transmitting?" was a real question on the bench and the
        // wire trace alone did not answer it convincingly.
        static uint32_t s_announces = 0;
        if (++s_announces <= 3 || (s_announces % 20) == 0)
          logmsg("BA announce #%lu sent", static_cast<unsigned long>(s_announces));
      } else {
        logmsg("BA announce REFUSED by the controller");
      }
      break;

    case Step::SawRequest:
      if (!expired(now, g_stepAt)) break;
      g_helloIndex = 0;
      g_step = Step::Hello;
      break;

    case Step::Hello:
      if (g_helloIndex && !expired(now, g_stepAt)) break;
      send(kIdSyncTx, kHello, 8);
      if (++g_helloIndex >= 3) {
        toStep(Step::WaitPanel, now + kAckTimeoutMs, "B0 x3 done");
        break;
      }
      g_stepAt = now + kHelloDelayMs;
      break;

    case Step::WaitPanel:
      // The display registers ITS channel before we register ours. If it never does, fall
      // back to a fresh announce rather than registering out of order.
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
      if (expired(now, g_stepAt)) { toStep(Step::Settle, now + kRetryMs, "power unacked"); }
      break;

    case Step::WarmUp:
      if (!expired(now, g_stepAt)) break;
      {
        const uint8_t clk[8] = {0x05, 0x56,
                                static_cast<uint8_t>(kClock[0]), static_cast<uint8_t>(kClock[1]),
                                static_cast<uint8_t>(kClock[2]), static_cast<uint8_t>(kClock[3]),
                                0x00, 0x00};
        if (send(kIdText, clk, 8)) toStep(Step::SetClock, now + kAckTimeoutMs, "glass warm");
      }
      break;

    case Step::SetClock:
      if (expired(now, g_stepAt)) { toStep(Step::WarmUp, now + kRetryMs, "clock unacked"); }
      break;

    case Step::Done:
      break;
  }
}

// ---------------------------------------------------------------------------
// HTTP — one page, plain text, meta refresh. No websockets, no JSON, no JS.
// ---------------------------------------------------------------------------
PsychicHttpServer g_server;
Preferences       g_prefs;
constexpr const char* kApSsid = "AffaTime";
constexpr const char* kApPass = "affatime";

// THE LINE THAT SEPARATES A SILENT BUS FROM ONE WE CANNOT DECODE. Both read rx 0.
//   busErr climbing, rx 0        -> traffic is present and we are failing to decode it:
//                                   wrong bitrate, or RX wired to the wrong pin/idle level
//   busErr frozen at 0, rx 0     -> genuinely nothing arriving
//   rxMissed climbing            -> frames ARE arriving and we are not draining them
//   state != RUNNING             -> the controller is off the bus (bus-off / stopped)
String twaiText() {
  twai_status_info_t st{};
  if (twai_get_status_info(&st) != ESP_OK)
    return String("twai   status unavailable (driver not installed?)\n");

  const char* state = st.state == TWAI_STATE_STOPPED   ? "STOPPED"
                    : st.state == TWAI_STATE_RUNNING   ? "RUNNING"
                    : st.state == TWAI_STATE_BUS_OFF   ? "BUS-OFF"
                                                       : "RECOVERING";
  char line[220];
  snprintf(line, sizeof(line),
           "twai   %s  txErr %lu  rxErr %lu  busErr %lu  arbLost %lu\n"
           "       rxMissed %lu  rxOverrun %lu  queued rx %lu tx %lu",
           state,
           static_cast<unsigned long>(st.tx_error_counter),
           static_cast<unsigned long>(st.rx_error_counter),
           static_cast<unsigned long>(st.bus_error_count),
           static_cast<unsigned long>(st.arb_lost_count),
           static_cast<unsigned long>(st.rx_missed_count),
           static_cast<unsigned long>(st.rx_overrun_count),
           static_cast<unsigned long>(st.msgs_to_rx),
           static_cast<unsigned long>(st.msgs_to_tx));
  return String(line);
}

String wireText() {
  WireRec snap[kWireRing];
  size_t head;
  uint32_t seq;
  portENTER_CRITICAL(&g_wireMux);
  memcpy(snap, g_wire, sizeof(snap));
  head = g_wireHead;
  seq  = g_wireSeq;
  portEXIT_CRITICAL(&g_wireMux);

  String out;
  out.reserve(3072);
  char line[112];
  snprintf(line, sizeof(line), "%lu frames seen\n   first    last   xN dir  id  data\n",
           static_cast<unsigned long>(seq));
  out += line;
  // Newest last. Only the most recent rows, so the response stays small enough to allocate
  // reliably on the HTTP task — a page that goes blank under load is worse than a short one.
  constexpr size_t kShow = 40;
  size_t total = 0;
  for (size_t i = 0; i < kWireRing; ++i) if (snap[(head + i) % kWireRing].count) ++total;
  size_t skip = (total > kShow) ? total - kShow : 0, seen = 0;
  for (size_t i = 0; i < kWireRing; ++i) {
    const WireRec& w = snap[(head + i) % kWireRing];
    if (!w.count) continue;
    if (seen++ < skip) continue;
    int n = snprintf(line, sizeof(line), "%8lu %7lu %4lu  %s %03X ",
                     static_cast<unsigned long>(w.firstMs),
                     static_cast<unsigned long>(w.lastMs),
                     static_cast<unsigned long>(w.count),
                     w.tx ? "TX" : "RX", static_cast<unsigned>(w.id));
    for (uint8_t b = 0; b < 8; ++b)
      n += snprintf(line + n, sizeof(line) - n, " %02X", static_cast<unsigned>(w.d[b]));
    out += line;
    out += '\n';
  }
  return out;
}

String logText() {
  String out;
  out.reserve(2048);
  char line[128];
  portENTER_CRITICAL(&g_logMux);
  for (size_t i = 0; i < kLogRing; ++i) {
    const LogRec& r = g_log[(g_logHead + i) % kLogRing];
    if (!r.ms && !r.msg[0]) continue;
    snprintf(line, sizeof(line), "%8lu  %s\n", static_cast<unsigned long>(r.ms), r.msg);
    out += line;
  }
  portEXIT_CRITICAL(&g_logMux);
  return out;
}

void routes() {
  g_server.on("/", HTTP_GET, [](PsychicRequest* r) {
    const uint32_t now = millis();
    String out;
    out.reserve(6144);
    char hdr[512];
    snprintf(hdr, sizeof(hdr),
             "%s   %s %s   img %.8s\n"
             "up %lus\n\n"
             "step   %s\n"
             "panel  1C1 seen %s   registered %s   openings %lu\n"
             "counts rx %lu  tx %lu  acks %lu  panelAcks %lu  lastRx %lus ago\n"
             "pins   rx=GPIO%d tx=GPIO%d @ %lu bit/s   (esp32_can / can_common)\n"
             "BA     announce %s every %lu ms   (persistent)\n"
             "%s\n"
             "[ /announce?on=1 ] [ /announce?on=0 ] [ /restart ] [ /update = OTA ]\n"
             "[ /send?id=3AF&d=BA00000000000000 ]\n\n"
             "---- wire ----\n",
             kBuildVersion, __DATE__, __TIME__, ESP.getSketchMD5().c_str(),
             static_cast<unsigned long>(now / 1000),
             stepName(g_step),
             g_panelSeen ? "yes" : "no", g_registered ? "yes" : "no",
             static_cast<unsigned long>(g_openings),
             static_cast<unsigned long>(g_rxCount), static_cast<unsigned long>(g_txCount),
             static_cast<unsigned long>(g_acks), static_cast<unsigned long>(g_panelAcks),
             static_cast<unsigned long>(g_lastRxMs ? (now - g_lastRxMs) / 1000 : 0),
             static_cast<int>(kRxPin), static_cast<int>(kTxPin),
             static_cast<unsigned long>(kBitrate),
             g_announce ? "ON " : "OFF", static_cast<unsigned long>(kAnnounceMs), twaiText().c_str());
    out += "<!doctype html><meta charset=utf-8><meta http-equiv=refresh content=2>"
           "<title>AffaTime</title><body style='background:#111;color:#ddd'><pre>";
    out += hdr;
    out += wireText();
    out += "\n---- log ----\n";
    out += logText();
    out += "</pre>";
    return r->reply(200, "text/html", out.c_str());
  });

  // Raw injection: /send?id=3AF&d=BA00000000000000
  g_server.on("/send", HTTP_GET, [](PsychicRequest* r) {
    const String idStr = r->getParam("id") ? r->getParam("id")->value() : String();
    const String hex   = r->getParam("d")  ? r->getParam("d")->value()  : String();
    if (idStr.isEmpty() || hex.isEmpty() || (hex.length() % 2) || hex.length() > 16)
      return r->reply(400, "text/plain", "usage: /send?id=3AF&d=BA00000000000000\n");
    const long id = strtol(idStr.c_str(), nullptr, 16);
    if (id <= 0 || id > 0x7FF) return r->reply(400, "text/plain", "id must be 1..7FF hex\n");
    uint8_t d[8] = {0};
    for (size_t i = 0; i < hex.length(); i += 2) {
      char h[3] = {hex[i], hex[i + 1], 0};
      char* end = nullptr;
      const long v = strtol(h, &end, 16);
      if (end != h + 2) return r->reply(400, "text/plain", "non-hex digit in d\n");
      d[i / 2] = static_cast<uint8_t>(v);
    }
    const bool ok = send(static_cast<uint32_t>(id), d, hex.length() / 2);
    logmsg("console: raw send 0x%03lX %s", static_cast<unsigned long>(id),
           ok ? "accepted" : "REFUSED");
    return r->reply(ok ? 200 : 503, "text/plain", ok ? "sent\n" : "controller refused\n");
  });

  // /announce?on=0 or ?on=1 — persistent, survives reboot.
  g_server.on("/announce", HTTP_GET, [](PsychicRequest* r) {
    if (r->getParam("on")) saveAnnounce(r->getParam("on")->value().toInt() != 0);
    char msg[64];
    snprintf(msg, sizeof(msg), "BA announce is %s\n", g_announce ? "ON" : "OFF");
    return r->reply(200, "text/plain", msg);
  });

  g_server.on("/restart", HTTP_GET, [](PsychicRequest* r) {
    restart("console");
    return r->reply(200, "text/plain", "sequence restarted\n");
  });
}

void startWifi() {
  // "megaopen" is the namespace this board already holds credentials in — the same one
  // 04_rows reads. READ-ONLY matters: an NVS write stalls CAN reception for the duration of
  // the flash write, because the TWAI ISR is not in IRAM.
  String ssid, pass;
  if (g_prefs.begin("megaopen", /*readOnly=*/true)) {
    ssid = g_prefs.getString("ssid", "");
    pass = g_prefs.getString("pass", "");
    g_prefs.end();
  }

  WiFi.persistent(false);
  WiFi.setSleep(true);          // one radio on the C3; let it interleave with CAN timing

  bool sta = false;
  if (ssid.length()) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());
    const uint32_t until = millis() + 15000;
    while (millis() < until && WiFi.status() != WL_CONNECTED) delay(100);
    sta = WiFi.status() == WL_CONNECTED;
  }
  if (!sta) {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(kApSsid, kApPass);
  }
  const String ip = sta ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  Serial.printf("[wifi] %s  http://%s/  OTA http://%s/update\n",
                sta ? "STA" : "AP", ip.c_str(), ip.c_str());
}

}  // namespace

void setup() {
  // Release the bus before anything can block: TX high is recessive.
  pinMode(kTxPin, OUTPUT);
  digitalWrite(kTxPin, HIGH);

  // TWO SECONDS, AND THE BUS IS HELD RECESSIVE THROUGHOUT. The ESP32-C3 SuperMini is
  // unreliable in the first moments after reset — USB-CDC enumeration, the boot-mode strap
  // and the regulator all settle at their own pace, and a peripheral brought up inside that
  // window sometimes comes up wrong. This costs two seconds once per boot and removes a
  // whole class of "it only fails on some resets".
  delay(2000);

  loadConfig();
  Serial.begin(115200);
  delay(300);
  Serial.printf("\n%s  %s %s\n", kBuildVersion, __DATE__, __TIME__);

  // ORDER IS LOAD-BEARING: pins, begin(), callback, then watchFor() LAST.
  CAN0.setCANPins(kRxPin, kTxPin);
  CAN0.begin(kBitrate);
  CAN0.setGeneralCallback(&onCanFrame);
  CAN0.watchFor();
  Serial.printf("[can] up at %lu bit/s, rx=GPIO%d tx=GPIO%d\n",
                static_cast<unsigned long>(kBitrate),
                static_cast<int>(kRxPin), static_cast<int>(kTxPin));

  startWifi();

  g_server.config.max_uri_handlers = 32;
  g_server.config.stack_size       = 8192;
  g_server.config.lru_purge_enable = true;
  g_server.listen(80);
  ElegantOTA.begin(&g_server);      // OTA FIRST, always
  routes();

  g_nextAnnounce = millis() + kAnnounceMs;
  logmsg("%s ready - goal: handshake, then clock %c%c:%c%c",
         kBuildVersion, kClock[0], kClock[1], kClock[2], kClock[3]);
}

void loop() {
  const uint32_t now = millis();

  RxRec f;
  for (uint8_t i = 0; i < 32 && rxPop(f); ++i) {
    handleRx(f, now);
    if (f.id & kReplyFlag) handleAck(f);
  }

  // THE CONTROLLER MUST BE PUT BACK ON THE BUS, OR NOTHING ELSE HERE MATTERS.
  //
  // With no display attached, every frame we transmit goes unacknowledged — a CAN frame needs
  // one other node to ACK it — so the error counter climbs to 128, the controller goes
  // BUS-OFF and ends up STOPPED. That is CORRECT behaviour on an empty bus. The defect is
  // staying there: esp32_can keeps accepting sends into its software queue afterwards, so the
  // tx counter climbs and everything LOOKS alive while the peripheral is off the wire. It
  // would still be off the wire when the display is finally plugged in.
  //
  // Measured on the bench with the screen detached: state 0 (STOPPED), busErr 15, qTx 0,
  // tx counter climbing past 27. Nothing would ever have recovered that.
  static uint32_t s_nextCanCheck = 0;
  if (expired(now, s_nextCanCheck)) {
    s_nextCanCheck = now + 1000;
    twai_status_info_t st{};
    if (twai_get_status_info(&st) == ESP_OK) {
      if (st.state == TWAI_STATE_BUS_OFF) {
        twai_initiate_recovery();
        logmsg("controller BUS-OFF - recovery initiated");
      } else if (st.state == TWAI_STATE_STOPPED) {
        if (twai_start() == ESP_OK) logmsg("controller was STOPPED - restarted");
      }
    }
  }

  pump(now);

  // Serial status every 3 s. On a board with no saved WiFi this is the ONLY window into the
  // controller, and "rx 0" alone cannot tell a silent bus from one we fail to decode — the
  // queued-tx and error columns are what separate them.
  static uint32_t s_nextStatus = 0;
  if (expired(now, s_nextStatus)) {
    s_nextStatus = now + 3000;
    twai_status_info_t st{};
    if (twai_get_status_info(&st) == ESP_OK) {
      Serial.printf("[%lu] %s | rx %lu tx %lu | twai s=%u txErr %lu rxErr %lu busErr %lu "
                    "arbLost %lu qRx %lu qTx %lu\n",
                    static_cast<unsigned long>(now), stepName(g_step),
                    static_cast<unsigned long>(g_rxCount),
                    static_cast<unsigned long>(g_txCount),
                    static_cast<unsigned>(st.state),
                    static_cast<unsigned long>(st.tx_error_counter),
                    static_cast<unsigned long>(st.rx_error_counter),
                    static_cast<unsigned long>(st.bus_error_count),
                    static_cast<unsigned long>(st.arb_lost_count),
                    static_cast<unsigned long>(st.msgs_to_rx),
                    static_cast<unsigned long>(st.msgs_to_tx));
    }
  }

  ElegantOTA.loop();
  delay(2);
}
