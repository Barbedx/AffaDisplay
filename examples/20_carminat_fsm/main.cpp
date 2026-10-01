// 20_carminat_fsm — the Carminat opening and the clock, as a REACTIVE state machine.
//
// WHAT IT IS FOR. src/ implements this protocol properly: a pull port, a transmit queue,
// ISO-TP, coalescing, ACK timeouts, link recovery, an owned task. That is the right thing to
// ship and the wrong thing to read when the question is only "what does the panel need to
// hear, and when?". This file answers that question and nothing else, in one page, on
// collin80's can_common / esp32_can. No WiFi, no OTA, no HTTP, no library FSM — the library
// is included for its CONSTANTS so the bytes have one home, and for nothing else.
//
// THE RULE THE FILE IS BUILT AROUND: WE NEVER INITIATE. Every protocol frame here is
// transmitted from inside the CAN receive callback, in answer to a frame the panel sent.
// There is no periodic protocol task, no free-running heartbeat and no retry loop; if the
// panel goes quiet, so do we, for ever. The one thing loop() polls on a timer is the CAN
// controller's own health (recoverLink()), and that puts nothing on the bus. Two deliberate
// exceptions to the rule, both named:
//
//   * `BA`, the announce — the one frame the library sends into a silent bus. It is OFF here
//     and stays off unless the `ba` console command turns it on; the choice is kept in NVS so
//     it survives the reflash. sendAnnounce() below says what it is for.
//   * the console commands. A human typing `t` is an initiation; that is the point of them.
//
// THE EXCHANGE, top to bottom. Left is the panel, right is what this program answers with.
//
//   3CF 69                 ->  3AF B9 00                       ping; alive, paced 250 ms
//   3CF 61 11 xx           ->  3AF B0 14 11 00 1F 00 00 00 x3   the hello burst
//   1C1 70                 ->  5C1 74                          the display opens ITS channel
//                              151 70                          ...and only then do we open ours
//   551 74                 ->  1F1 70                          the second function
//   5F1 74                 ->  (settle ~400 ms — see below)
//   3CF 69 (the next one)  ->  151 03 52 09                    light the glass
//   551 74                 ->  151 05 56 31 30 30 30           the clock — hardcoded 10:00
//   551 74                 ->  (done)
//
// WE REGISTER AFTER THE DISPLAY DOES, and that ordering is measured 4/4 in the OEM captures,
// not a preference: the display's own `1C1 70` lands between B0#1 and B0#2, we answer `5C1 74`
// within half a millisecond, and the radio's `151 70` follows ~61 ms later. Registering off
// the hello alone gets the order right only by luck.
//
// WHAT IS SIMPLER HERE THAN IN THE LIBRARY, and where that will bite:
//   * the three B0 frames go out back to back. The captured OEM radio paces them 31 ms apart
//     (carminat::kSync); MeganeCAN's proven sender did not (kLegacyMeganeCanSync, gaps 0), and
//     a receive callback is the wrong place to sleep. This takes the legacy spelling.
//   * every payload below is 8 bytes or shorter, so there is NO ISO-TP in this file at all.
//     setText is 20+ bytes and multi-frame, which is exactly why setText is not here.
//   * nothing is retried, and esp32_can's sendFrame() returns true whatever happened — a frame
//     lost to a full controller queue is simply lost. `s` prints txErr/busErr; that is the
//     only place a loss shows up.
//   * the FSM runs on esp32_can's callback task and the console on loopTask. The shared state
//     is `volatile` and nothing more — no queue, no mutex. The console can write `g_step` from
//     under the callback (`r`), and the worst that costs is one confused line of output. A
//     program with real work in it does not get to be this casual.
//
// IF NOTHING ARRIVES AT ALL after a flash: power-cycle the DISPLAY, not the board. It sleeps
// when the bus resets underneath it and never speaks again until it is woken — and since this
// program only answers, a sleeping display means a permanently silent bus.
//
//   pio run -e ex20_carminat_fsm_c3     -t upload -t monitor   C3 SuperMini, rx 3 / tx 4
//   pio run -e ex20_carminat_fsm_devkit -t upload -t monitor   DevKit V1,    rx 5 / tx 4

#include <Arduino.h>
#include <Preferences.h>
#include <esp32_can.h>
#include <driver/twai.h>
#include <esp_rom_gpio.h>          // the GPIO matrix, wired by hand in setup() — see there
#include <soc/gpio_sig_map.h>

#include <AffaDisplay.h>           // CONSTANTS ONLY: no display object is ever constructed

#if !AFFA_PANEL_CARMINAT
#  error "20_carminat_fsm is a Carminat example: build with -D AFFA_PANEL_CARMINAT=1"
#endif

namespace cm = affa::carminat;

// The pins come from the env, because they differ per board and a welded pin is invisible in
// every other counter. rx is the transceiver's R, tx its D — swap them and you get a link
// that never errors and never receives, which reads as a dead panel.
#ifndef AFFA_CAN_RX
#  define AFFA_CAN_RX 3
#endif
#ifndef AFFA_CAN_TX
#  define AFFA_CAN_TX 4
#endif

namespace {

constexpr gpio_num_t kRxPin   = static_cast<gpio_num_t>(AFFA_CAN_RX);
constexpr gpio_num_t kTxPin   = static_cast<gpio_num_t>(AFFA_CAN_TX);
constexpr uint32_t   kBitrate = 500000;

// HARDCODED, and this example will never do anything else. "HHMM", four ASCII digits: 10:00.
// A real clock is an application concern — an RTC, NTP, the car's own time — and putting one
// here would be the second thing in the file to read as protocol when it is not.
constexpr char kClock[4] = {'1', '0', '0', '0'};

// A panel that has not acknowledged us repeats its ping at line rate — 1472 frames/s measured
// on this bench. One reply per copy is a transmit storm, so the pong has a floor, and it can
// only ever make us send LESS.
constexpr uint32_t kPongMinMs = AFFA_PING_REPLY_MIN_MS;   // 250

// THE QUIET INTERVAL AFTER REGISTRATION. The captured OEM radio waits ~400 ms after the last
// `70` is acknowledged before its first payload, and a message sent inside that window is one
// the panel takes and does not draw — a blank screen with every frame acknowledged, which is
// the most expensive way this protocol fails. A program with no timers cannot wait, so the
// wait is hung on the panel's own voice instead: the first frame to arrive past the interval
// carries the work. The display pings every ~500 ms, so the cue is never far away.
constexpr uint32_t kSettleMs = cm::kPayloadAfterRegistrationMs;   // 400

// ---------------------------------------------------------------------------
// State. One step per outstanding ACK, which is the whole FSM: each arriving 74 is what
// advances it, so the sequence cannot run ahead of the panel.
// ---------------------------------------------------------------------------
enum class Step : uint8_t {
  Silent,    // nothing heard yet — we have transmitted nothing
  Hello,     // the B0 burst is out; waiting for the display to open its own channel
  Reg151,    // 151 70 out, waiting for 551 74
  Reg1F1,    // 1F1 70 out, waiting for 5F1 74
  Settling,  // registered; holding the quiet interval, released by the next frame in
  Power,     // 151 03 52 09 out, waiting for 551 74
  Clock,     // 151 05 56 .. out, waiting for 551 74
  Done,
};

const char* stepName(Step s) {
  switch (s) {
    case Step::Silent:   return "Silent";
    case Step::Hello:    return "Hello";
    case Step::Reg151:   return "Reg151";
    case Step::Reg1F1:   return "Reg1F1";
    case Step::Settling: return "Settling";
    case Step::Power:    return "Power";
    case Step::Clock:    return "Clock";
    case Step::Done:     return "Done";
  }
  return "?";
}

volatile Step     g_step        = Step::Silent;
volatile bool     g_peerSeen    = false;   // the display sent its own 1C1 70
volatile bool     g_announce    = false;   // may we send BA at all? NVS-backed, off by default
volatile bool     g_announced   = false;   // ...and it is a ONE-SHOT within a session
volatile bool     g_verbose     = true;    // print every received frame
volatile uint32_t g_rx = 0, g_tx = 0, g_keys = 0;
uint32_t          g_nextPongMs  = 0;       // callback task only
uint32_t          g_settledMs   = 0;       // callback task only: when Settling may end

Preferences g_nvs;

// ---------------------------------------------------------------------------
// Transmit. Every payload shorter than 8 bytes is padded with the family filler, which for
// Carminat is 0x00 — so `70` on the wire is `70 00 00 00 00 00 00 00`, as the captures show.
// ---------------------------------------------------------------------------
void tx(uint16_t id, const uint8_t* payload, uint8_t n, const char* why) {
  CAN_FRAME f;
  f.id       = id;
  f.extended = 0;
  f.rtr      = 0;
  f.length   = affa::kPacketLength;
  for (uint8_t i = 0; i < affa::kPacketLength; ++i)
    f.data.uint8[i] = i < n ? payload[i] : cm::kFiller;
  CAN0.sendFrame(f);          // always returns true — see the header note on losses
  ++g_tx;
  Serial.printf("      TX %03X  %02X %02X %02X %02X %02X %02X %02X %02X   %s\n",
                static_cast<unsigned>(id), f.data.uint8[0], f.data.uint8[1], f.data.uint8[2],
                f.data.uint8[3], f.data.uint8[4], f.data.uint8[5], f.data.uint8[6],
                f.data.uint8[7], why);
}

// `3AF BA 00` — THE ANNOUNCE, and the only frame in the repertoire that asks rather than
// answers. The display replies to it with its NEXT `61 11`, and per the captures only that one
// draws a burst that opens the panel's own 1C1 channel. Off by default here because this
// example is about the answering half; turn it on with `ba` when the display is asleep and
// something has to call it back.
void sendAnnounce() {
  const uint8_t d[] = {cm::kSync.requestByte, cm::kSync.requestArg};
  tx(cm::kIdSync, d, sizeof(d), "BA announce");
}

// `3AF B9 00` — alive. In the OEM captures this is a free-running 500 ms timer and NOT a
// reply; here it is a reply, which is MeganeCAN's proven spelling and the only one available
// to a program with no timers.
void sendAlive() {
  const uint8_t d[] = {cm::kSync.aliveByte, 0x00};
  tx(cm::kIdSync, d, sizeof(d), "B9 alive (pong)");
}

// `3AF B0 14 11 00 1F 00 00 00`, three times. The second and third are identical; that is not
// a typo and it is not deduplicated — it is what the capture holds.
void sendHello() {
  for (uint8_t i = 0; i < cm::kSync.helloCount; ++i)
    tx(cm::kIdSync, cm::kSync.hello[i], affa::kPacketLength, "B0 hello");
}

// The control ACK on id|0x400. `1C1 -> 5C1`, answered in 0.25–0.48 ms in every OEM capture.
void sendAck(uint16_t id) {
  const uint8_t d[] = {affa::kAckDone};
  tx(static_cast<uint16_t>(id | affa::kReplyFlag), d, sizeof(d), "74 ack");
}

void sendRegister(uint16_t funcId) {
  const uint8_t d[] = {affa::kRegisterByte};
  tx(funcId, d, sizeof(d), "70 register function");
}

void sendPower(bool on) {
  const uint8_t d[] = {0x03, cm::kCmdCtrl, on ? cm::kDisplayCtrlOn : cm::kDisplayCtrlOff};
  tx(cm::kIdDisplayCtrl, d, sizeof(d), on ? "52 power on" : "52 power off");
}

// `05 56 H H M M 00 00` — 0x05 is SF_DL, five payload bytes follow. Single frame, which is why
// the clock is the one screen command a file this size can carry.
void sendClock() {
  const uint8_t d[] = {0x05, cm::kCmdClock,
                       static_cast<uint8_t>(kClock[0]), static_cast<uint8_t>(kClock[1]),
                       static_cast<uint8_t>(kClock[2]), static_cast<uint8_t>(kClock[3])};
  tx(cm::kIdSetText, d, sizeof(d), "56 clock 10:00");
}

// ---------------------------------------------------------------------------
// The receive callback — every transition in the program lives below this line.
//
// It runs on esp32_can's own task (priority 15, 8 kB stack), NOT in an ISR, so transmitting
// from it is legal; sendFrame() blocks at most 4 ms on a full queue. The library refuses to do
// protocol work here on purpose — see ICanLink's note on the ACK deadlock — and a program that
// grows past this one should adopt the ring-and-drain shape of link/CanCommonLink.h.
// ---------------------------------------------------------------------------

void startRegistration() {
  sendRegister(cm::kIdSetText);      // 151 first: ORDER IS ON THE WIRE
  g_step = Step::Reg151;
}

// `3CF 61 11 xx`. ANY complete request is the same request — byte 2 is the display reporting
// its own state, not an authorization grade, and a session in the corpus completes on sixteen
// `01`s without one `00`.
void onSyncRequest(const CAN_FRAME* f) {
  if (f->length < 3) return;         // a short 61 11 is a real frame on this channel, and not
                                     // a request; do not read past the DLC for byte 2

  // Arriving once we are past the opening, this says the panel forgot us. Everything behind it
  // is void — start again rather than carrying on talking over it. `g_peerSeen` deliberately
  // survives: the display is demonstrably still talking, and its channel is still open.
  if (g_step > Step::Hello) {
    Serial.printf("      -- 61 11 %02X while at %s: the panel voided us, reopening\n",
                  f->data.uint8[2], stepName(g_step));
    g_step = Step::Silent;
  }

  if (g_announce && !g_announced) {
    g_announced = true;
    sendAnnounce();                  // and NOTHING else: the burst belongs to the NEXT request
    return;
  }

  sendHello();
  g_step = Step::Hello;
  // Usually the display's 1C1 has already arrived — 4/4 in the captures it lands between B0#1
  // and B0#2 — in which case registration starts here. If it has not, onPanelChannel() does.
  if (g_peerSeen) startRegistration();
}

// `3CF 69`. data[0] is the ENTIRE test; nothing else in the frame may be read.
void onPing() {
  const uint32_t now = millis();
  if (static_cast<int32_t>(now - g_nextPongMs) < 0) return;
  g_nextPongMs = now + kPongMinMs;
  sendAlive();
}

// `1C1` — the display's channel. Two different things arrive here and both are acknowledged:
// its registration (`70`) and its key presses (`03 89 ..`). It is NOT a key-only channel, which
// is why the two guard bytes are load-bearing: without them the other traffic on it decodes as
// keys 0x640F and 0x3030.
void onPanelChannel(const CAN_FRAME* f) {
  sendAck(cm::kIdKeyPressed);        // the reflex, before anything is understood about the frame

  if (f->length >= 1 && f->data.uint8[0] == affa::kRegisterByte) {
    g_peerSeen = true;
    if (g_step == Step::Hello) startRegistration();
    return;
  }
  if (f->length >= 4 && f->data.uint8[0] == affa::kKeyFrameByte0 &&
      f->data.uint8[1] == affa::kKeyFrameByte1) {
    ++g_keys;
    Serial.printf("      -- key 0x%04X\n",
                  static_cast<unsigned>((f->data.uint8[2] << 8) | f->data.uint8[3]));
  }
}

// `551` / `5F1` — the panel's verdict on the last thing we sent. THIS is what advances the
// sequence, and the reason nothing here needs a timeout: one outstanding message at a time,
// and the next one is composed by the ACK for the previous.
void onAck(uint16_t id, const CAN_FRAME* f) {
  if (f->length < 1 || f->data.uint8[0] != affa::kAckDone) return;   // 30 01 00 is a partial,
                                                                     // and nothing here is
                                                                     // multi-frame
  const uint16_t ack151 = cm::kIdSetText | affa::kReplyFlag;
  const uint16_t ack1F1 = cm::kIdNav | affa::kReplyFlag;

  switch (g_step) {
    case Step::Reg151:
      if (id != ack151) break;
      sendRegister(cm::kIdNav);
      g_step = Step::Reg1F1;
      break;
    case Step::Reg1F1:
      if (id != ack1F1) break;
      Serial.printf("      == both functions registered - settling %lu ms\n",
                    static_cast<unsigned long>(kSettleMs));
      g_settledMs = millis() + kSettleMs;
      g_step      = Step::Settling;      // released by the next frame in, see onCanFrame()
      break;
    case Step::Power:
      if (id != ack151) break;
      // The panel does not announce that its glass is lit, and the clock can land while it is
      // still coming up. If the display stays blank, type `t`.
      sendClock();
      g_step = Step::Clock;
      break;
    case Step::Clock:
      if (id != ack151) break;
      g_step = Step::Done;
      Serial.println("      == CONNECTED. clock set to 10:00");
      break;
    default:
      break;
  }
}

void dispatch(uint16_t id, const CAN_FRAME* f) {
  // A real controller never hears its own transmissions, so there is no echo to filter here.
  if (id == cm::kIdSyncReply) {
    if (f->length >= 2 && f->data.uint8[0] == affa::kSyncRequestByte0 &&
        f->data.uint8[1] == affa::kSyncRequestByte1) {
      onSyncRequest(f);
    } else if (f->length >= 1 && f->data.uint8[0] == affa::kSyncPeerAlive) {
      onPing();
    }
    return;
  }
  if (id == cm::kIdKeyPressed) { onPanelChannel(f); return; }
  if ((id & affa::kReplyFlag) != 0) { onAck(id, f); return; }
}

void onCanFrame(CAN_FRAME* f) {
  if (!f) return;
  ++g_rx;
  const uint16_t id = static_cast<uint16_t>(f->id);

  if (g_verbose) {
    Serial.printf("RX %03X  ", static_cast<unsigned>(id));
    for (uint8_t i = 0; i < f->length && i < 8; ++i) Serial.printf("%02X ", f->data.uint8[i]);
    Serial.println();
  }

  dispatch(id, f);

  // THE ONE PIECE OF WORK NO PARTICULAR FRAME ASKS FOR. Whatever the panel said, if it said it
  // after the quiet interval, that is the cue to start the payloads — so the wait is still
  // spent listening rather than counting, and a panel that stops talking simply never gets a
  // screen it would not have drawn anyway.
  if (g_step == Step::Settling && static_cast<int32_t>(millis() - g_settledMs) >= 0) {
    sendPower(true);
    g_step = Step::Power;
  }
}

// ---------------------------------------------------------------------------
// The console. Line-based, one letter per command, and the only place a frame leaves this
// program without one arriving first.
// ---------------------------------------------------------------------------
void printHelp() {
  Serial.println(
      "\ncommands:\n"
      "  s     status\n"
      "  t     re-send the clock (10:00)\n"
      "  1 0   power the display on / off\n"
      "  ba    toggle the BA announce, stored in NVS  (default OFF: we only answer)\n"
      "  kick  send ONE BA right now, whatever the switch says\n"
      "  r     forget the session and wait for the panel again\n"
      "  v     toggle the per-frame RX log\n"
      "  ?     this");
}

void printStatus() {
  twai_status_info_t st{};
  const bool ok = twai_get_status_info(&st) == ESP_OK;
  Serial.printf("\nstep %s   peerChannel %s   announce %s%s\n", stepName(g_step),
                g_peerSeen ? "seen" : "-", g_announce ? "on" : "off",
                g_announced ? " (spent)" : "");
  Serial.printf("rx %lu   tx %lu   keys %lu   pins rx=%d tx=%d @ %lu\n",
                static_cast<unsigned long>(g_rx), static_cast<unsigned long>(g_tx),
                static_cast<unsigned long>(g_keys), static_cast<int>(kRxPin),
                static_cast<int>(kTxPin), static_cast<unsigned long>(kBitrate));
  if (!ok) { Serial.println("twai: no status"); return; }
  // state 0 STOPPED / 1 RUNNING / 2 BUS_OFF / 3 RECOVERING. queuedTx growing with every error
  // counter at zero is the one that fools people: the controller is waiting for a bus-idle
  // that never comes because its RX reads permanent dominant.
  Serial.printf("twai: state %d  txErr %lu  rxErr %lu  busErr %lu  arbLost %lu  queuedTx %lu\n",
                static_cast<int>(st.state), static_cast<unsigned long>(st.tx_error_counter),
                static_cast<unsigned long>(st.rx_error_counter),
                static_cast<unsigned long>(st.bus_error_count),
                static_cast<unsigned long>(st.arb_lost_count),
                static_cast<unsigned long>(st.msgs_to_tx));
}

// THE ONE THING loop() DOES BESIDES THE CONSOLE, AND IT IS NOT PROTOCOL — it is keeping the
// controller on the wire, which is a precondition for answering anything.
//
// MEASURED THE FIRST TIME THIS RAN ON THE BENCH, with the display asleep: a single `kick` went
// out, nothing on the bus acknowledged it, the controller retransmitted it into an empty bus
// until it hit the error limit, and TWAI went bus-off and STOPPED — `busErr 15`, state 0, on
// ONE frame. A stopped controller does not receive either, so the whole reactive design was
// then permanently deaf and no amount of the panel waking up would have been noticed.
//
// Bus-off is a two-stage return: ask for recovery, then start the driver once the hardware has
// finished the 128 x 11 recessive bits it insists on. Do NOT reinstall the driver or re-run the
// pin setup here — the pads are already right, and touching them is how a working link gets
// re-broken (see setup()).
void recoverLink() {
  static uint32_t nextMs = 0;
  const uint32_t now = millis();
  if (static_cast<int32_t>(now - nextMs) < 0) return;
  nextMs = now + 1000;

  twai_status_info_t st{};
  if (twai_get_status_info(&st) != ESP_OK) return;
  if (st.state == TWAI_STATE_BUS_OFF) {
    Serial.println("      !! bus-off (nobody acknowledged us) - recovering");
    twai_initiate_recovery();
  } else if (st.state == TWAI_STATE_STOPPED) {
    Serial.println("      !! controller stopped - restarting");
    twai_start();
  }
}

void command(const char* line) {
  if (!strcmp(line, "?") || !strcmp(line, "h")) { printHelp(); return; }
  if (!strcmp(line, "s")) { printStatus(); return; }
  if (!strcmp(line, "t")) { sendClock(); return; }
  if (!strcmp(line, "1")) { sendPower(true); return; }
  if (!strcmp(line, "0")) { sendPower(false); return; }
  if (!strcmp(line, "v")) {
    g_verbose = !g_verbose;
    Serial.printf("rx log %s\n", g_verbose ? "on" : "off");
    return;
  }
  if (!strcmp(line, "ba")) {
    g_announce = !g_announce;
    g_nvs.putBool("ba", g_announce);
    Serial.printf("announce %s (stored)\n", g_announce ? "ON - we will call the panel"
                                                       : "off - we only answer");
    return;
  }
  if (!strcmp(line, "kick")) { g_announced = true; sendAnnounce(); return; }
  if (!strcmp(line, "r")) {
    g_step      = Step::Silent;
    g_peerSeen  = false;
    g_announced = false;
    Serial.println("reset - waiting for the panel");
    return;
  }
  Serial.printf("? %s   (type ? for the list)\n", line);
}

}  // namespace

void setup() {
  delay(3000);

  Serial.begin(115200);
  delay(300);
  Serial.println("\nAffaDisplay 20_carminat_fsm - reactive Carminat opening + clock");

  g_nvs.begin("affafsm", false);
  g_announce = g_nvs.getBool("ba", false);

  // THE PADS COME BACK TO A KNOWN STATE FIRST. They do not survive a reboot as defaults: a
  // software reset leaves the IO MUX and the GPIO matrix holding whatever the previous image
  // configured, and installing the driver on top of that yields a controller that reports
  // RUNNING, counts no errors and receives nothing.
  gpio_reset_pin(kRxPin);
  gpio_reset_pin(kTxPin);

  // ORDER IS LOAD-BEARING: pins, begin(), callback, watchFor() LAST. watchFor() with no
  // argument accepts every id; setting it before the callback loses early frames.
  CAN0.setCANPins(kRxPin, kTxPin);
  CAN0.begin(kBitrate);

  // And nail the input route down by hand — pad -> input buffer -> GPIO matrix -> TWAI_RX_IDX.
  // If anything has repointed that last hop the controller sits at RUNNING hearing nothing
  // while a scope on the pin shows perfect traffic. The pull-up is what keeps a receive line
  // that loses contact reading RECESSIVE instead of floating into a permanent dominant, which
  // on this bench cost 64 000 bus errors a second. Both lines are lifted verbatim from
  // src/link/CanCommonLink.h, where the comments say what each one cost.
  gpio_set_direction(kRxPin, GPIO_MODE_INPUT);
  esp_rom_gpio_connect_in_signal(kRxPin, TWAI_RX_IDX, false);
  gpio_set_pull_mode(kRxPin, GPIO_PULLUP_ONLY);

  CAN0.setGeneralCallback(&onCanFrame);
  CAN0.watchFor();

  Serial.printf("can rx=%d tx=%d @ %lu   announce %s\n", static_cast<int>(kRxPin),
                static_cast<int>(kTxPin), static_cast<unsigned long>(kBitrate),
                g_announce ? "ON" : "off");
  Serial.println("waiting for the panel. nothing goes out until it speaks.");
  printHelp();
}

void loop() {
  // NO PROTOCOL HERE, and that is the whole point of the file: the state machine is entirely
  // in onCanFrame(). This reads the console and keeps the driver on the wire, and neither is
  // protocol.
  static char    line[16];
  static uint8_t n = 0;

  while (Serial.available()) {
    const char c = static_cast<char>(Serial.read());
    if (c == '\r' || c == '\n') {
      line[n] = '\0';
      if (n) command(line);
      n = 0;
    } else if (n < sizeof(line) - 1) {
      line[n++] = c;
    }
  }

  recoverLink();

  // loopTask runs at priority 1 and IDLE at 0, so a loop() that never yields starves IDLE and
  // a single-core part panics with "Task watchdog got triggered (IDLE)".
  delay(10);
}
