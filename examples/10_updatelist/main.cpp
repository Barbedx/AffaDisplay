// 10_updatelist — FIRST CONTACT with an AFFA2 / UpdateList panel, and a full log of it.
//
// THIS IS A DIAGNOSTIC, NOT A DEMO. Everything else in examples/ drives the Carminat
// family, which has four OEM captures and 1 h 36 m on glass behind it. The UpdateList
// family has NEVER MET A BUS: every byte it emits is pinned by golden vectors extracted
// from a reference driver, and step 8 of docs/REFACTOR-PLAN.md moved its SEQUENCING onto
// the measured Carminat rules on the strength of an argument rather than a measurement.
//
// So this example exists to answer one question — does that panel answer us — and to say
// exactly where it stopped if it does not. It prints EVERY frame in both directions, every
// phase transition, and a status line twice a second. No WiFi, no OTA, no console: fewer
// moving parts between the wire and the log.
//
//   pio run -e ex10_updatelist -t upload --upload-port COM5
//   pio device monitor -e ex10_updatelist
//
// WHAT SUCCESS LOOKS LIKE. The panel volunteers `3CF 61 11 xx`; we answer one hello frame
// on `3DF`; the panel opens its own channel with a `70` probe, which we reflex-ACK; we
// register `121` and `1B1`; the library powers the glass; and `SUCCESS` appears.
//
//   RX 3CF  61 11 01 ..        the panel asks
//   TX 3DF  70 1A 11 00 ..     our single hello frame — UpdateList sends ONE, not three
//   RX 0A9  70 ..              the panel registers ITS channel   (or 1C1 on some panels)
//   TX 4A9  74 81 81 ..        our reflex ACK
//   TX 121  70 81 81 ..        our function registration, in table order
//   TX 1B1  70 81 81 ..
//   TX 1B1  04 52 02 FF FF     the library powers the panel on
//   TX 121  10 19 76 ..        "SUCCESS"
//
// AND WHAT THE FAILURES LOOK LIKE, because they are the point of the log:
//
//   nothing on RX at all           the panel does not speak this bus, or the wiring is out.
//                                  Check that 09_golden still works before blaming this.
//   RX 3CF but phase stays Silent  we are not answering. The request was short (no byte 2)
//                                  or arrived before begin().
//   phase stuck at HelloPending    our hello was not accepted by the controller — look for
//                                  txErr climbing, not for a protocol bug.
//   phase stuck at AwaitPeerChannel  THE LIKELIEST ONE. The panel never sent a `70` probe
//                                  of its own, so we correctly refuse to register. That
//                                  gate is measured on CARMINAT and assumed here; if this
//                                  is where it stops, that assumption is the bug and
//                                  registerAfterHello is the knob.
//   phase reaches Ready, glass dark  the bytes are wrong for this panel, not the sequence.
//                                  Compare the TX lines against docs/WIRE-SPEC.md §9.
//
// If it stalls in the handshake, docs/REFACTOR-PLAN.md names the first two knobs to turn:
// `replyToPing` (its reference pongs every `0x69`, and that pong was its only heartbeat
// until March 2026) and reverting registration to lazy.

#include <Arduino.h>
#include <AffaDisplay.h>

#if !AFFA_PANEL_UPDATELIST
#  error "10_updatelist is an UpdateList example: build with -D AFFA_PANEL_UPDATELIST=1"
#endif

namespace {

// The bench rig: ESP32 DevKit V1, CRX -> GPIO5, CTX -> GPIO4, 500 kbit/s. Identical to
// 09_golden, so a run of this that hears nothing at all is a wiring or panel answer, never
// a pinout difference between the two examples.
constexpr gpio_num_t kRxPin   = GPIO_NUM_5;
constexpr gpio_num_t kTxPin   = GPIO_NUM_4;
constexpr uint32_t   kBitrate = 500000;

constexpr const char* kText = "SUCCESS";

struct ArduinoClock final : affa::IClock {
  uint32_t millis() const override { return ::millis(); }
};

affa::CanCommonLink      g_link;
ArduinoClock             g_clock;
// The 8-cell text encoding (`76`), not the LCD's text-plus-icons (`7F`). It is the simpler
// of the two and the one with the shorter payload, which is what you want for first
// contact: fewer frames, fewer ways to be wrong. If the panel registers and powers but
// draws nothing, swapping this for UpdateListMenuDisplay is the next thing to try.
affa::UpdateListDisplay  g_display(g_link, g_clock);

// ---------------------------------------------------------------------------
// The log
// ---------------------------------------------------------------------------
// EVERY FRAME, BOTH DIRECTIONS, UNFILTERED. The heartbeat is included on purpose: this is a
// first-contact log, and "the 1 Hz alive is running" is evidence, not noise. It is a few
// lines a second at most, because nothing here scrolls a marquee.
void onWire(const affa::Frame& f, affa::Direction d, void*) {
  char line[96];
  int n = snprintf(line, sizeof(line), "[%8lu] %s %03X ",
                   static_cast<unsigned long>(millis()),
                   d == affa::Direction::Tx ? "TX" : "RX",
                   static_cast<unsigned>(f.id));
  for (uint8_t i = 0; i < f.len && i < 8; ++i)
    n += snprintf(line + n, sizeof(line) - n, " %02X", static_cast<unsigned>(f.data[i]));
  Serial.println(line);
}

void onSync(affa::SyncState s, void*) {
  Serial.printf("[%8lu] ** sync 0x%02X %s%s%s\n", static_cast<unsigned long>(millis()),
                static_cast<unsigned>(s),
                affa::hasFlag(s, affa::SyncState::Failed) ? "FAILED " : "",
                affa::hasFlag(s, affa::SyncState::PeerAlive) ? "PEER_ALIVE " : "",
                affa::hasFlag(s, affa::SyncState::FuncsReg) ? "REGISTERED" : "");
}

void onDone(affa::TxTicket t, affa::Result r, void*) {
  Serial.printf("[%8lu] ** ticket %u -> %s (%u)\n", static_cast<unsigned long>(millis()),
                static_cast<unsigned>(t),
                r == affa::Result::Ok ? "OK" : "FAILED", static_cast<unsigned>(r));
}

affa::TxTicket g_textTicket = affa::kNoTicket;
bool           g_textSent   = false;
uint32_t       g_firstRxMs  = 0;

}  // namespace

void setup() {
  // Hold the bus recessive while the transceiver settles, exactly as every other example
  // opens. A dominant TX pin during boot is a jammed bus and looks like a dead panel.
  pinMode(kTxPin, OUTPUT);
  digitalWrite(kTxPin, HIGH);
  delay(2000);

  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=== 10_updatelist — AFFA2 first contact ===");
  Serial.printf("pins rx=GPIO%d tx=GPIO%d @ %lu bit/s\n", static_cast<int>(kRxPin),
                static_cast<int>(kTxPin), static_cast<unsigned long>(kBitrate));
  Serial.printf("we transmit on 0x%03X, the panel answers on 0x%03X\n",
                static_cast<unsigned>(affa::updatelist::kIdSync),
                static_cast<unsigned>(affa::updatelist::kIdSyncReply));
  Serial.println("waiting for the panel to speak. It leads; we answer.");
  Serial.println();

  if (!g_link.begin(kRxPin, kTxPin, kBitrate))
    Serial.println("!! the CAN link did not come up — nothing below will mean anything");

  // Taps before begin(): a callback installed afterwards misses whatever already happened,
  // and on this family the opening can complete inside the first poll.
  g_display.onFrame(&onWire, nullptr);
  g_display.onSync(&onSync, nullptr);
  g_display.onComplete(&onDone, nullptr);
  g_display.begin();
}

void loop() {
  g_display.poll();

  const uint32_t now = millis();

  // PHASE IS THE HEADLINE. Printed on change, because a phase that will not advance names
  // the frame that never arrived — see the failure table at the top of this file.
  static affa::Phase s_phase = affa::Phase::Silent;
  if (g_display.phase() != s_phase) {
    Serial.printf("[%8lu] ** PHASE %s -> %s\n", static_cast<unsigned long>(now),
                  affa::phaseName(s_phase), affa::phaseName(g_display.phase()));
    s_phase = g_display.phase();
  }

  if (!g_firstRxMs && g_display.stats().rxFrames) {
    g_firstRxMs = now;
    Serial.printf("[%8lu] ** first frame heard from the bus\n",
                  static_cast<unsigned long>(now));
  }

  // ONE RENDER, ONCE, AND ONLY FROM Ready. Ready means registered AND the glass is on — the
  // library sends this family's `1B1 04 52 02 FF FF` itself on the way there. Drawing before
  // that is the failure with no symptom: the panel ACKs a screen it never lights.
  if (!g_textSent && g_display.phase() == affa::Phase::Ready) {
    const affa::Result r = g_display.setText(kText);
    g_textTicket = g_display.lastEnqueued();
    Serial.printf("[%8lu] ** setText(\"%s\") accepted=%s ticket=%u\n",
                  static_cast<unsigned long>(now), kText,
                  r == affa::Result::Ok ? "yes" : "NO", static_cast<unsigned>(g_textTicket));
    if (r != affa::Result::Ok)
      Serial.printf("[%8lu] !! rejected with %u — that is an ACCEPTANCE verdict, not a "
                    "delivery one\n", static_cast<unsigned long>(now),
                    static_cast<unsigned>(r));
    g_textSent = true;
  }

  // TWICE A SECOND, AND IT INCLUDES QUEUE DEPTHS. `rx 0` with zero errors fits three
  // different states — a silent bus, a bus we cannot decode, and a controller that never
  // started — and telling them apart needs the queue counters. A dead receive path once
  // looked exactly like a sleeping panel for hours because nothing printed them.
  static uint32_t s_nextStatus = 0;
  if (static_cast<int32_t>(now - s_nextStatus) >= 0) {
    s_nextStatus = now + 500;
    const affa::Stats st = g_display.stats();
    const affa::CanCommonLink::Driver d = g_link.driver();
    Serial.printf("[%8lu] .. phase %-16s sync 0x%02X reg %d | rx %lu tx %lu txDrop %lu "
                  "ovf %lu | drv %u txErr %lu rxErr %lu busErr %lu qRx %lu qTx %lu\n",
                  static_cast<unsigned long>(now), affa::phaseName(g_display.phase()),
                  static_cast<unsigned>(g_display.syncState()),
                  g_display.registered() ? 1 : 0,
                  static_cast<unsigned long>(st.rxFrames),
                  static_cast<unsigned long>(st.txFrames),
                  static_cast<unsigned long>(st.txDropped),
                  static_cast<unsigned long>(st.ringOverflow),
                  static_cast<unsigned>(d.valid ? d.state : 255),
                  static_cast<unsigned long>(d.txErr),
                  static_cast<unsigned long>(d.rxErr),
                  static_cast<unsigned long>(d.busErr),
                  static_cast<unsigned long>(d.queuedRx),
                  static_cast<unsigned long>(d.queuedTx));
  }

  // A VERDICT, ONCE, SO THE LOG DOES NOT HAVE TO BE READ BACKWARDS. Ten seconds is far
  // longer than the whole opening takes when it works — 6 s of that is the boot delay.
  static bool s_verdict = false;
  if (!s_verdict && now > 15000) {
    s_verdict = true;
    Serial.println();
    Serial.println("=== 15-second verdict ===");
    if (!g_display.stats().rxFrames) {
      Serial.println("NOTHING HEARD. The panel has not put a single frame on the bus.");
      Serial.println("  This is not an UpdateList question yet — flash ex09_golden and");
      Serial.println("  confirm the wiring and the panel still work at all.");
    } else if (g_display.phase() == affa::Phase::Ready) {
      Serial.println("READY. Registered, powered, and the text was submitted.");
      Serial.println("  If the glass is still blank the SEQUENCE is right and the BYTES");
      Serial.println("  are wrong — compare the TX lines with docs/WIRE-SPEC.md §9.1.");
    } else {
      Serial.printf("STALLED AT %s.\n", affa::phaseName(g_display.phase()));
      Serial.println("  The frame that would advance it is named in the table at the top");
      Serial.println("  of examples/10_updatelist/main.cpp. AwaitPeerChannel is the one to");
      Serial.println("  expect: it means the panel never registered a channel of its own,");
      Serial.println("  which is a rule measured on Carminat and only ASSUMED here.");
    }
    Serial.println("=========================");
    Serial.println();
  }
}
