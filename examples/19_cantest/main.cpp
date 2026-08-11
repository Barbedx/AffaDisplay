// 19_cantest — two ESP32s and a bus, with every variable ours.
//
// WHY THIS EXISTS. A session went into "why does this board never receive", and it produced
// a lot of retracted conclusions, because every measurement was taken through the AffaDisplay
// protocol, on a bus with a real panel on it, with an intermittent contact underneath — three
// variables moving at once. The answers kept flipping because the question kept changing.
//
// So: no panel, no protocol, no library in the default configuration. One node can prove
// itself alone, two nodes can prove the bus between them, and the one thing under test is
// named in the URL.
//
// ── the procedure it was built for ───────────────────────────────────────────
//   1. ONE BOARD AT A TIME, mode=selftest. The controller transmits with self-reception and
//      no ACK required, so the frame goes controller -> CTX -> transceiver -> CANH/CANL ->
//      transceiver -> CRX -> controller with nobody else involved. `matched` climbing is a
//      working controller, a working transceiver, both signal wires and this node's bus pair.
//      `matched` at zero is a fault entirely inside this board, and the counters say which
//      half: `queued` growing with every error at zero is the receive path (CRX stuck
//      dominant, so the controller never sees bus-idle and never starts); `txFail` climbing
//      is the transmit path. THIS STEP ALONE ANSWERS "is something wrong with this ESP".
//   2. BOTH ON THE BUS, mode=pingpong. Expect matched == sent, seqGaps 0, busErr 0, RTT a few
//      hundred microseconds. Soak it for five minutes.
//   3. IF STEP 2 IS MARGINAL, op=bitrate&v=125000. Clean at 125 k and dirty at 500 k is a
//      wiring or termination answer, not a silicon one, and it is one command instead of an
//      afternoon.
//   4. op=layer&v=link ON BOTH, repeat step 2. Same numbers means CanCommonLink is sound and
//      the panel can go back on the bus. Different numbers means the library layer is the
//      problem — found with no panel in the way to argue about.
//
// TERMINATION: the bus wants exactly two 120 ohm resistors, at the ends. With the panel out
// both modules need theirs — the panel was providing one of the pair, which is why removing
// one branch once left the other node bus-off.
//
// ── the switches ─────────────────────────────────────────────────────────────
// Anything that decides how the driver is installed is persisted in NVS and takes effect on
// the next boot, and the command reboots for you. That is not laziness: twai_driver_uninstall()
// does NOT hand the pads back to GPIO control, so a reinstall without gpio_reset_pin() yields
// a controller that reports RUNNING and hears nothing — the exact fault this example exists to
// diagnose, manufactured by the diagnostic. Rebooting sidesteps it entirely.
//
//   GET /                          the page, server-rendered, refreshes itself
//   GET /api/state                 every counter as JSON
//   GET /api/cmd?op=…
//        op=layer&v=twai|link      which stack owns the controller        (NVS, reboots)
//        op=mode&v=selftest|pingpong|listen                               (NVS, reboots)
//        op=bitrate&v=125000|250000|500000|1000000                        (NVS, reboots)
//        op=role&v=a|b             who originates and who echoes          (NVS, live)
//        op=rate&ms=               ping period                            (NVS, live)
//        op=reset                  zero the app counters
//        op=sample&ms=             live edge count on CRX
//        op=wifi&ssid=&pass=       store credentials (bare op=wifi reports the SSID)
//        op=reboot
//   /update                        OTA
//
//   pio run -e ex19_cantest_c3     -t upload
//   pio run -e ex19_cantest_devkit -t upload
//
// THE PAGE HAS NO JAVASCRIPT, deliberately. A console script is a C++ raw string literal, so
// a broken one compiles, links, flashes and serves perfectly while doing nothing —
// tools/check_web_ui.js exists because that has happened. An instrument used when everything
// else is suspect should have nothing in it that can fail silently, so this page is rendered
// on the device, refreshes with a meta tag and drives every command through plain links.
#include <Arduino.h>
#include <AffaDisplay.h>
#include <driver/twai.h>
#include <esp_rom_gpio.h>
#include <soc/gpio_sig_map.h>
#include <ESPmDNS.h>       // LDF only scans THIS file for #include, so the framework
#include <ElegantOTA.h>     // libraries shared/net.h uses have to be named here too, or
#include <Preferences.h>    // their include paths are never added to the build
#include <PsychicHttp.h>
#include <WiFi.h>

#include "../shared/net.h"

#if !AFFA_ENABLE_CANCOMMON_LINK
#  error "19_cantest needs the library's CAN seam for its `link` layer: -D AFFA_ENABLE_CANCOMMON_LINK=1"
#endif

namespace {

// ---------------------------------------------------------------------------
// The board
// ---------------------------------------------------------------------------
// FROM THE BUILD, never welded here. Two boards on one bus with different soldering is the
// whole point of this example, and a welded pin is how the last session lost a day: the env
// said rx=3, the binary listened on 5, and every counter agreed the bus was silent.
#ifndef AFFA_CAN_RX
#  define AFFA_CAN_RX 3
#endif
#ifndef AFFA_CAN_TX
#  define AFFA_CAN_TX 4
#endif
// 0 = node A, 1 = node B. Names the access point, and supplies the DEFAULT role — the role
// itself is a runtime setting, because which board originates is a question about the test,
// while which board this is, is a question about the bench.
#ifndef AFFA_CANTEST_NODE
#  define AFFA_CANTEST_NODE 0
#endif

constexpr gpio_num_t kRxPin = static_cast<gpio_num_t>(AFFA_CAN_RX);
constexpr gpio_num_t kTxPin = static_cast<gpio_num_t>(AFFA_CAN_TX);

// Two ids, low enough to win arbitration against anything a panel sends, and OUTSIDE every id
// the AffaDisplay protocol uses (0x121/0x151/0x1B1/0x1F1/0x3AF/0x3DF and the key ids). If one
// of these ever appears on a car's bus it is ours.
constexpr uint32_t kIdA = 0x100;   // role A originates here
constexpr uint32_t kIdB = 0x101;   // role B echoes here

constexpr const char* kNvs = "affacan";

enum class Layer : uint8_t { Twai = 0, Link = 1 };
enum class Mode  : uint8_t { SelfTest = 0, PingPong = 1, Listen = 2 };
enum class Role  : uint8_t { A = 0, B = 1 };

struct Cfg {
  // SELFTEST IS THE BOOT DEFAULT because it is step 1 and it needs no peer: a board flashed
  // for the first time answers "is this ESP alive" before anything else is even connected.
  Layer    layer   = Layer::Twai;
  Mode     mode    = Mode::SelfTest;
  Role     role    = AFFA_CANTEST_NODE ? Role::B : Role::A;
  uint32_t bitrate = 500000;
  uint16_t rateMs  = 100;
};
Cfg g_cfg;

const char* layerName(Layer l) { return l == Layer::Link ? "link" : "twai"; }
const char* modeName(Mode m) {
  return m == Mode::PingPong ? "pingpong" : m == Mode::Listen ? "listen" : "selftest";
}
const char* roleName(Role r) { return r == Role::B ? "b" : "a"; }

void loadCfg() {
  Preferences p;
  if (!p.begin(kNvs, true)) return;
  g_cfg.layer   = static_cast<Layer>(p.getUChar("layer", static_cast<uint8_t>(g_cfg.layer)));
  g_cfg.mode    = static_cast<Mode>(p.getUChar("mode", static_cast<uint8_t>(g_cfg.mode)));
  g_cfg.role    = static_cast<Role>(p.getUChar("role", static_cast<uint8_t>(g_cfg.role)));
  g_cfg.bitrate = p.getULong("bitrate", g_cfg.bitrate);
  g_cfg.rateMs  = static_cast<uint16_t>(p.getUShort("rate", g_cfg.rateMs));
  p.end();
}

void saveCfg() {
  Preferences p;
  if (!p.begin(kNvs, false)) return;
  p.putUChar("layer", static_cast<uint8_t>(g_cfg.layer));
  p.putUChar("mode", static_cast<uint8_t>(g_cfg.mode));
  p.putUChar("role", static_cast<uint8_t>(g_cfg.role));
  p.putULong("bitrate", g_cfg.bitrate);
  p.putUShort("rate", g_cfg.rateMs);
  p.end();
}

// selftest needs a self-reception request and NO_ACK; listen needs LISTEN_ONLY. esp32_can
// offers neither — it installs the driver in NORMAL mode and its send path has no self flag —
// so those two modes are raw-layer only. Said out loud rather than silently ignored: a mode
// that quietly did nothing is exactly the kind of instrument this example refuses to be.
bool modeNeedsRawLayer(Mode m) { return m != Mode::PingPong; }

// Anything this file had to change behind the user's back, in words, on the page and in the
// JSON. A board that quietly corrected itself and then reported a dead bus is the failure
// this whole example is a reaction to.
const char* g_note = "";

// ---------------------------------------------------------------------------
// The boot probe
// ---------------------------------------------------------------------------
// ONE SECOND OF CRX AS A PLAIN INPUT, BEFORE ANY DRIVER EXISTS — the best instrument found
// all session. A dead line reads 0 edges; a live 500 kbit/s bus read 70 214, and the
// controller's own `rx` started counting seconds later.
//
// EDGES ARE THE READING, never the high/low ratio: CRX is pulled up below, so a line with
// nothing on it reads high rather than low. `op=sample` takes the same count live, with the
// driver installed, and that is the one to loop at 500 ms while someone presses on a joint —
// a count that decays 271018 -> 175088 -> 64281 -> 2720 -> 0 over thirteen seconds with no
// code change is a mechanical fault, and no A/B of two firmwares can survive it.
uint32_t g_bootHi = 0, g_bootEdges = 0, g_bootN = 0;

void countEdges(uint32_t ms, uint32_t& hi, uint32_t& edges, uint32_t& n) {
  hi = edges = n = 0;
  int last = digitalRead(kRxPin);
  const uint32_t until = ::millis() + ms;
  while (static_cast<int32_t>(::millis() - until) < 0) {
    const int v = digitalRead(kRxPin);
    if (v) ++hi;
    if (v != last) { ++edges; last = v; }
    ++n;
  }
}

// ---------------------------------------------------------------------------
// The bus, either way round
// ---------------------------------------------------------------------------
// The two layers are given IDENTICAL pad treatment on purpose. If they were configured
// differently, any difference in their numbers would be an artifact of this file rather than
// a finding about the library — and comparing them is the entire point of step 4.
affa::CanCommonLink g_link;
bool g_busUp = false;
// Shut for the duration of a flash write. CanCommonLink has its own gate and the raw driver
// has none, so the gate lives here and covers both — otherwise an OTA on the twai layer would
// keep transmitting through the write.
bool g_txGate = true;

void preparePads() {
  // Pads do NOT come back as defaults after a software reset, which is what an OTA is. The IO
  // MUX and the GPIO matrix keep whatever the previous image left, so a pin an earlier build
  // routed elsewhere stays routed elsewhere and the driver installs on top of it: RUNNING,
  // every counter zero, nothing received.
  gpio_reset_pin(kRxPin);
  gpio_reset_pin(kTxPin);
}

void settlePads() {
  // Nail the input route down by hand — pad -> input buffer -> GPIO matrix -> TWAI_RX_IDX. If
  // anything has repointed that last hop the controller sits at RUNNING hearing nothing while
  // a scope on the pin shows good traffic: two instruments, one wire, opposite answers.
  gpio_set_direction(kRxPin, GPIO_MODE_INPUT);
  esp_rom_gpio_connect_in_signal(kRxPin, TWAI_RX_IDX, false);
  // And pull CRX up, so a receive line that loses contact reads recessive instead of floating
  // into a permanent dominant. Floating low costs 64 000 error flags a second at 500 kbit/s,
  // with rxErr pinned at 129 and the controller error-passive — drowning in its own error
  // frames, which is how it misses the windows when an intermittent contact returns.
  gpio_set_pull_mode(kRxPin, GPIO_PULLUP_ONLY);
}

bool busBegin() {
  preparePads();

  if (g_cfg.layer == Layer::Link) {
    if (!g_link.begin(kRxPin, kTxPin, g_cfg.bitrate)) return false;
    settlePads();                       // begin() does this too; doing it twice costs nothing
    return true;
  }

  const twai_mode_t mode = g_cfg.mode == Mode::Listen   ? TWAI_MODE_LISTEN_ONLY
                         : g_cfg.mode == Mode::SelfTest ? TWAI_MODE_NO_ACK
                                                        : TWAI_MODE_NORMAL;
  // TX FIRST. The IDF macro is (tx, rx, mode) and esp32_can's setCANPins() is (rx, tx) — two
  // stacks in one file with the pair in opposite orders. Swapping them yields a link that
  // never errors and never receives, which reads as a dead peer rather than as miswiring.
  twai_general_config_t gc = TWAI_GENERAL_CONFIG_DEFAULT(kTxPin, kRxPin, mode);
  gc.rx_queue_len = 32;
  gc.tx_queue_len = 32;

  twai_timing_config_t tc = TWAI_TIMING_CONFIG_500KBITS();
  switch (g_cfg.bitrate) {
    case 125000:  tc = TWAI_TIMING_CONFIG_125KBITS();  break;
    case 250000:  tc = TWAI_TIMING_CONFIG_250KBITS();  break;
    case 1000000: tc = TWAI_TIMING_CONFIG_1MBITS();    break;
    default:      g_cfg.bitrate = 500000;              break;
  }
  const twai_filter_config_t fc = TWAI_FILTER_CONFIG_ACCEPT_ALL();

  if (twai_driver_install(&gc, &tc, &fc) != ESP_OK) return false;
  if (twai_start() != ESP_OK) return false;
  settlePads();
  return true;
}

// `self` asks the controller to deliver its own transmission back to us. It is what makes
// selftest a measurement rather than a hope, and it is ignored by the link layer, which has no
// way to ask for it.
bool busSend(uint32_t id, const uint8_t* d, uint8_t len, bool self) {
  if (!g_busUp || !g_txGate) return false;
  if (g_cfg.layer == Layer::Link) {
    affa::Frame f{};
    f.id = id; f.len = len; f.ext = false;
    for (uint8_t i = 0; i < len && i < 8; ++i) f.data[i] = d[i];
    return g_link.send(f);
  }
  twai_message_t m = {};
  m.identifier       = id;
  m.data_length_code = len;
  m.self             = self ? 1 : 0;
  // m.ss STAYS ZERO. Single-shot means "do not retry on arbitration loss or error", and it
  // once cost 89% of this bench's transmits before anyone noticed the flag was set.
  for (uint8_t i = 0; i < len && i < 8; ++i) m.data[i] = d[i];
  return twai_transmit(&m, 0) == ESP_OK;
}

bool busRecv(uint32_t& id, uint8_t* d, uint8_t& len) {
  if (!g_busUp) return false;
  if (g_cfg.layer == Layer::Link) {
    affa::Frame f{};
    if (!g_link.recv(f)) return false;
    id = f.id; len = f.len;
    for (uint8_t i = 0; i < 8; ++i) d[i] = f.data[i];
    return true;
  }
  twai_message_t m = {};
  if (twai_receive(&m, 0) != ESP_OK) return false;
  id = m.identifier;
  len = m.data_length_code;
  for (uint8_t i = 0; i < 8; ++i) d[i] = m.data[i];
  return true;
}

// ---------------------------------------------------------------------------
// The counters
// ---------------------------------------------------------------------------
struct Counters {
  uint32_t sent = 0, refused = 0;
  uint32_t rxEcho = 0, rxOther = 0;
  uint32_t matched = 0, seqGaps = 0;
  uint32_t rttMinUs = 0xFFFFFFFFu, rttMaxUs = 0, rttN = 0;
  uint64_t rttSumUs = 0;
  uint32_t lastSeq = 0;
  bool     haveSeq = false;
};
Counters g_c;
uint32_t g_nextSeq = 1;

void resetCounters() { g_c = Counters{}; g_nextSeq = 1; }

uint32_t rd32(const uint8_t* d) {
  return static_cast<uint32_t>(d[0]) | (static_cast<uint32_t>(d[1]) << 8) |
         (static_cast<uint32_t>(d[2]) << 16) | (static_cast<uint32_t>(d[3]) << 24);
}
void wr32(uint8_t* d, uint32_t v) {
  d[0] = static_cast<uint8_t>(v);         d[1] = static_cast<uint8_t>(v >> 8);
  d[2] = static_cast<uint8_t>(v >> 16);   d[3] = static_cast<uint8_t>(v >> 24);
}

// Which id we transmit on, and which one carries the answer. In selftest both are ours: the
// controller hears its own frame, so the "echo" is the transmission itself.
uint32_t txId()    { return g_cfg.role == Role::A ? kIdA : kIdB; }
uint32_t watchId() {
  if (g_cfg.mode == Mode::SelfTest) return txId();
  return g_cfg.role == Role::A ? kIdB : kIdA;
}

// A frame carries seq (4 B LE) then txUs (4 B LE).
//
// MICROSECONDS, NOT MILLISECONDS. A round trip on a healthy 500 kbit/s bus is a few hundred
// microseconds, and a millisecond clock reports that as 0 or 1 — an instrument with no
// resolution in the range it exists to measure. The low 32 bits of micros() wrap every 71
// minutes, which a delta does not care about.
void onFrame(uint32_t id, const uint8_t* d, uint8_t len) {
  if (id != watchId() || len < 8) { ++g_c.rxOther; return; }
  ++g_c.rxEcho;

  const uint32_t seq = rd32(d);

  // GAPS ARE COUNTED IN FRAMES MISSING, not in events, so one long dropout does not read the
  // same as one lost frame. A backwards jump — a peer that rebooted — counts as one.
  if (g_c.haveSeq) {
    if (seq > g_c.lastSeq + 1)   g_c.seqGaps += seq - g_c.lastSeq - 1;
    else if (seq != g_c.lastSeq + 1) ++g_c.seqGaps;
  }
  g_c.lastSeq = seq;
  g_c.haveSeq = true;

  if (g_cfg.role == Role::B && g_cfg.mode == Mode::PingPong) {
    // B does not measure: the timestamp in the payload is A's clock, and the two are not
    // synchronised. B echoes, and its own `sent` is the count of echoes it managed.
    uint8_t out[8];
    for (uint8_t i = 0; i < 8; ++i) out[i] = d[i];
    if (busSend(kIdB, out, 8, false)) ++g_c.sent; else ++g_c.refused;
    return;
  }

  // A frame we sent, coming back. `matched` counts echoes whose seq is one we have issued —
  // so matched == sent is the pass condition, and a payload that came back corrupted or from
  // something else on the bus fails to match instead of quietly averaging into the RTT.
  if (seq == 0 || seq >= g_nextSeq) return;
  ++g_c.matched;

  const uint32_t rtt = static_cast<uint32_t>(micros()) - rd32(d + 4);
  if (rtt < 1000000u) {                       // a second of "round trip" is a wrapped clock
    if (rtt < g_c.rttMinUs) g_c.rttMinUs = rtt;
    if (rtt > g_c.rttMaxUs) g_c.rttMaxUs = rtt;
    g_c.rttSumUs += rtt;
    ++g_c.rttN;
  }
}

// The originator's tick. Role B in pingpong never gets here — it speaks only when spoken to,
// which is what makes a gap in ITS numbers unambiguous.
void tick() {
  if (g_cfg.mode == Mode::Listen) return;
  if (g_cfg.mode == Mode::PingPong && g_cfg.role == Role::B) return;

  static uint32_t nextMs = 0;
  const uint32_t now = ::millis();
  if (static_cast<int32_t>(now - nextMs) < 0) return;
  nextMs = now + g_cfg.rateMs;

  uint8_t d[8];
  wr32(d, g_nextSeq);
  wr32(d + 4, static_cast<uint32_t>(micros()));
  if (busSend(txId(), d, 8, g_cfg.mode == Mode::SelfTest)) { ++g_c.sent; ++g_nextSeq; }
  else ++g_c.refused;
}

// ---------------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------------
PsychicHttpServer g_server;
String g_out;

void jclear() { g_out = ""; }
void jf(const char* fmt, ...) {
  char b[320]; va_list ap; va_start(ap, fmt);
  vsnprintf(b, sizeof(b), fmt, ap); va_end(ap); g_out += b;
}
String q(PsychicRequest* r, const char* k, const char* def = "") {
  return r->hasParam(k) ? r->getParam(k)->value() : String(def);
}
long qn(PsychicRequest* r, const char* k, long def) {
  return r->hasParam(k) ? strtol(r->getParam(k)->value().c_str(), nullptr, 0) : def;
}

// The whole state, in one object, because the answer to "what is wrong" is always a
// comparison between the app's view and the controller's.
void buildState() {
  jclear();
  jf("{\"node\":\"%c\",\"layer\":\"%s\",\"mode\":\"%s\",\"role\":\"%s\"",
     'A' + AFFA_CANTEST_NODE, layerName(g_cfg.layer), modeName(g_cfg.mode),
     roleName(g_cfg.role));
  jf(",\"bitrate\":%lu,\"rateMs\":%u,\"rxPin\":%d,\"txPin\":%d,\"busUp\":%s",
     static_cast<unsigned long>(g_cfg.bitrate), g_cfg.rateMs, static_cast<int>(kRxPin),
     static_cast<int>(kTxPin), g_busUp ? "true" : "false");
  jf(",\"txId\":\"%03lX\",\"watchId\":\"%03lX\"",
     static_cast<unsigned long>(txId()), static_cast<unsigned long>(watchId()));

  jf(",\"app\":{\"sent\":%lu,\"refused\":%lu,\"rxEcho\":%lu,\"rxOther\":%lu",
     static_cast<unsigned long>(g_c.sent), static_cast<unsigned long>(g_c.refused),
     static_cast<unsigned long>(g_c.rxEcho), static_cast<unsigned long>(g_c.rxOther));
  jf(",\"matched\":%lu,\"seqGaps\":%lu", static_cast<unsigned long>(g_c.matched),
     static_cast<unsigned long>(g_c.seqGaps));
  jf(",\"rttMinUs\":%lu,\"rttAvgUs\":%lu,\"rttMaxUs\":%lu,\"rttN\":%lu}",
     static_cast<unsigned long>(g_c.rttN ? g_c.rttMinUs : 0),
     static_cast<unsigned long>(g_c.rttN ? g_c.rttSumUs / g_c.rttN : 0),
     static_cast<unsigned long>(g_c.rttMaxUs), static_cast<unsigned long>(g_c.rttN));

  // THE CONTROLLER'S OWN COUNTERS. They are the only thing that separates three states the
  // app numbers report identically as "nothing came back": an empty bus, a peer that is not
  // answering, and a receive pin that is stuck. Note state 0 is STOPPED, not an error code.
  twai_status_info_t t{};
  if (twai_get_status_info(&t) == ESP_OK) {
    jf(",\"twai\":{\"state\":%u,\"txErr\":%lu,\"rxErr\":%lu,\"txFail\":%lu,\"busErr\":%lu",
       static_cast<unsigned>(t.state), static_cast<unsigned long>(t.tx_error_counter),
       static_cast<unsigned long>(t.rx_error_counter),
       static_cast<unsigned long>(t.tx_failed_count),
       static_cast<unsigned long>(t.bus_error_count));
    jf(",\"arbLost\":%lu,\"rxMissed\":%lu,\"queuedTx\":%lu,\"queuedRx\":%lu}",
       static_cast<unsigned long>(t.arb_lost_count),
       static_cast<unsigned long>(t.rx_missed_count),
       static_cast<unsigned long>(t.msgs_to_tx), static_cast<unsigned long>(t.msgs_to_rx));
  } else {
    jf(",\"twai\":null");
  }

  jf(",\"boot\":{\"edges\":%lu,\"hi\":%lu,\"n\":%lu}",
     static_cast<unsigned long>(g_bootEdges), static_cast<unsigned long>(g_bootHi),
     static_cast<unsigned long>(g_bootN));
  jf(",\"heap\":%lu,\"uptimeMs\":%lu,\"note\":\"%s\"}",
     static_cast<unsigned long>(ESP.getFreeHeap()), static_cast<unsigned long>(::millis()),
     g_note);
}

bool g_rebootAt = false;
uint32_t g_rebootMs = 0;
void rebootSoon() { g_rebootAt = true; g_rebootMs = ::millis() + 400; }  // let the reply leave

String cmd(PsychicRequest* r) {
  const String op = q(r, "op");

  if (op == "layer" || op == "mode") {
    const Cfg before = g_cfg;          // restored on refusal; NVS may not have these keys yet
    const String v = q(r, "v");
    if (op == "layer") {
      if (v == "twai") g_cfg.layer = Layer::Twai;
      else if (v == "link") g_cfg.layer = Layer::Link;
      else return "layer: twai|link";
    } else {
      if (v == "selftest") g_cfg.mode = Mode::SelfTest;
      else if (v == "pingpong") g_cfg.mode = Mode::PingPong;
      else if (v == "listen") g_cfg.mode = Mode::Listen;
      else return "mode: selftest|pingpong|listen";
    }
    // REFUSE THE IMPOSSIBLE PAIR RATHER THAN BOOTING INTO IT. esp32_can installs NORMAL mode
    // and cannot request self-reception, so selftest and listen have no meaning there. A
    // board that rebooted into a mode it silently could not run would report a dead bus.
    if (g_cfg.layer == Layer::Link && modeNeedsRawLayer(g_cfg.mode)) {
      const Mode asked = g_cfg.mode;
      const Layer askedLayer = g_cfg.layer;
      g_cfg = before;
      return String("refused: layer=") + layerName(askedLayer) + " cannot run mode=" +
             modeName(asked) +
             " (esp32_can has no NO_ACK, no LISTEN_ONLY and no self-reception). Nothing changed.";
    }
    saveCfg();
    rebootSoon();
    return String("layer=") + layerName(g_cfg.layer) + " mode=" + modeName(g_cfg.mode) +
           " — rebooting";
  }

  if (op == "bitrate") {
    const long v = qn(r, "v", 500000);
    if (v != 125000 && v != 250000 && v != 500000 && v != 1000000)
      return "bitrate: 125000|250000|500000|1000000";
    g_cfg.bitrate = static_cast<uint32_t>(v);
    saveCfg();
    rebootSoon();
    return String("bitrate=") + g_cfg.bitrate + " — rebooting";
  }

  // Role and rate touch nothing in the driver, so they take effect now. Counters are zeroed
  // with the role because half of them would otherwise be about the other role.
  if (op == "role") {
    const String v = q(r, "v");
    if (v == "a") g_cfg.role = Role::A;
    else if (v == "b") g_cfg.role = Role::B;
    else return "role: a|b";
    resetCounters();
    saveCfg();
    return String("role=") + roleName(g_cfg.role);
  }

  if (op == "rate") {
    g_cfg.rateMs = static_cast<uint16_t>(constrain(qn(r, "ms", 100), 5L, 10000L));
    saveCfg();
    return String("rate=") + g_cfg.rateMs + " ms";
  }

  if (op == "reset") {
    // The driver's own counters are not ours to zero — only a reinstall clears them, and this
    // command deliberately does not reinstall anything.
    resetCounters();
    return "app counters zeroed (the twai block is the driver's and keeps counting)";
  }

  if (op == "sample") {
    // digitalRead() only taps the input path, so the controller keeps the pin and nothing is
    // reconfigured. Loop this while someone presses on a joint.
    const uint32_t ms = static_cast<uint32_t>(constrain(qn(r, "ms", 1000), 10L, 5000L));
    uint32_t hi = 0, edges = 0, n = 0;
    countEdges(ms, hi, edges, n);
    char b[96];
    snprintf(b, sizeof(b), "gpio%d: %lu edges, %lu/%lu high, in %lu ms", static_cast<int>(kRxPin),
             static_cast<unsigned long>(edges), static_cast<unsigned long>(hi),
             static_cast<unsigned long>(n), static_cast<unsigned long>(ms));
    return String(b);
  }

  if (op == "wifi") {
    const String s = q(r, "ssid");
    if (!s.length()) { String cur; affanet::storedSsid(cur); return String("ssid=") + cur; }
    affanet::storeWifi(s.c_str(), q(r, "pass").c_str());
    rebootSoon();
    return String("ssid=") + s + " stored — rebooting";
  }

  if (op == "reboot") { rebootSoon(); return "rebooting"; }

  return "op: layer|mode|bitrate|role|rate|reset|sample|wifi|reboot";
}

// One row of the page: label, value, and the note that says what the value MEANS. The notes
// are the point — a counter whose meaning lives in someone's head is how a session gets spent.
void row(String& h, const char* k, const String& v, const char* note) {
  h += "<tr><th>"; h += k; h += "</th><td><b>"; h += v; h += "</b></td><td>"; h += note;
  h += "</td></tr>";
}

String page() {
  twai_status_info_t t{};
  const bool haveT = twai_get_status_info(&t) == ESP_OK;
  static const char* kState[] = { "STOPPED", "RUNNING", "BUS-OFF", "RECOVERING" };

  String h("<!doctype html><meta charset=utf-8><meta http-equiv=refresh content=2>");
  h += "<title>cantest ";
  h += static_cast<char>('A' + AFFA_CANTEST_NODE);
  h += "</title><style>body{font:14px system-ui;margin:1.5rem;max-width:52rem}"
       "table{border-collapse:collapse;width:100%;margin:.6rem 0}"
       "th,td{border-bottom:1px solid #ddd;padding:.25rem .5rem;text-align:left}"
       "th{width:8rem;font-weight:normal;color:#666}td:last-child{color:#666;font-size:12px}"
       "a{display:inline-block;padding:.2rem .5rem;margin:.1rem;border:1px solid #ccc;"
       "border-radius:.3rem;text-decoration:none;color:#06c}h2{font-size:14px;margin:1rem 0 0}"
       "</style>";
  h += "<h1>node "; h += static_cast<char>('A' + AFFA_CANTEST_NODE);
  h += " — "; h += layerName(g_cfg.layer); h += " / "; h += modeName(g_cfg.mode);
  h += " / role "; h += roleName(g_cfg.role); h += "</h1>";

  if (*g_note) { h += "<p style='background:#fee;padding:.4rem .6rem;border-radius:.3rem'>";
                 h += g_note; h += "</p>"; }

  h += "<table>";
  row(h, "bus", g_busUp ? "up" : "DOWN",
      g_busUp ? "driver installed and started" : "the driver refused to install — check the pins");
  row(h, "pins", String("rx ") + static_cast<int>(kRxPin) + " / tx " + static_cast<int>(kTxPin),
      "from the build, not welded in the source");
  row(h, "bitrate", String(g_cfg.bitrate),
      "if 125k is clean and 500k is not, that is wiring or termination, not silicon");
  row(h, "ids", String(txId(), HEX) + " out / " + String(watchId(), HEX) + " watched",
      "in selftest both are ours — the controller hears its own frame");
  h += "</table>";

  h += "<h2>us</h2><table>";
  row(h, "sent", String(g_c.sent), "frames the controller accepted");
  row(h, "refused", String(g_c.refused), "it would not take them — queue full, or bus-off");
  row(h, "matched", String(g_c.matched),
      "echoes carrying a seq we issued. matched == sent is the pass condition");
  row(h, "seqGaps", String(g_c.seqGaps), "frames missing from the received sequence. must be 0");
  row(h, "rxEcho", String(g_c.rxEcho), "frames on the watched id");
  row(h, "rxOther", String(g_c.rxOther), "everything else on the bus — a panel would land here");
  row(h, "rtt", g_c.rttN ? String(g_c.rttMinUs) + " / " + String((uint32_t)(g_c.rttSumUs / g_c.rttN)) +
                           " / " + String(g_c.rttMaxUs) + " us"
                         : String("—"),
      "min / avg / max. A healthy 500k bus is a few hundred microseconds");
  h += "</table>";

  h += "<h2>the controller</h2><table>";
  if (haveT) {
    row(h, "state", kState[t.state & 3],
        "STOPPED is not an error code, it is state 0 — nothing has started it");
    row(h, "txErr / rxErr", String(t.tx_error_counter) + " / " + String(t.rx_error_counter),
        "128 is error-passive. rxErr pinned at 129 with a quiet bus is a floating CRX");
    row(h, "txFail", String(t.tx_failed_count), "the transmit path: it tried and could not");
    row(h, "busErr", String(t.bus_error_count), "must be 0 on a two-node bus with both terminators");
    row(h, "arbLost", String(t.arb_lost_count), "harmless in itself; a third talker if it climbs");
    row(h, "rxMissed", String(t.rx_missed_count), "we are not draining fast enough");
    row(h, "queued tx/rx", String(t.msgs_to_tx) + " / " + String(t.msgs_to_rx),
        "tx growing with every error at zero = CRX stuck dominant, so bus-idle never comes");
  } else {
    row(h, "twai", "unavailable", "no driver is installed");
  }
  h += "</table>";

  h += "<h2>the pin, before any driver existed</h2><table>";
  row(h, "boot edges", String(g_bootEdges),
      "0 is a dead line. A live 500k bus reads tens of thousands. READ EDGES, not the ratio");
  row(h, "boot high", String(g_bootHi) + " / " + String(g_bootN),
      "means little: CRX is pulled up, so a disconnected line reads high");
  h += "</table>";

  h += "<h2>switches</h2>";
  h += "<p>layer <a href='/api/cmd?op=layer&v=twai'>twai</a>"
       "<a href='/api/cmd?op=layer&v=link'>link</a>"
       " &nbsp; mode <a href='/api/cmd?op=mode&v=selftest'>selftest</a>"
       "<a href='/api/cmd?op=mode&v=pingpong'>pingpong</a>"
       "<a href='/api/cmd?op=mode&v=listen'>listen</a>"
       " &nbsp; role <a href='/api/cmd?op=role&v=a'>A</a><a href='/api/cmd?op=role&v=b'>B</a></p>";
  h += "<p>bitrate <a href='/api/cmd?op=bitrate&v=125000'>125k</a>"
       "<a href='/api/cmd?op=bitrate&v=250000'>250k</a>"
       "<a href='/api/cmd?op=bitrate&v=500000'>500k</a>"
       "<a href='/api/cmd?op=bitrate&v=1000000'>1M</a>"
       " &nbsp; <a href='/api/cmd?op=reset'>reset counters</a>"
       "<a href='/api/cmd?op=sample&ms=1000'>sample the pin</a>"
       "<a href='/api/cmd?op=reboot'>reboot</a>"
       "<a href='/api/state'>json</a><a href='/update'>OTA</a></p>";
  h += "<p style='color:#666;font-size:12px'>Layer, mode and bitrate reboot the board: "
       "uninstalling the driver does not hand the pads back, and reinstalling on top of that "
       "gives a controller that reports RUNNING and hears nothing.</p>";
  return h;
}

void routes() {
  g_server.on("/", HTTP_GET, [](PsychicRequest* r) {
    return r->reply(200, "text/html", page().c_str());
  });
  g_server.on("/api/state", HTTP_GET, [](PsychicRequest* r) {
    buildState();
    return r->reply(200, "application/json", g_out.c_str());
  });
  g_server.on("/api/cmd", HTTP_GET, [](PsychicRequest* r) {
    const String msg = cmd(r);
    return r->reply(200, "text/plain", msg.c_str());
  });
}

// The transmitter must not race a flash write, on either layer.
void otaStart() { g_txGate = false; g_link.setTxGate(false); }
void otaEnd(bool) { g_txGate = true;  g_link.setTxGate(true); }

}  // namespace

void setup() {
  Serial.begin(115200);
#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
  delay(1000);          // native USB-CDC re-enumerates after a reboot; 300 ms eats the log
#else
  delay(300);
#endif

  loadCfg();

  // A COMBINATION FROM NVS THAT CANNOT RUN IS CORRECTED HERE, LOUDLY. The command route
  // refuses this pair, so it only arrives from a build that stored it before that check
  // existed — and left alone it would install esp32_can in NORMAL mode, send with no self
  // flag, receive nothing and report a dead board. The layer gives way rather than the mode:
  // selftest and listen have no meaning without the raw driver, so honouring the mode is the
  // only reading of the request that runs at all.
  if (g_cfg.layer == Layer::Link && modeNeedsRawLayer(g_cfg.mode)) {
    g_cfg.layer = Layer::Twai;
    g_note = "layer forced to twai: the stored mode cannot run on the link layer";
  }

  // THE PIN, BEFORE ANYTHING OWNS IT. This has to happen ahead of every driver and ahead of
  // WiFi — a blocking join once banked 372 000 bogus bus errors before the CAN driver had
  // even started, and read as a dead controller.
  pinMode(kRxPin, INPUT);
  countEdges(1000, g_bootHi, g_bootEdges, g_bootN);

  g_busUp = busBegin();

  Serial.printf("\n[cantest%c] layer=%s mode=%s role=%s %lu bit/s rx=%d tx=%d  bus=%s\n",
                'A' + AFFA_CANTEST_NODE, layerName(g_cfg.layer), modeName(g_cfg.mode),
                roleName(g_cfg.role), static_cast<unsigned long>(g_cfg.bitrate),
                static_cast<int>(kRxPin), static_cast<int>(kTxPin), g_busUp ? "up" : "DOWN");
  Serial.printf("[cantest%c] boot probe: %lu edges, %lu/%lu high\n", 'A' + AFFA_CANTEST_NODE,
                static_cast<unsigned long>(g_bootEdges), static_cast<unsigned long>(g_bootHi),
                static_cast<unsigned long>(g_bootN));

  const char* ap = AFFA_CANTEST_NODE ? "AffaCanB" : "AffaCanA";
  affanet::startWifi(ap, "affa1234", AFFA_CANTEST_NODE ? "affacanb" : "affacana", "cantest");
  affanet::startHttp(g_server, otaStart, otaEnd, routes);
}

void loop() {
  ElegantOTA.loop();

  uint32_t id = 0; uint8_t d[8] = {0}, len = 0;
  while (busRecv(id, d, len)) onFrame(id, d, len);

  tick();

  if (g_rebootAt && static_cast<int32_t>(::millis() - g_rebootMs) >= 0) ESP.restart();
  delay(1);
}
