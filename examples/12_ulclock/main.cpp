// 12_ulclock — WHICH FRAME SETS THE CLOCK ON AN AFFA2 PANEL? A numbered probe.
//
// The UpdateList reference driver has no clock command, and `UpdateListConstants.h` is right
// not to invent one. But the bench panel is UNIVERSAL — it answers as Carminat too — and it
// displays a clock that free-runs from its own power-on with nothing able to set it. There
// are several candidate frames and no capture that settles it, so this asks the panel.
//
// HOW TO READ THE RESULT. The panel's TEXT line shows which candidate is being fired:
//
//     TRY 1  12:34
//
// and the candidate goes out immediately after that line is acknowledged. Watch the panel's
// CLOCK area, not the text line. Whichever number is on screen when the clock jumps to 12:34
// is the frame that works. Nothing else in this example writes the clock, and the target
// 12:34 is deliberately nothing like a plausible free-running value.
//
// IT CYCLES FOREVER, so a missed transition comes round again in half a minute.
//
//   pio run -e ex12_ulclock -t upload --upload-port COM5
//   pio device monitor -e ex12_ulclock
//
// EVERY CANDIDATE IS A GUESS AND IS LABELLED AS ONE. Two are structural analogies, one is
// transcribed from a capture of a different panel, and one is a frame this exact panel has
// already accepted — but on the other family, on a channel this family never registers.
// If none of them works that is a real result too: it means the clock is not reachable from
// this family's registered channels, and the next place to look is the panel's own key
// channel or a broadcast id nobody has decoded yet.

#include <Arduino.h>
#include <AffaDisplay.h>

#if !AFFA_PANEL_UPDATELIST
#  error "12_ulclock probes an UpdateList panel: build with -D AFFA_PANEL_UPDATELIST=1"
#endif

namespace {

constexpr gpio_num_t kRxPin   = GPIO_NUM_5;
constexpr gpio_num_t kTxPin   = GPIO_NUM_4;
constexpr uint32_t   kBitrate = 500000;

// THE TARGET. Nothing like a plausible free-running value, so "did it work" needs no
// judgement call. ASCII for the text-style candidates, and the same time as plain and BCD
// bytes for the raw ones.
constexpr uint8_t kHour = 12, kMin = 34;
constexpr const char* kHHMM = "1234";
constexpr uint8_t kHourBcd = 0x12, kMinBcd = 0x34;

constexpr uint32_t kHoldMs = 6000;   // long enough to read the clock and look away and back

struct ArduinoClock final : affa::IClock {
  uint32_t millis() const override { return ::millis(); }
};

affa::CanCommonLink     g_link;
ArduinoClock            g_clock;
affa::UpdateListDisplay g_display(g_link, g_clock);

bool     g_busy   = false;
uint8_t  g_try    = 0;          // 0 = show the marker, 1 = fire, then advance
size_t   g_cand   = 0;
uint32_t g_nextAt = 0;
uint32_t g_passes = 0;

// ---------------------------------------------------------------------------
// The candidates
// ---------------------------------------------------------------------------
// `raw` frames bypass the library's transmit FSM entirely — no registration, no ISO-TP, no
// ACK matching — because two of these are not AFFA messages at all and would be corrupted by
// being framed. The library's own queue is idle when they go out; see the gate in loop().
struct Candidate {
  const char* name;      // for the serial log
  bool        raw;       // true = ICanLink::send(), false = enqueue() through the transport
  uint16_t    id;
  uint8_t     len;
  uint8_t     data[8];
  const char* rationale;
};

// A SWEEP ENTRY, generated rather than written out. `05 <cmd> "1234"` on a registered
// channel, for every command byte worth trying. Carminat's clock is command `0x56` on its
// text function and this family's text function is `0x121`, so if the analogy holds at all
// the answer is a single byte away from something we have already sent and had ACKED.
#define SWEEP(id, cmd) \
  {"sweep " #id " 05 " #cmd " HHMM", false, id, 6, {0x05, cmd, '1', '2', '3', '4'}, \
   "command-byte sweep on a registered channel"}

const Candidate kCandidates[] = {
    // [GUESS] The one designed for exactly this situation. `3EF A6 <hh> <mm>`, DLC 3, no PCI
    // and no SF_DL — it does not go through the transport at all. Transcribed from an OEM
    // radio↔cluster capture (docs/PROTOCOL-NOTES.md §9, ClusterConstants.h) and never put on
    // a bus by this library. If this works it is the RIGHT answer, not merely a working one:
    // it needs no function registration and belongs to no family.
    {"3EF A6 hh mm (plain)", true, 0x3EF, 3, {0xA6, kHour, kMin},
     "the cluster capture's clock, decimal bytes"},

    // [GUESS] The same frame with BCD, because a dashboard clock encoding its digits is at
    // least as likely as binary and the capture cannot distinguish 0x12 from 12 for hours
    // under 10.
    {"3EF A6 hh mm (BCD)", true, 0x3EF, 3, {0xA6, kHourBcd, kMinBcd},
     "same frame, BCD digits"},

    // [GUESS] THE CLOSEST STRUCTURAL ANALOGY. Carminat sets its clock with `05 56 "HHMM"` on
    // its TEXT function (0x151); this is the identical command on THIS family's text
    // function (0x121), which is registered and acknowledged. If the two families really are
    // the same protocol with different ids — which is what this whole day suggested — this
    // is the frame it should be.
    {"121 05 56 HHMM", false, 0x121, 6, {0x05, 0x56, '1', '2', '3', '4'},
     "Carminat's clock command on this family's text id"},

    // [GUESS] The same command on the display-control function, because a clock is arguably
    // control rather than content, and 0x1B1 is the other id we register.
    {"1B1 05 56 HHMM", false, 0x1B1, 6, {0x05, 0x56, '1', '2', '3', '4'},
     "…and on the control id instead"},

    // [CAP-adjacent] The frame THIS PHYSICAL PANEL has already accepted — read off the glass
    // as 10:00 on 2026-07-28 — but sent on Carminat's id, which this family never registers.
    // Raw, because enqueue() would refuse it as UnknownFunc. Whether an unregistered channel
    // is even listened to is the actual question here.
    {"151 05 56 HHMM (raw)", true, 0x151, 8,
     {0x05, 0x56, '1', '2', '3', '4', 0x81, 0x81},
     "Carminat's proven clock frame on an unregistered channel"},

    // [GUESS] THE OPENING IS FULL OF BYTES NOBODY HAS EXPLAINED, which is the owner's
    // suggestion and a good one. This family's hello is `70 1A 11 00 00 00 00 01` and
    // Carminat's is `B0 14 11 00 1F 00 00 00`: the leading three bytes differ per family,
    // and then there are FOUR bytes that are zero in one and near-zero in the other, sitting
    // in the one frame the panel is definitely listening to. If the radio tells the panel
    // the time at all, telling it during the opening would be a reasonable design.
    //
    // Sent raw, after the session is already up, so it is a probe rather than a handshake
    // change — the real hello has long since gone out and been answered.
    {"3DF hello + BCD time", true, 0x3DF, 8,
     {0x70, 0x1A, 0x11, 0x00, kHourBcd, kMinBcd, 0x00, 0x01},
     "the hello's unexplained zero bytes, filled with BCD hh mm"},

    {"3DF hello + plain time", true, 0x3DF, 8,
     {0x70, 0x1A, 0x11, 0x00, kHour, kMin, 0x00, 0x01},
     "same, decimal"},

    // THE SWEEP. Everything above is an analogy; this is a search. Command bytes adjacent to
    // the ones this family already uses — 0x52 is display control, 0x76 and 0x7F are the two
    // text encodings — plus Carminat's 0x56 neighbourhood.
    SWEEP(0x121, 0x50), SWEEP(0x121, 0x51), SWEEP(0x121, 0x53), SWEEP(0x121, 0x54),
    SWEEP(0x121, 0x55), SWEEP(0x121, 0x57), SWEEP(0x121, 0x58), SWEEP(0x121, 0x59),
    SWEEP(0x121, 0x5A), SWEEP(0x121, 0x60), SWEEP(0x121, 0x63), SWEEP(0x121, 0x64),
    SWEEP(0x1B1, 0x56), SWEEP(0x1B1, 0x57), SWEEP(0x1B1, 0x63), SWEEP(0x1B1, 0x64),
};
constexpr size_t kCandCount = sizeof(kCandidates) / sizeof(kCandidates[0]);

void onDone(affa::TxTicket, affa::Result r, void*) {
  g_busy = false;
  if (r != affa::Result::Ok)
    Serial.printf("[%8lu] !! render failed (%u)\n", static_cast<unsigned long>(millis()),
                  static_cast<unsigned>(r));
}

// EVERY FRAME, BOTH DIRECTIONS — because the interesting answer to a probe is not whether it
// was ACKED but whether the panel said anything ELSE. A candidate that provokes an unusual
// reply, or a `61 11` that tears the session down, is a stronger signal than the clock not
// moving. The 1 Hz heartbeat pair is filtered so it does not bury them.
void onWire(const affa::Frame& f, affa::Direction d, void*) {
  if (f.len >= 1 && ((f.id == 0x3DF && f.data[0] == 0x79) ||
                     (f.id == 0x3CF && f.data[0] == 0x69)))
    return;
  char line[96];
  int n = snprintf(line, sizeof(line), "[%8lu]    %s %03X ",
                   static_cast<unsigned long>(millis()),
                   d == affa::Direction::Tx ? "tx" : "rx",
                   static_cast<unsigned>(f.id));
  for (uint8_t i = 0; i < f.len && i < 8; ++i)
    n += snprintf(line + n, sizeof(line) - n, " %02X", static_cast<unsigned>(f.data[i]));
  Serial.println(line);
}

void fire(const Candidate& c) {
  char bytes[40] = {0};
  int n = 0;
  for (uint8_t i = 0; i < c.len; ++i)
    n += snprintf(bytes + n, sizeof(bytes) - n, " %02X", static_cast<unsigned>(c.data[i]));

  if (c.raw) {
    affa::Frame f;
    f.id  = c.id;
    f.len = c.len;
    memcpy(f.data, c.data, c.len);
    const bool ok = g_link.send(f);
    Serial.printf("[%8lu] >> RAW  %03X%s   %s%s\n", static_cast<unsigned long>(millis()),
                  static_cast<unsigned>(c.id), bytes, c.name, ok ? "" : "  [SEND REFUSED]");
  } else {
    // enqueue() returns a TICKET, not a Result — kNoTicket is the only signal of rejection
    // and the reason lands in lastResult(). Dropping the ticket would turn a refusal into a
    // frame that silently never went out, which in a probe like this reads as "the panel
    // ignored it" and would be the wrong conclusion entirely.
    const affa::TxTicket t = g_display.enqueue(c.id, c.data, c.len);
    Serial.printf("[%8lu] >> TX   %03X%s   %s   accepted=%s%s\n",
                  static_cast<unsigned long>(millis()), static_cast<unsigned>(c.id), bytes,
                  c.name, t != affa::kNoTicket ? "yes" : "NO — ",
                  t != affa::kNoTicket ? "" :
                      (g_display.lastResult() == affa::Result::UnknownFunc
                           ? "UnknownFunc, this id is not in the table"
                           : "rejected"));
    if (t != affa::kNoTicket) g_busy = true;
  }
}

}  // namespace

void setup() {
  pinMode(kTxPin, OUTPUT);
  digitalWrite(kTxPin, HIGH);
  delay(2000);

  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=== 12_ulclock — which frame sets an AFFA2 clock? ===");
  Serial.printf("target %02u:%02u — watch the panel's CLOCK, not its text line\n",
                static_cast<unsigned>(kHour), static_cast<unsigned>(kMin));
  for (size_t i = 0; i < kCandCount; ++i)
    Serial.printf("  TRY %u  %s — %s\n", static_cast<unsigned>(i + 1),
                  kCandidates[i].name, kCandidates[i].rationale);
  Serial.println();

  if (!g_link.begin(kRxPin, kTxPin, kBitrate))
    Serial.println("!! the CAN link did not come up");

  g_display.onFrame(&onWire, nullptr);
  g_display.onComplete(&onDone, nullptr);
  g_display.begin();
}

void loop() {
  g_display.poll();
  const uint32_t now = millis();

  static affa::Phase s_phase = affa::Phase::Silent;
  if (g_display.phase() != s_phase) {
    Serial.printf("[%8lu] ** PHASE %s -> %s\n", static_cast<unsigned long>(now),
                  affa::phaseName(s_phase), affa::phaseName(g_display.phase()));
    s_phase = g_display.phase();
  }

  // Nothing is probed before Ready: an unregistered, unlit panel would refuse or ignore
  // everything below and the run would prove nothing at all.
  if (g_display.phase() != affa::Phase::Ready || g_busy) return;
  if (static_cast<int32_t>(now - g_nextAt) < 0) return;

  if (g_try == 0) {
    // THE MARKER GOES UP FIRST and is acknowledged before the candidate is fired, so the
    // number on the glass is always the candidate that has just gone out. Getting this
    // backwards would make every confirmation off by one, which is the only way this
    // experiment can produce a confidently wrong answer.
    char label[16];
    snprintf(label, sizeof(label), "TRY %u  %02u:%02u", static_cast<unsigned>(g_cand + 1),
             static_cast<unsigned>(kHour), static_cast<unsigned>(kMin));
    if (g_display.setText(label) == affa::Result::Ok) {
      g_busy  = true;
      g_try   = 1;
      g_nextAt = now + 400;         // let the glass settle before the candidate lands
    } else {
      g_nextAt = now + 250;
    }
    return;
  }

  fire(kCandidates[g_cand]);
  g_try   = 0;
  g_nextAt = now + kHoldMs;
  if (++g_cand >= kCandCount) {
    g_cand = 0;
    Serial.printf("[%8lu] == pass %lu complete — none of the %u candidates has been "
                  "confirmed yet\n", static_cast<unsigned long>(now),
                  static_cast<unsigned long>(++g_passes),
                  static_cast<unsigned>(kCandCount));
  }
}
