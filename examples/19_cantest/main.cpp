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
//   0. mode=loop FIRST, WHEN THE FIRMWARE ITSELF IS IN QUESTION. The controller's receive
//      input is taken from its own transmit pad through the GPIO matrix, so the frame never
//      leaves the die and there is nothing external left to blame. `matched` climbing clears
//      the silicon, the bit timing and every line of the install; `matched` at zero means the
//      fault is in this file and no amount of probing wires will find it. Added the day two
//      boards failed step 1 together on a bus measured good at 60 ohm.
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
//        op=mode&v=selftest|pingpong|listen|loop                          (NVS, reboots)
//        op=bitrate&v=125000|250000|500000|1000000                        (NVS, reboots)
//        op=role&v=a|b             who originates and who echoes          (NVS, live)
//        op=rate&ms=               ping period                            (NVS, live)
//        op=reset                  zero the app counters
//        op=sample&ms=             live edge count on CRX
//        op=wifi&ssid=&pass=       store credentials (bare op=wifi reports the SSID)
//        op=swap&on=0|1        exchange rx and tx                     (NVS, reboots)
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
#include <driver/rtc_io.h>   // an RTC pad can be latched THROUGH a reboot — see releasePads()
#include <esp_rom_gpio.h>
#include <soc/gpio_sig_map.h>
#include <soc/gpio_reg.h>   // GPIO_IN_REG — op=scan samples every pin in the same instant
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

// NOT constexpr, because `op=swap` decides them at boot. The pin PAIR has been read the wrong
// way round twice on this bench — once off a silkscreen, once out of a working driver for a
// mirror-soldered board — and each time it cost hours of arguing with instruments. A swapped
// pair is indistinguishable from a dead transceiver in every counter here: driving the pin
// that is really R fights a push-pull output and loses, and listening on the pin that is
// really D hears the module's own protective pull-up and nothing else. So it is a switch, not
// a rebuild.
constexpr gpio_num_t kRxBuild = static_cast<gpio_num_t>(AFFA_CAN_RX);
constexpr gpio_num_t kTxBuild = static_cast<gpio_num_t>(AFFA_CAN_TX);
gpio_num_t kRxPin = kRxBuild;
gpio_num_t kTxPin = kTxBuild;

// Two ids, low enough to win arbitration against anything a panel sends, and OUTSIDE every id
// the AffaDisplay protocol uses (0x121/0x151/0x1B1/0x1F1/0x3AF/0x3DF and the key ids). If one
// of these ever appears on a car's bus it is ours.
constexpr uint32_t kIdA = 0x100;   // role A originates here
constexpr uint32_t kIdB = 0x101;   // role B echoes here

constexpr const char* kNvs = "affacan";

enum class Layer : uint8_t { Twai = 0, Link = 1 };
// `Loop` is `SelfTest` with the outside world removed — see busBegin(). Both send with a
// self-reception request; only the route back differs.
enum class Mode  : uint8_t { SelfTest = 0, PingPong = 1, Listen = 2, Loop = 3 };
enum class Role  : uint8_t { A = 0, B = 1 };

struct Cfg {
  // SELFTEST IS THE BOOT DEFAULT because it is step 1 and it needs no peer: a board flashed
  // for the first time answers "is this ESP alive" before anything else is even connected.
  Layer    layer   = Layer::Twai;
  Mode     mode    = Mode::SelfTest;
  Role     role    = AFFA_CANTEST_NODE ? Role::B : Role::A;
  uint32_t bitrate = 500000;
  uint16_t rateMs  = 100;
  bool     swap    = false;      // exchange the rx/tx pins the build asked for
};
Cfg g_cfg;

const char* layerName(Layer l) { return l == Layer::Link ? "link" : "twai"; }
const char* modeName(Mode m) {
  return m == Mode::PingPong ? "pingpong" : m == Mode::Listen ? "listen"
       : m == Mode::Loop     ? "loop"     : "selftest";
}
// The two modes where the node talks to itself and the answer must come back on the watched
// id. They differ only in how far the signal travels to get there.
bool selfMode(Mode m) { return m == Mode::SelfTest || m == Mode::Loop; }
const char* roleName(Role r) { return r == Role::B ? "b" : "a"; }

void loadCfg() {
  Preferences p;
  if (!p.begin(kNvs, true)) return;
  g_cfg.layer   = static_cast<Layer>(p.getUChar("layer", static_cast<uint8_t>(g_cfg.layer)));
  g_cfg.mode    = static_cast<Mode>(p.getUChar("mode", static_cast<uint8_t>(g_cfg.mode)));
  g_cfg.role    = static_cast<Role>(p.getUChar("role", static_cast<uint8_t>(g_cfg.role)));
  g_cfg.bitrate = p.getULong("bitrate", g_cfg.bitrate);
  g_cfg.rateMs  = static_cast<uint16_t>(p.getUShort("rate", g_cfg.rateMs));
  g_cfg.swap    = p.getBool("swap", g_cfg.swap);
  p.end();
  // Resolved once, here, so every probe and both layers see the same pair. Doing it at each
  // use site is how one of them ends up reading the pin nothing is wired to.
  kRxPin = g_cfg.swap ? kTxBuild : kRxBuild;
  kTxPin = g_cfg.swap ? kRxBuild : kTxBuild;
}

void saveCfg() {
  Preferences p;
  if (!p.begin(kNvs, false)) return;
  p.putUChar("layer", static_cast<uint8_t>(g_cfg.layer));
  p.putUChar("mode", static_cast<uint8_t>(g_cfg.mode));
  p.putUChar("role", static_cast<uint8_t>(g_cfg.role));
  p.putULong("bitrate", g_cfg.bitrate);
  p.putUShort("rate", g_cfg.rateMs);
  p.putBool("swap", g_cfg.swap);
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

// THE EXTERNAL LOOP, MEASURED AGAINST AN OPPOSING PULL — the part that makes it trustworthy.
//
// The obvious version of this test drives D and watches R, and it has already lied on this
// bench: it read "no response" on a link that was working, and on that reading a healthy
// transceiver was declared dead. The reason it lies is that a floating pin reads whatever it
// last saw, so "R did not follow" and "R is not connected" produce the same number.
//
// So each direction is sampled with the internal pull-up or pull-down fighting the expected
// answer. A transceiver's R is push-pull and beats 45 kohm without noticing; a floating or
// high-Z pin loses to it every time. That turns an ambiguous reading into a yes/no:
//
//   dom = 0   with D driven LOW, R stayed LOW against a PULL-UP    -> the loop echoes. Healthy.
//   dom = 100 with D driven LOW, the pull-up won                   -> nothing drives R.
//   rec = 100 with D released HIGH, R stayed HIGH against a PULL-DOWN -> R drives recessive.
//   rec = 0   with D released HIGH, the pull-down won              -> nothing drives R.
//
// AND THE DOMINANT IS ASKED FOR AS A SQUARE WAVE, NEVER AS A HELD LEVEL. This transceiver
// family implements a TXD DOMINANT TIMEOUT of roughly 1-4 ms: hold D low longer than that and
// it disarms its own driver, by design, so that one stuck node cannot jam a bus. A probe that
// drives D low and then samples for milliseconds is therefore measuring a driver that has
// already switched itself off, and it reports a healthy transceiver as dead.
//
// That is not a hypothetical. It invalidated a whole session's "the transmit path is
// physically broken" verdict once (docs and notes both record the retraction), and it
// invalidated the first version of THIS function too — every `dom` reading it produced was
// the timeout, not the part. So the dominant is now toggled at ~10 kHz, each one 50 us long
// and far inside the timeout, and what is counted is EDGES on R:
//
//   edges ~200 — R followed D through the transceiver and the bus. Healthy.
//   edges 0    — nothing came back, and this time the timeout cannot be the reason.
//   rec = 100  — with D released, R holds recessive against a PULL-DOWN. The receiver drives.
//   rec = 0    — nothing drives R at all: standby, no supply, or an open R line.
//
// Run at boot, before any driver owns the pins. It disturbs a bench bus for about 20 ms.
uint32_t g_xEdges = 0;
uint8_t  g_xRec   = 0;

void probeExternalLoop() {
  pinMode(kTxPin, OUTPUT);

  // Recessive first, and this one IS a static test — no driver is being asked for, so no
  // timeout can spoil it. It is the reading that survived the first version of this probe.
  digitalWrite(kTxPin, HIGH);
  pinMode(kRxPin, INPUT_PULLDOWN);
  delayMicroseconds(500);
  for (uint8_t i = 0; i < 100; ++i) { if (digitalRead(kRxPin)) ++g_xRec; delayMicroseconds(50); }

  // Then the dominant, as a wave.
  pinMode(kRxPin, INPUT_PULLUP);
  delayMicroseconds(200);
  int last = digitalRead(kRxPin);
  for (uint16_t i = 0; i < 400; ++i) {
    digitalWrite(kTxPin, (i & 1) ? HIGH : LOW);
    delayMicroseconds(50);
    const int v = digitalRead(kRxPin);
    if (v != last) { ++g_xEdges; last = v; }
  }

  digitalWrite(kTxPin, HIGH);
  pinMode(kRxPin, INPUT);
  pinMode(kTxPin, INPUT);
}

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

// SOMETHING HOLDS THE LINE AT STARTUP — and on these parts something literally can.
//
// Both problem pads here are RTC-capable (ESP32-C3 GPIO0..5; classic ESP32 GPIO4 among
// others), and an RTC pad can be LATCHED. gpio_hold_en() and the deep-sleep hold freeze a
// pad's level AND its routing, and that latch SURVIVES A SOFTWARE RESET. An OTA is a software
// reset. A power cycle is not. So a pad held once stays held across every reflash until the
// board is physically unplugged — which is exactly what "it works sometimes, and today it
// suddenly worked" looks like from the outside, and exactly what no continuity test can find.
//
// Worse: a pad the RTC IO MUX still owns is disconnected from the DIGITAL GPIO matrix
// altogether. The wire is fine, the pad is fine, and TWAI_RX_IDX is listening to something
// that was never connected to it.
//
// The failing halves on this bench line up with it exactly — C3 receive on GPIO3 (RTC),
// DevKit transmit on GPIO4 (RTC), and the one half that has never once failed is the DevKit's
// receive on GPIO5, which is NOT an RTC pin on the classic ESP32.
//
// So the latch is broken and the pad handed back to the digital side BEFORE anything reads
// it — before the boot probes, before any driver, before WiFi.
void releasePads() {
  gpio_deep_sleep_hold_dis();
  const gpio_num_t pins[2] = { kRxPin, kTxPin };
  for (gpio_num_t p : pins) {
    // The one that matters on the C3, and the only RTC facility it has for these pads: it
    // supports the HOLD latch but has no RTC IO mux to take a pin away in the first place.
    gpio_hold_dis(p);
#if defined(SOC_RTCIO_INPUT_OUTPUT_SUPPORTED) && SOC_RTCIO_INPUT_OUTPUT_SUPPORTED
    // The classic ESP32 does have one, and a pad it still owns is invisible to the digital
    // GPIO matrix — so hand it back explicitly. Absent on the C3, hence the guard.
    if (rtc_gpio_is_valid_gpio(p)) rtc_gpio_deinit(p);
#endif
  }
}

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

  const twai_mode_t mode = g_cfg.mode == Mode::Listen ? TWAI_MODE_LISTEN_ONLY
                         : selfMode(g_cfg.mode)       ? TWAI_MODE_NO_ACK
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

  // THE INTERNAL LOOPBACK, and it is the only test here that can fail for exactly one reason.
  //
  // Every other mode routes the answer through the transceiver, two signal wires, the bus pair
  // and back — so a failure has half a dozen candidates and the argument never ends. This one
  // takes the controller's RX input from its OWN TX PAD through the GPIO matrix. The frame
  // never leaves the die. Nothing outside the chip is in the path: not the transceiver, not
  // CANH/CANL, not a joint, not termination, not the peer.
  //
  // So the reading is absolute. `matched` climbing means the TWAI controller, the bit timing,
  // NO_ACK mode, the self-reception request and every line of the install above are correct,
  // and any remaining fault is outside the chip. `matched` at zero means the fault is IN HERE
  // — in this file or in the silicon — and no amount of probing wires will find it.
  //
  // The TX pad keeps driving the transceiver's D input while this runs. That input is high-Z
  // and cannot fight back, so the loop costs the bus nothing and needs no spare pin.
  if (g_cfg.mode == Mode::Loop) {
    gpio_set_direction(kTxPin, GPIO_MODE_INPUT_OUTPUT);   // read the pad we are driving
    // AND PUT THE PERIPHERAL'S OWN ROUTE BACK, because gpio_set_direction() just took it away.
    // It re-points the pad's OUTPUT at plain GPIO, which parks it at whatever the output
    // register holds — zero — so the controller reads a permanently dominant line, never sees
    // bus-idle and never starts a transmission. Measured before this line existed: state
    // RUNNING, `queuedTx` frozen at 33, and every single error counter at zero. That trio is
    // worth memorising; it is what "the controller never even tried" looks like, and it is
    // indistinguishable from a dead peer if you only read the app's own counters.
    esp_rom_gpio_connect_out_signal(kTxPin, TWAI_TX_IDX, false, false);
    esp_rom_gpio_connect_in_signal(kTxPin, TWAI_RX_IDX, false);
    return true;                                          // deliberately NOT settlePads()
  }

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
  if (selfMode(g_cfg.mode)) return txId();
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
  if (busSend(txId(), d, 8, selfMode(g_cfg.mode))) { ++g_c.sent; ++g_nextSeq; }
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
  jf(",\"bitrate\":%lu,\"rateMs\":%u,\"rxPin\":%d,\"txPin\":%d,\"swap\":%s,\"busUp\":%s",
     static_cast<unsigned long>(g_cfg.bitrate), g_cfg.rateMs, static_cast<int>(kRxPin),
     static_cast<int>(kTxPin), g_cfg.swap ? "true" : "false", g_busUp ? "true" : "false");
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
  // edges ~200 and rec 100 is a transceiver echoing D onto R. Zeros mean it drives nothing.
  jf(",\"xloop\":{\"edges\":%lu,\"rec\":%u}", static_cast<unsigned long>(g_xEdges),
     static_cast<unsigned>(g_xRec));
  // Which pads could have been latched through the last reboot at all. A pin that says false
  // here cannot be the RTC-hold fault, whatever else it is doing.
  jf(",\"rtcPad\":{\"rx\":%s,\"tx\":%s}",
     rtc_gpio_is_valid_gpio(kRxPin) ? "true" : "false",
     rtc_gpio_is_valid_gpio(kTxPin) ? "true" : "false");
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
      else if (v == "loop") g_cfg.mode = Mode::Loop;
      else return "mode: selftest|pingpong|listen|loop";
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
    // CHUNKED, WITH A YIELD BETWEEN CHUNKS — same reason as op=hold. This is a busy spin on
    // digitalRead() inside the web server's task, and a spin that outlasts the 3 s socket
    // timeout takes HTTP down with it. A handful of edges are missed during each yield, which
    // costs nothing: the question this answers is zero versus tens of thousands.
    const uint32_t ms = static_cast<uint32_t>(constrain(qn(r, "ms", 1000), 10L, 5000L));
    uint32_t hi = 0, edges = 0, n = 0;
    for (uint32_t done = 0; done < ms; done += 100) {
      uint32_t h = 0, e = 0, c = 0;
      countEdges(ms - done < 100 ? ms - done : 100, h, e, c);
      hi += h; edges += e; n += c;
      delay(1);
    }
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

  // Exchange rx and tx and reboot. `xloop` answers it in one boot: a correctly-ordered pair
  // reads 0/100, and a swapped one reads 100/100 — which is also what a standby transceiver
  // and an open CTX read, so this is the switch that tells the three apart.
  if (op == "swap") {
    const String on = q(r, "on", "1");
    g_cfg.swap = !(on == "0" || on == "off" || on == "false");
    saveCfg();
    rebootSoon();
    return String("swap=") + (g_cfg.swap ? "on" : "off") + " — rebooting";
  }

  // HOLD THE BUS DOMINANT FROM THIS NODE, so the OTHER one can say whether it arrived.
  //
  // This is the cross-node test, and it exists because each board here has one half that is
  // proven good and one that is not. It pairs them: this node drives D low with plain GPIO —
  // its transmit path — while the peer runs op=sample on its own CRX, which is its receive
  // path. A peer that reports `hi` near zero saw the dominant, and the bus can carry one. A
  // peer still reading all-high means the dominant never formed, whoever is at fault.
  //
  // No self-measurement anywhere in that sentence, which is the point: every instrument that
  // asks a node about itself has been ambiguous tonight.
  // TOGGLED, NEVER HELD — see probeExternalLoop(). A held dominant disarms the transceiver's
  // own driver within a millisecond or so and the peer correctly reports a quiet bus, which
  // has already been mistaken for a broken transmit path once on this bench and once tonight.
  if (op == "hold") {
    const uint32_t ms = static_cast<uint32_t>(constrain(qn(r, "ms", 2000), 50L, 5000L));
    pinMode(kTxPin, OUTPUT);
    const uint32_t until = ::millis() + ms;
    // IN BURSTS, WITH A YIELD BETWEEN THEM, and that yield is not a nicety. This runs inside
    // the web server's own task, and delayMicroseconds() is a BUSY SPIN — it never hands the
    // scheduler back. A handler that spins for four seconds starves the network stack for
    // four seconds, stale sockets pile up against a 3 s timeout and seven slots, and the board
    // stops answering HTTP entirely while still replying to ping and still accepting TCP
    // connections. It looks exactly like a crash and is not one; it cost this DevKit a power
    // cycle. delay() yields where delayMicroseconds() does not, which is the whole difference
    // between this and 17_mediascreen's op=wiggle running happily for a minute.
    while (static_cast<int32_t>(::millis() - until) < 0) {
      const uint32_t burst = ::millis() + 200;
      while (static_cast<int32_t>(::millis() - burst) < 0) {
        digitalWrite(kTxPin, LOW);      // ask for a dominant, briefly
        delayMicroseconds(50);
        digitalWrite(kTxPin, HIGH);
        delayMicroseconds(50);
      }
      delay(2);                         // let the server breathe; the peer never notices
    }
    digitalWrite(kTxPin, HIGH);
    // AND HAND THE PAD BACK TO THE CONTROLLER. pinMode() re-points the pad's output at plain
    // GPIO; without this the transmitter is silently disconnected until the next boot and
    // every reading afterwards is a lie. Learned the hard way an hour ago, in loop mode.
    gpio_set_direction(kTxPin, GPIO_MODE_OUTPUT);
    esp_rom_gpio_connect_out_signal(kTxPin, TWAI_TX_IDX, false, false);
    char b[80];
    snprintf(b, sizeof(b), "toggled gpio%d dominant at 10 kHz for %lu ms", static_cast<int>(kTxPin),
             static_cast<unsigned long>(ms));
    return String(b);
  }

  // WHICH PIN IS REALLY THE RECEIVE LINE? Toggle our own D — the half of this board that is
  // proven to work — and count edges on EVERY pin at once by reading the whole GPIO input
  // register each pass. Nothing is assumed about the silkscreen, the wiring diagram or the
  // build flag: the pin that follows the bus IS the receive line, and if none of them does,
  // the transceiver's R really is not driving and no pin assignment can rescue it.
  //
  // The register read is what makes it honest — every candidate is sampled in the SAME
  // instant, so a pin cannot be missed because it was measured during a quiet moment.
  if (op == "scan") {
    // THE PINS THIS SCAN MUST NOT TOUCH, AND THEY ARE NOT THE SAME PART TO PART. The SPI
    // flash lives on GPIO6-11 on the classic ESP32 and on GPIO11-17 on the C3, and the C3
    // puts native USB on 18/19. The first version of this list was written for the C3 and run
    // on a DevKit, which quietly reconfigured six live flash pins mid-execution; the board
    // survived and returned an empty reply, which is luckier than it deserved.
#if defined(CONFIG_IDF_TARGET_ESP32C3) && CONFIG_IDF_TARGET_ESP32C3
    static const uint8_t skip[] = { 11, 12, 13, 14, 15, 16, 17, 18, 19 };
#else
    static const uint8_t skip[] = { 6, 7, 8, 9, 10, 11 };
#endif
    uint32_t mask = 0;
    for (uint8_t p = 0; p <= 21; ++p) {
      bool bad = (p == static_cast<uint8_t>(kTxPin));
      for (uint8_t s : skip) if (p == s) bad = true;
      if (bad) continue;
      if (!GPIO_IS_VALID_GPIO(p)) continue;
      pinMode(p, INPUT_PULLUP);
      mask |= (1u << p);
    }

    uint16_t edges[22] = {0};
    pinMode(kTxPin, OUTPUT);
    uint32_t prev = REG_READ(GPIO_IN_REG);
    for (uint16_t i = 0; i < 600; ++i) {
      digitalWrite(kTxPin, (i & 1) ? HIGH : LOW);
      delayMicroseconds(50);                       // inside the TXD dominant timeout
      const uint32_t now = REG_READ(GPIO_IN_REG);
      uint32_t diff = (now ^ prev) & mask;
      while (diff) {
        const uint8_t b = static_cast<uint8_t>(__builtin_ctz(diff));
        if (b < 22) ++edges[b];
        diff &= diff - 1;
      }
      prev = now;
    }
    digitalWrite(kTxPin, HIGH);
    gpio_set_direction(kTxPin, GPIO_MODE_OUTPUT);
    esp_rom_gpio_connect_out_signal(kTxPin, TWAI_TX_IDX, false, false);   // give the pad back

    String out = String("toggled gpio") + static_cast<int>(kTxPin) + " 300 times; edges seen:";
    bool any = false;
    for (uint8_t p = 0; p < 22; ++p)
      if (edges[p]) { out += " gpio" + String(p) + "=" + String(edges[p]); any = true; }
    if (!any) out += " NONE — no pin on this board follows the bus";
    return out;
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
  row(h, "xloop edges/rec", String(g_xEdges) + " / " + String(g_xRec),
      g_xEdges > 50 && g_xRec == 100
        ? "R follows D through the transceiver, and holds recessive against a pull-down. Healthy."
        : (g_xEdges == 0 && g_xRec == 0
             ? "R drives NOTHING. Standby (Rs pin), no supply, or an open R line."
             : "D is toggled at 10 kHz so the TXD dominant timeout cannot spoil it; "
               "~200 edges and rec 100 is healthy"));
  h += "</table>";

  h += "<h2>switches</h2>";
  h += "<p>layer <a href='/api/cmd?op=layer&v=twai'>twai</a>"
       "<a href='/api/cmd?op=layer&v=link'>link</a>"
       " &nbsp; mode <a href='/api/cmd?op=mode&v=selftest'>selftest</a>"
       "<a href='/api/cmd?op=mode&v=pingpong'>pingpong</a>"
       "<a href='/api/cmd?op=mode&v=listen'>listen</a>"
       "<a href='/api/cmd?op=mode&v=loop'>loop (internal)</a>"
       " &nbsp; role <a href='/api/cmd?op=role&v=a'>A</a><a href='/api/cmd?op=role&v=b'>B</a></p>";
  h += "<p>bitrate <a href='/api/cmd?op=bitrate&v=125000'>125k</a>"
       "<a href='/api/cmd?op=bitrate&v=250000'>250k</a>"
       "<a href='/api/cmd?op=bitrate&v=500000'>500k</a>"
       "<a href='/api/cmd?op=bitrate&v=1000000'>1M</a>"
       " &nbsp; <a href='/api/cmd?op=swap&on=1'>swap pins</a>"
       "<a href='/api/cmd?op=swap&on=0'>unswap</a>"
       "<a href='/api/cmd?op=reset'>reset counters</a>"
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
  // A FULL SECOND BEFORE ANYTHING AT ALL — before Serial, before the config, before a pin is
  // read. Every rail on this bench gets to settle first, the transceiver included, and none
  // of the probes below can catch a supply on its way up and report it as a dead line.
  //
  // It costs a second of boot on a diagnostic tool and nothing else. A settling-time
  // dependency is also exactly the shape of a fault that "works sometimes", so having it
  // unconditionally at the very top removes the question rather than leaving it open.
  delay(1000);

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
  // BEFORE ANY PROBE READS A PIN. If a pad is latched from a previous image, every number
  // below is a measurement of the latch and not of the wire.
  releasePads();

  pinMode(kRxPin, INPUT);
  countEdges(1000, g_bootHi, g_bootEdges, g_bootN);
  probeExternalLoop();

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
