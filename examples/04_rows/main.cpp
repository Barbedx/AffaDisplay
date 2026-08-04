// 04_rows — a live three-row screen, a periodic popup, and a console to poke them with.
//
// This is the example to read AFTER 03_hello. 03_hello puts one string on the glass and
// stops; this one keeps a screen ALIVE for hours, which is a different problem and has
// exactly three parts:
//
//   1. WHAT THE ROWS SAY is a widget, not a protocol call. affa::widget::RowScreen holds
//      three marquees, tells you when the visible window has actually changed, and knows
//      nothing about CAN. It transmits nothing. THIS FILE does the transmitting.
//   2. WHEN TO REPAINT is a function of the wire, not of the loop. A Carminat fullscreen is
//      fourteen ACKed frames — about 190 ms — so the wire carries roughly five screens a
//      second. Painting on a timer would queue faster than that and the screen would lag
//      further behind reality every minute. Painting when the PREVIOUS ONE COMPLETED and
//      the content has moved is self-limiting and cannot fall behind.
//   3. THE POPUP IS AN OVERLAY, and it is the one screen here that must be closed. It
//      survives a redraw of the screen underneath; a fullscreen does not need a teardown
//      at all, because the next fullscreen simply replaces it.
//
// THE ONE-LINE VERSION OF THE API, and the thing people get wrong:
//
//     every render call ENQUEUES and returns immediately. The value it hands back is an
//     ACCEPTANCE verdict, never a delivery verdict. Delivery arrives later, on onComplete.
//
// So `showFullscreenText(a, b, c)` twice in a row does not draw two screens: they share one
// render slot and the second replaces the first. That is a FEATURE for a live screen — it
// is what "coalescing" means, and it is why this file can be sloppy about timing — but it
// makes "call it in a loop and watch it animate" produce a frozen screen and no error.
//
// WHY IT HAS WIFI when 03_hello deliberately does not: this runs for an hour on a board
// with no cable attached. The console is how you change a speed without a rebuild, and
// ElegantOTA is the only way back in. Both are at the BOTTOM of this file, fenced off —
// delete everything below THE DEPLOYMENT SCAFFOLDING line and the example still works, it
// just becomes a cable-flashed appliance with fixed speeds.
//
// Build:
//   pio run -e ex04_rows -t upload     first flash, over USB
//   thereafter                         http://<ip>/       console
//                                      http://<ip>/update ElegantOTA

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <PsychicHttp.h>
#include <ElegantOTA.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include <AffaDisplay.h>

// The ECC probe reads the TWAI peripheral's own registers. The register map and status API
// are ESP-IDF's; nothing here reaches into Esp32CanLink's private state.
#include <driver/twai.h>
#include <soc/twai_struct.h>
#include <soc/gpio_struct.h>
#include <soc/gpio_sig_map.h>
#include <soc/io_mux_reg.h>
#include <soc/system_reg.h>

#if !AFFA_PANEL_CARMINAT
#  error "04_rows is a Carminat example: build with -D AFFA_PANEL_CARMINAT=1"
#endif
#if !AFFA_ENABLE_TASK
#  error "04_rows relies on the library owning the poll task: -D AFFA_ENABLE_TASK=1"
#endif
#if !AFFA_ENABLE_MARQUEE
#  error "04_rows is built on the RowScreen widget: -D AFFA_ENABLE_MARQUEE=1"
#endif
#if !AFFA_ENABLE_FULLSCREEN || !AFFA_ENABLE_POPUP
#  error "04_rows draws a fullscreen and a popup: -D AFFA_ENABLE_FULLSCREEN=1 -D AFFA_ENABLE_POPUP=1"
#endif

namespace {

using affa::widget::Marquee;
using affa::widget::RowScreen;

// ---------------------------------------------------------------------------
// Board
// ---------------------------------------------------------------------------
// ESP32-C3 SuperMini on the bench rig. RX FIRST — the named fields are what stop the swap
// from becoming a silent bus with no error reported anywhere.
constexpr affa::CanPins kPins{ .rx = GPIO_NUM_3, .tx = GPIO_NUM_4 };
constexpr uint32_t      kBitrate = 500000;

// The panel does not announce that its glass is lit, so this is a hard-coded wait and there
// is no honest way around it.
constexpr uint32_t kWarmUpMs = 750;

// What the panel's own clock is set to once the glass is warm, four ASCII digits "HHMM".
// A fixed value on purpose: this board has no RTC and no NTP by the time the sequence
// runs, and the goal state is verifiable — the glass must read SUCCESS and 10:00.
constexpr const char* kClockHHMM = "1000";

// ---------------------------------------------------------------------------
// Screen geometry and cadence — the one set of numbers worth understanding
// ---------------------------------------------------------------------------
// Visible cells per row on a Carminat fullscreen. The wire carries far more (96 content
// bytes for the three lines together); this is how much of it the glass actually shows, so
// it is the width the marquee window must be.
constexpr uint8_t kRowWidth = 20;

// Blank cells appended before the text wraps, so the wrap reads as a pause and not as a
// collision between the end and the start.
constexpr uint8_t kRowGap = 6;

// THREE SPEEDS ON ONE GRID, and the harmonising is the point rather than the values.
//
// A repaint costs ~190 ms of wire, so about five screens a second is all there is. These
// three cadences ask for 1/0.4 + 1/0.8 + 1/1.2 = 4.2 steps a second in the WORST case — but
// because 800 and 1200 are multiples of 400, the step instants COINCIDE, and the actual
// repaint rate is 2.5/s. Pick 250/400/700 instead and the same visible motion costs eight
// repaints a second: 60 % over what the wire can carry, so the rows step at whatever rate
// the bus allows rather than the rate you configured, and the scroll goes uneven.
//
// Oversubscribing is SAFE — nothing breaks, the library coalesces what it cannot send — it
// just stops looking like what you asked for. The console lets you try it.
constexpr uint32_t kSpeedFast   = 400;    // row 0
constexpr uint32_t kSpeedMedium = 800;    // row 1
constexpr uint32_t kSpeedSlow   = 1200;   // row 2

// The popup: every 30 s, visible for 3 s, carrying a counter so you can tell one from the
// next from across the room.
constexpr uint32_t kPopupEveryMs = 30000;
constexpr uint32_t kPopupHoldMs  = 3000;

// A render that never completes must not wedge the sequence for ever. Two full ISO-TP ACK
// timeouts is comfortably longer than any healthy screen takes.
constexpr uint32_t kRenderTimeoutMs = 4000;

// How often the soak line goes into the log ring. Once a minute is enough to reconstruct an
// hour afterwards and cheap enough to leave running for a day.
constexpr uint32_t kSoakReportMs = 60000;

// ---------------------------------------------------------------------------
// The library objects. Four, and that is the whole surface.
// ---------------------------------------------------------------------------
struct ArduinoClock final : affa::IClock {
  uint32_t millis() const override { return ::millis(); }
};

affa::Esp32CanLink    g_link;
ArduinoClock          g_clock;
affa::CarminatDisplay g_display(g_link, g_clock);
affa::rtos::AffaTask  g_task;      // the library polls itself; loop() may block freely

// The widget. Holds content and phase; holds no link, no display and no task.
affa::widget::RowScreenGeometry rowGeometry() {
  affa::widget::RowScreenGeometry g;
  g.width = kRowWidth;
  g.gap   = kRowGap;
  return g;
}
RowScreen g_rows{rowGeometry()};

// ---------------------------------------------------------------------------
// The log ring — this board is watched over HTTP, not over a cable
// ---------------------------------------------------------------------------
// Written from loop() and from the library's owned task (the sync callback), read from the
// HTTP task. A spinlock rather than "it is only diagnostics": a torn record here is exactly
// the kind of thing that sends you hunting a protocol bug that does not exist.
struct LogRec { uint32_t ms = 0; char msg[96] = {0}; };
constexpr uint8_t kLogRing = 96;
LogRec            g_log[kLogRing];
uint8_t           g_logHead = 0;
portMUX_TYPE      g_logMux  = portMUX_INITIALIZER_UNLOCKED;

void logmsg(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void logmsg(const char* fmt, ...) {
  char buf[96];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  const uint32_t ms = ::millis();
  portENTER_CRITICAL(&g_logMux);
  LogRec& r = g_log[g_logHead];
  r.ms = ms;
  snprintf(r.msg, sizeof(r.msg), "%s", buf);
  g_logHead = (g_logHead + 1) % kLogRing;
  portEXIT_CRITICAL(&g_logMux);

  if (Serial) Serial.printf("[%lu] %s\n", static_cast<unsigned long>(ms), buf);
}

// ---------------------------------------------------------------------------
// The frame ring — WHAT THE PANEL ACTUALLY SENDS US
// ---------------------------------------------------------------------------
// AffaDisplayBase::onFrame() is the Layer-0 tap: every frame, both directions, before any
// interpretation. It is the only view that can answer "is the other end talking at all",
// and without it a screen that does not appear is indistinguishable from a panel that is
// switched off.
//
// Pushed from the library's owned task, read from the HTTP task, so it takes a spinlock. Not
// because it is precious, but because a torn record here sends you hunting a protocol bug
// that does not exist.
// A RAW RING IS THE WRONG SHAPE HERE and it is why nothing useful has been captured yet.
// The panel transmits ONE IDENTICAL FRAME back to back at line rate — measured ~3000 in two
// seconds — so sixty-four raw slots hold about twenty milliseconds of history and every one
// of them says the same thing. The two frames a second that WE send, which are the only
// candidate cause, are flushed out before anyone can read them.
//
// So identical consecutive frames COALESCE into one row with a repeat count and a last-seen
// stamp. The panel's flood becomes a single line, our transmissions stay visible next to it,
// and the ring holds minutes instead of milliseconds. A trace then reads:
//
//     RX 3CF 61 11 01 A3.. x14832   (t=2140..7003)
//     TX 3AF B9 00 ..               (t=7010)
//     RX 3CF 61 11 01 A3.. x4       (t=7011..7012)
//     <nothing more>
//
// which names the last frame we sent before the bus went quiet. That is the whole question.
struct FrameRec {
  uint32_t firstMs = 0;
  uint32_t lastMs  = 0;
  uint32_t count   = 0;
  uint16_t id      = 0;
  uint8_t  dir     = 0;      // 0 = RX (heard), 1 = TX (ours)
  uint8_t  len     = 0;
  uint8_t  d[8]    = {0};
};
constexpr uint8_t kFrameRing = 160;   // a rolling wire log, not a 48-row death snapshot
FrameRec     g_frames[kFrameRing];
uint8_t      g_frameHead = 0;      // slot AFTER the newest, i.e. the next one to overwrite
uint32_t     g_frameSeq  = 0;      // total frames observed, uncoalesced
portMUX_TYPE g_frameMux  = portMUX_INITIALIZER_UNLOCKED;

// Counted separately from the driver's counters: these are frames that reached the LIBRARY,
// which is what "do we hear the panel" actually asks.
uint32_t g_rxSeen   = 0;
uint32_t g_txSeen   = 0;
uint32_t g_lastRxMs = 0;

// THE TRACE FREEZES ITSELF WHEN RECEPTION STOPS, and that is the entire point of it. Every
// measurement taken so far has been of the steady state AFTER the link died, by which time
// the evidence has been overwritten by a thousand of our own retries. Frozen, the ring holds
// the last frames before the panel went quiet — including whatever we transmitted into it.
bool     g_traceFrozen = false;
// See the DEAF handler: the trace is a rolling wire log by default, not a one-shot snapshot.
constexpr bool kFreezeTraceOnDeaf = false;
uint32_t g_frozenAtMs  = 0;

void onTap(const affa::Frame& f, affa::Direction d, void*) {
  const bool rx = (d == affa::Direction::Rx);
  const uint32_t ms = ::millis();
  portENTER_CRITICAL(&g_frameMux);
  ++g_frameSeq;
  if (rx) { ++g_rxSeen; g_lastRxMs = ms; } else { ++g_txSeen; }

  if (!g_traceFrozen) {
    // Coalesce against the NEWEST row only. Not a search: the question is "what changed",
    // and a run that is interrupted and resumes is exactly the thing that must show as two
    // rows rather than one.
    FrameRec& last = g_frames[(g_frameHead + kFrameRing - 1) % kFrameRing];
    const bool same = last.count && last.dir == (rx ? 0 : 1) &&
                      last.id == static_cast<uint16_t>(f.id) && last.len == f.len &&
                      memcmp(last.d, f.data, 8) == 0;
    if (same) {
      ++last.count;
      last.lastMs = ms;
    } else {
      FrameRec& r = g_frames[g_frameHead];
      r.firstMs = ms;
      r.lastMs  = ms;
      r.count   = 1;
      r.id      = static_cast<uint16_t>(f.id);
      r.dir     = rx ? 0 : 1;
      r.len     = f.len;
      memcpy(r.d, f.data, 8);
      g_frameHead = static_cast<uint8_t>((g_frameHead + 1) % kFrameRing);
    }
  }
  portEXIT_CRITICAL(&g_frameMux);
}

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
    // Aborted is the EXPECTED one here and not a fault: a live screen supersedes its own
    // render whenever the content moves again before the last one reached the wire.
    case affa::Result::Aborted:      return "Aborted";
    default:                         return "?";
  }
}

// ---------------------------------------------------------------------------
// Render completion — published by the library's task, consumed by loop()
// ---------------------------------------------------------------------------
// THE SEQUENCE NUMBER IS PUBLISHED LAST, and everything else is written before it. A reader
// that sees the bump therefore sees a consistent record, with no lock on the path the
// library actually cares about.
volatile uint32_t g_ackReq = 0;
volatile uint8_t  g_ackRes = 0;
volatile uint32_t g_ackSeq = 0;
uint32_t          g_ackSeen = 0;    // loop()'s cursor into that stream

uint32_t g_inFlight    = 0;         // the request we are waiting on; 0 when idle
uint32_t g_flightSince = 0;

void onRenderComplete(affa::rtos::TxRequest req, affa::Result r, void*) {
  g_ackReq = static_cast<uint32_t>(req);
  g_ackRes = static_cast<uint8_t>(r);
  ++g_ackSeq;
}

// 0 = still waiting, 1 = ours completed Ok, 2 = ours failed, 3 = nothing in flight.
uint8_t pollRender(affa::Result& out) {
  if (g_inFlight == 0) { g_ackSeen = g_ackSeq; return 3; }
  while (g_ackSeen != g_ackSeq) {
    ++g_ackSeen;
    if (g_ackReq == g_inFlight) {
      out = static_cast<affa::Result>(g_ackRes);
      g_inFlight = 0;
      return out == affa::Result::Ok ? 1 : 2;
    }
    // Somebody else's completion. The library makes renders of its own — a menu redraw, a
    // close banner — and they come through this same callback. Keep looking.
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Application state
// ---------------------------------------------------------------------------
enum class Step : uint8_t { WaitPanel, PowerOn, WarmUp, Live };

// Spelled out rather than numbered, because "step 0" tells you nothing and this is the first
// question anyone asks a screen that is blank.
const char* stepName(Step s) {
  switch (s) {
    case Step::WaitPanel: return "wait-panel  nothing heard from the panel yet";
    case Step::PowerOn:   return "power-on    setPower enqueued, waiting for its ACK";
    case Step::WarmUp:    return "warm-up     ACKed; letting the glass light up";
    case Step::Live:      return "live        drawing rows";
  }
  return "?";
}

Step     g_step     = Step::WaitPanel;
uint32_t g_deadline = 0;

bool     g_powerWanted = true;     // what the console last asked for
bool     g_powerIsOn   = false;    // what the panel was last told

bool     g_popupUp     = false;
uint32_t g_popupNextMs = 0;
uint32_t g_popupUntil  = 0;
uint32_t g_popupCount  = 0;

// The clock is HALF THE GOAL STATE ("SUCCESS and 10:00"), so it is a LATCHED goal with a
// paced re-issue, not a fire-and-forget step. The rows survive any failed render because
// pumpScreen() republishes them every iteration; the clock needs the same property by
// hand, or one Timeout right after warm-up loses it until an unrelated sync loss happens
// to replay the whole sequence.
bool     g_clockOk      = false;   // its render completed Ok; cleared by restart()
uint32_t g_clockReq     = 0;       // the setTime render in flight, 0 when none
uint32_t g_clockRetryAt = 0;       // earliest re-issue after a failure
constexpr uint32_t kClockRetryMs = 5000;

// ---------------------------------------------------------------------------
// LISTEN BEFORE YOU SPEAK
// ---------------------------------------------------------------------------
// THE BOARD COMES UP WITH ITS TRANSMITTER GATED SHUT, and this is the change that matters
// rather than any of the instrumentation around it.
//
// The panel repeats one identical frame back to back at line rate. That is not a node
// spamming; it is a CAN transmitter RETRANSMITTING BECAUSE NOTHING HAS ACKNOWLEDGED IT. A
// node in that state is transmitting essentially all the time, and a node that is
// transmitting cannot acknowledge anybody else — so our own frames go unacknowledged too,
// our controller retries, and both ends sit in a deadlock neither can leave. Every
// measurement of this rig fits that shape: perfect in listen-only, collapse the moment we
// drive, `rxErr` 129 (a bit error on the ACK — the one bit we drive), `txErr` -> 128 ->
// bus-off, and a working image that heard 498 frames and then went deaf for ever.
//
// THE SOFTWARE GATE IS EXACTLY THE RIGHT TOOL and it is why it is not a driver mode: it
// refuses our own send() while THE CONTROLLER STILL ACKS IN HARDWARE. So with the gate shut
// we are the silent listener that acknowledges — which is precisely what a runaway
// retransmitter needs to see in order to stop. Listen-only cannot do this: it acknowledges
// nothing, which is why it never rescues the bus however long it runs.
//
// Only once we have actually heard the panel do we open the gate and let the library talk.
constexpr uint32_t kQuietMinMs    = 3000;   // never speak before this, however loud the bus
constexpr uint8_t  kHeardEnough   = 20;     // frames decoded before we consider the bus sane

// AND IF IT DIES AGAIN, GO QUIET AGAIN. Reception stopping while our gate is open is the one
// event worth reacting to, because it is the transition nobody has ever captured. We freeze
// the trace, shut the gate, and let the panel recover — rather than retrying into it, which
// is what has been happening for three days.
constexpr uint32_t kDeafMs        = 600;    // no RX for this long, having heard before
constexpr uint32_t kBackoffMs     = 5000;   // stay silent this long before trying again
constexpr uint32_t kShoutMs       = 4000;   // talk this long into silence before giving up

// ---------------------------------------------------------------------------
// SHOUT — "can it still hear us?", answered without a scope
// ---------------------------------------------------------------------------
// While we are in NORMAL we decode nothing, so we cannot watch the panel answer. But we can
// watch what it does AFTERWARDS. So: drop to normal, transmit for a couple of seconds, go
// back to listen-only — where reception is flawless — and compare the panel's traffic with
// what it was doing before. A panel that heard us CHANGES: it stops repeating the sync
// request and moves on. A panel that heard nothing carries on identically.
//
// That is the difference between "our transmitter does not reach the wire" and "it reaches
// the wire and only our ACK is malformed", and those need opposite fixes.
uint8_t  g_shoutPhase = 0;      // 0 idle, 1 start, 2 shouting
uint32_t g_shoutMs    = 2500;
uint32_t g_shoutUntil = 0;

// ---------------------------------------------------------------------------
// KICK — answer the panel BLIND, which is the one thing never yet tried
// ---------------------------------------------------------------------------
// The panel asks `61 11 01` at line rate and the answer is a three-frame hello. But the
// library only emits that hello ON RECEIPT of the request — and in NORMAL mode we decode
// nothing, so the request never arrives and the hello can never fire. Every measurement so
// far has therefore had us transmitting only the 1 Hz heartbeat and one registration probe.
//
// THE PANEL HAS NEVER ACTUALLY BEEN ANSWERED. Not once, in three days.
//
// We do not need to hear it to answer it: the request is unconditional and so is the reply.
// So drop to NORMAL and put the whole handshake on the wire from a table — heartbeat, the
// three hello frames, then a registration probe on every function id — then go back to
// listen-only, where reception is flawless, and read what the panel does next.
//
// ONLY THE HELLO GOES OUT BLIND, AND REGISTRATION DELIBERATELY DOES NOT.
//
// The hello is a pure reply to an unconditional request — fire and forget, no ACK, no state.
// Registration is the opposite: `70` on a funcId, WAIT for `74` on funcId|0x400, then the
// next one, and FUNCSREG is latched on those ACKs. Firing registration probes blind cannot
// complete anything — we would not see the ACK, the flag would stay clear, and we would have
// put unanswered probes on the bus for nothing. The library owns registration; it runs it
// properly the moment we can hear again.
//
// AND 0x1C1 IS NOT OURS TO SEND ON. The OEM capture shows `1C1 70 A3 A3 …` and it is easy to
// misread as the radio registering a third channel. THE FILLER SETTLES IT: 0xA3 is the
// PANEL's filler and 0x00 is the radio's, so that frame is the PANEL registering itself, and
// `5C1 74 00 …` is the radio acknowledging it. Same shape, opposite direction:
//
//     0x151  70 00 …  radio  ->  0x551  74 A3 …  panel ACKs
//     0x1F1  70 00 …  radio  ->  0x5F1  74 A3 …  panel ACKs
//     0x1C1  70 A3 …  PANEL  ->  0x5C1  74 00 …  RADIO ACKs
//
// Which is exactly right for a key channel: the joystick is on the panel, so keys flow one
// way and we answer them. funcs[] = {0x151, 0x1F1} was correct all along.
struct KickFrame { uint16_t id; uint8_t d[8]; };
const KickFrame kKick[] = {
  {0x3AF, {0xB9, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},   // alive — "a master is here"
  {0x3AF, {0x70, 0x1A, 0x11, 0x00, 0x00, 0x00, 0x00, 0x01}},   // hello 1
  {0x3AF, {0xB0, 0x14, 0x11, 0x00, 0x1F, 0x00, 0x00, 0x00}},   // hello 2
  {0x3AF, {0xB0, 0x14, 0x11, 0x00, 0x1F, 0x00, 0x00, 0x00}},   // hello 3 — identical, [CAP]
};
constexpr uint8_t kKickCount = sizeof(kKick) / sizeof(kKick[0]);
bool g_kickWanted = false;

bool     g_gateOpen    = false;   // our copy; g_link owns the truth
uint32_t g_gateOpenMs  = 0;       // when we last opened it
uint32_t g_reopenAtMs  = 0;       // when we may open it again after a backoff
uint32_t g_deafEvents  = 0;       // times reception died while we were talking

// ---------------------------------------------------------------------------
// ECC — the error-code-capture register, read for the first time on this bench
// ---------------------------------------------------------------------------
// Every diagnosis so far has argued from COUNTERS: rxErr pinned at 129, busErr at exactly
// the panel's frame rate, and the mechanism inferred. The controller has been holding the
// primary evidence the whole time. The SJA1000-compatible ECC register latches the TYPE
// (bit/form/stuff/other), DIRECTION (tx/rx) and the exact FIELD (SOF, data, CRC, ACK slot,
// error delimiter, intermission...) of the first bus error since it was last read — and
// reading it re-arms the capture. A histogram of those codes through one NORMAL-mode episode
// against the storm NAMES the failing bit, which is the difference between:
//
//   BIT errors at ACK-SLOT        our dominant never appears at our own sample point
//   FORM errors at the delimiters the panel starts its next frame early and a compliant
//   or around INTERMISSION        receiver answers with overload/error frames, for ever
//   STUFF/BIT in early fields     the controller trips over the frame AFTER an error
//
// Those need three different fixes, and no counter can tell them apart.
//
// The poller runs on its own low-priority task. It touches the registers ONLY while
// twai_get_status_info() succeeds: with the driver uninstalled the peripheral clock may be
// gated, and a raw register read is then a fault, not a zero. After a BUS-OFF sighting it
// stands back for 600 ms while library-owned direct-TWAI recovery may reconfigure the
// peripheral; losing a few poll beats costs nothing while reading mid-uninstall costs a
// panic. Reading ECC races the driver's own ISR on bus-error
// interrupts; the register keeps the captured value until the NEXT error overwrites it, so
// the race costs at worst one re-arm, never a wrong code.
namespace ecc {

struct Change { uint32_t ms; uint8_t raw; uint8_t mode; uint8_t rec; uint8_t tec; };

uint32_t g_hist[4][2][32];            // [errc][dir][seg]; racy reads fine, see soak counters
uint32_t g_samples      = 0;          // poller beats with the driver installed
uint32_t g_absent       = 0;          // beats skipped: driver uninstalled
uint32_t g_standoff     = 0;          // beats skipped: recent BUS-OFF, reinstall imminent
uint32_t g_empty        = 0;          // reads with nothing captured (raw == 0)
uint32_t g_changes      = 0;          // distinct raw-value transitions recorded
uint32_t g_inNormal     = 0;          // beats with lom == 0 (an ACK-driving mode)
uint32_t g_remasks      = 0;          // times BEIE had to be cleared (== driver reinstalls seen)
uint8_t  g_lastRaw      = 0;
volatile bool g_clear   = false;
Change   g_ring[24];                  // last distinct codes, oldest overwritten
uint8_t  g_ringHead     = 0;

const char* segName(uint8_t s) {
  switch (s) {
    case 0x03: return "SOF";          case 0x02: return "ID28-21";
    case 0x06: return "ID20-18";      case 0x04: return "SRTR";
    case 0x05: return "IDE";          case 0x07: return "ID17-13";
    case 0x0F: return "ID12-5";       case 0x0E: return "ID4-0";
    case 0x0C: return "RTR";          case 0x0D: return "RSVD1";
    case 0x09: return "RSVD0";        case 0x0B: return "DLC";
    case 0x0A: return "DATA";         case 0x08: return "CRC-SEQ";
    case 0x18: return "CRC-DELIM";    case 0x19: return "ACK-SLOT";
    case 0x1B: return "ACK-DELIM";    case 0x1A: return "EOF";
    case 0x12: return "INTERMISSION"; case 0x11: return "ACTIVE-ERR-FLAG";
    case 0x16: return "PASSIVE-ERR-FLAG"; case 0x13: return "TOLERATE-DOM";
    case 0x17: return "ERR-DELIM";    case 0x1C: return "OVERLOAD-FLAG";
    default:   return "?";
  }
}
const char* errcName(uint8_t e) {
  switch (e) {
    case 0:  return "BIT";
    case 1:  return "FORM";
    case 2:  return "STUFF";
    default: return "OTHER";
  }
}

// THE ONLY SAFE WAY TO TOUCH TWAI.* FROM A SIDE TASK. A library recovery or controlled
// setListenOnly() restart can uninstall the peripheral from another task, and an
// uninstalled peripheral has its clock gated: a raw
// register access then faults, it does not read zero. twai_get_status_info() alone is a
// TOCTOU check — the uninstall can land between it and the reads. So the reads run inside
// a critical section (single core: nothing else can run) with the peripheral's own
// clock-enable/reset bits as the authority. Returns false when the registers were not
// safely readable.
struct RegSnapshot { uint8_t ecc, mode, rec, tec; bool beieWasSet; };
portMUX_TYPE g_regMux = portMUX_INITIALIZER_UNLOCKED;

bool readTwaiRegs(RegSnapshot& out, bool maskBeie) {
  bool ok = false;
  portENTER_CRITICAL(&g_regMux);
  const bool clkOn = (REG_READ(SYSTEM_PERIP_CLK_EN0_REG) & SYSTEM_TWAI_CLK_EN) != 0;
  const bool inRst = (REG_READ(SYSTEM_PERIP_RST_EN0_REG) & SYSTEM_TWAI_RST) != 0;
  if (clkOn && !inRst) {
    out.beieWasSet = TWAI.interrupt_enable_reg.beie;
    if (maskBeie && out.beieWasSet) TWAI.interrupt_enable_reg.beie = 0;
    if (!maskBeie && !out.beieWasSet) TWAI.interrupt_enable_reg.beie = 1;
    out.ecc  = static_cast<uint8_t>(TWAI.error_code_capture_reg.val);
    out.mode = static_cast<uint8_t>(TWAI.mode_reg.val & 0x0F);
    out.rec  = static_cast<uint8_t>(TWAI.rx_error_counter_reg.rxerr);
    out.tec  = static_cast<uint8_t>(TWAI.tx_error_counter_reg.txerr);
    ok = true;
  }
  portEXIT_CRITICAL(&g_regMux);
  return ok;
}

// Whether the poller masks the bus-error interrupt. Masked (the default), the ISR never
// consumes the ECC capture and the histogram works — measured unmasked: 7618 polls, 7618
// empty, while busErr counted hundreds, because the ISR's re-arming read eats the capture
// microseconds after it latches. THE COST: the driver's bus_error_count FREEZES while
// masked, so /api/status's busErr line stops discriminating "silent bus" from "error
// storm" — /api/ecc's transitions counter and REC/TEC carry that job instead. /api/ecc?mask=0
// hands the interrupt back and busErr resumes.
volatile bool g_maskBeie = true;

// Opt-in. The probe preempts the poll task and touches BEIE on a live driver; see the note
// at its start() call site for the measurement that demoted it.
constexpr bool kEnableEccProbe = false;

void taskFn(void*) {
  uint32_t standoffMs = 0;
  for (;;) {
    vTaskDelay(1);                    // one tick = 1 ms: ~one read per error at storm rate
    if (g_clear) {
      memset(g_hist, 0, sizeof(g_hist));
      memset(g_ring, 0, sizeof(g_ring));
      g_samples = g_absent = g_standoff = g_empty = g_changes = g_inNormal = g_remasks = 0;
      g_ringHead = 0;
      g_lastRaw  = 0;
      g_clear    = false;
    }
    twai_status_info_t st;
    if (twai_get_status_info(&st) != ESP_OK) { ++g_absent; continue; }
    const uint32_t now = millis();
    // Stand back 600 ms after ANY non-RUNNING sighting, not just BUS_OFF: the reinstalls
    // this firmware performs (deaf recovery, mode switches) start from RUNNING, and the
    // window right after a state excursion is where the uninstall lives.
    if (st.state != TWAI_STATE_RUNNING) standoffMs = now ? now : 1;
    if (standoffMs && now - standoffMs < 600) { ++g_standoff; continue; }
    standoffMs = 0;

    RegSnapshot r;
    if (!readTwaiRegs(r, g_maskBeie)) { ++g_absent; continue; }

    ++g_samples;
    if (!(r.mode & 0x02)) ++g_inNormal;         // MOD.1 = listen-only
    if (r.beieWasSet && g_maskBeie) ++g_remasks;

    if (r.ecc == 0) { ++g_empty; g_lastRaw = 0; continue; }
    const uint8_t seg  = r.ecc & 0x1F;
    const uint8_t dir  = (r.ecc >> 5) & 0x01;
    const uint8_t code = (r.ecc >> 6) & 0x03;
    ++g_hist[code][dir][seg];
    if (r.ecc != g_lastRaw) {
      ++g_changes;
      Change& c = g_ring[g_ringHead];
      c.ms   = now ? now : 1;
      c.raw  = r.ecc;
      c.mode = r.mode;
      c.rec  = r.rec;
      c.tec  = r.tec;
      g_ringHead = static_cast<uint8_t>((g_ringHead + 1)
                     % (sizeof(g_ring) / sizeof(g_ring[0])));
      g_lastRaw = r.ecc;
    }
  }
}

void start() { xTaskCreate(&taskFn, "ecc", 2048, nullptr, 3, nullptr); }

}  // namespace ecc

// Soak counters. Read from the HTTP task without a lock: aligned 32-bit reads do not tear on
// this core, and a diagnostic that is one repaint stale is still a correct diagnostic.
uint32_t g_repaints    = 0;
uint32_t g_renderFails = 0;
uint32_t g_timeouts    = 0;
uint32_t g_syncLosses  = 0;
uint32_t g_nextSoakMs  = kSoakReportMs;

// Row 0 is the goal state and it HOLDS STILL: setup() gives it scroll 0, so the word is
// readable from across the room the moment the first paint lands, rather than sliding
// past. Row 1 scrolls to prove the link is alive. Row 2 is generated — the uptime clock.
char g_text0[AFFA_TEXT_MAX] = "SUCCESS";
char g_text1[AFFA_TEXT_MAX] = "HELLO ABADON - WELCOME FROM HELL";

// ---------------------------------------------------------------------------
// Row 2: the clock row, and the reason RowScreen exists at all
// ---------------------------------------------------------------------------
// Its text changes EVERY SECOND. Under a marquee's normal policy a changed string restarts
// at character 0, so this row would be pinned to the start for ever and would flicker once a
// second instead of scrolling. RowScreen publishes with Marquee::Phase::Keep, which makes
// the scroll phase a property of the ROW rather than of the string — so a row can scroll and
// tell the time at once. Nothing here has to know that; it just works.
void buildClockRow(uint32_t upMs, char* out, size_t n) {
  const uint32_t s = upMs / 1000;
  snprintf(out, n, "UP %02lu:%02lu:%02lu  PAINTS %lu  POPUPS %lu",
           static_cast<unsigned long>(s / 3600),
           static_cast<unsigned long>((s / 60) % 60),
           static_cast<unsigned long>(s % 60),
           static_cast<unsigned long>(g_repaints),
           static_cast<unsigned long>(g_popupCount));
}

void restart(const char* why) {
  logmsg("sequence restart: %s", why);
  g_step         = Step::WaitPanel;
  g_inFlight     = 0;
  g_powerIsOn    = false;
  g_popupUp      = false;
  g_clockOk      = false;       // the panel may have lost power; the clock goes out again
  g_clockReq     = 0;
  g_clockRetryAt = ::millis();  // NOT 0: expired(now, 0) is FALSE for the entire second
                                // half of the millis() range (signed compare), which
                                // would park the retry for ~25 days after a sync loss on
                                // a long soak. Stamping "due now" is wrap-proof.
  g_rows.invalidate();          // whatever was on the glass, assume it is gone
}

// Enqueue one render and arm the wait. Every issue path goes through here so the timeout and
// the counters cannot disagree.
bool issue(affa::rtos::TxRequest req, uint32_t now, const char* what) {
  if (req == affa::rtos::kNoRequest) {
    // The command queue refused: it is full, or the task is not running. Not worth a retry
    // this iteration — the next one is 10 ms away.
    logmsg("%s: command queue refused", what);
    return false;
  }
  g_inFlight    = static_cast<uint32_t>(req);
  g_flightSince = now;
  return true;
}

// ---------------------------------------------------------------------------
// THE LIVE SCREEN — this is the whole example, and it is nineteen lines
// ---------------------------------------------------------------------------
void pumpScreen(uint32_t now) {
  // ---- publish the content ------------------------------------------------
  // Every iteration, unconditionally, with no change detection of our own. setText() is a
  // no-op when the string is identical and does not disturb the scroll, so this costs a
  // strcmp and is the intended way to use the widget: publish what is true, let it work out
  // whether anything moved.
  char clock[AFFA_TEXT_MAX];
  buildClockRow(now, clock, sizeof(clock));
  g_rows.setRows(g_text0, g_text1, clock, now);

  // ---- the popup, which owns the glass while it is up ---------------------
  if (g_popupUp) {
    if (!affa::expired(now, g_popupUntil)) return;   // still showing; do not repaint under it
    if (g_inFlight) return;                          // wait for our own last render first
    if (issue(g_task.hidePopup(), now, "hidePopup")) {
      g_popupUp = false;
      g_popupNextMs = now + kPopupEveryMs;
      // The rows have not moved as far as RowScreen knows, but the GLASS has changed under
      // them. invalidate() is exactly this case: force one repaint whatever the content says.
      g_rows.invalidate();
    }
    return;
  }

  if (affa::expired(now, g_popupNextMs)) {
    if (g_inFlight) return;
    char msg[AFFA_TEXT_MAX];
    snprintf(msg, sizeof(msg), "POPUP #%lu", static_cast<unsigned long>(g_popupCount + 1));
    if (issue(g_task.showPopupText(msg), now, "showPopupText")) {
      ++g_popupCount;
      g_popupUp    = true;
      g_popupUntil = now + kPopupHoldMs;
    }
    return;
  }

  // ---- the rows -----------------------------------------------------------
  // ONE RENDER IN FLIGHT AT A TIME. This is the self-limiting part: the next screen is not
  // even built until the panel has acknowledged the last one, so the repaint rate is
  // whatever the wire actually sustains and the screen can never queue itself into the past.
  if (g_inFlight) return;
  if (!g_rows.dirty(now)) return;

  char win[RowScreen::kRows][RowScreen::kCell];
  g_rows.render(now, win);

  if (!issue(g_task.showFullscreenText(win[0], win[1], win[2]), now, "showFullscreenText"))
    return;

  // LATCH ON ACCEPTANCE, NOT ON COMPLETION. A render that is superseded before it reaches
  // the wire is still the newest value for its slot, so latching later would repaint a
  // window the panel has already been given.
  g_rows.painted(now);
  ++g_repaints;
}

// ---------------------------------------------------------------------------
// The gate FSM — silent until the bus proves itself, silent again the moment it stops
// ---------------------------------------------------------------------------
// The shout mode change is driven from loop(). setListenOnly() performs a controlled direct
// TWAI restart, so it can briefly pause traffic while the RX task and driver are replaced.
void pumpShout(uint32_t now) {
  if (g_shoutPhase == 1) {
    g_link.setListenOnly(false);
    g_link.setTxEnabled(true);
    g_gateOpen   = true;
    g_gateOpenMs = now;
    g_shoutUntil = now + g_shoutMs;
    g_shoutPhase = 2;
    logmsg("SHOUT: normal + gate open for %lu ms - talking blind",
           static_cast<unsigned long>(g_shoutMs));

    if (g_kickWanted) {
      g_kickWanted = false;
      // Straight onto the wire, bypassing the library's FSM entirely. That is the whole
      // point: the FSM will not emit a hello it has not been asked for, and it cannot be
      // asked for while we are deaf.
      uint8_t sent = 0;
      for (uint8_t i = 0; i < kKickCount; ++i) {
        affa::Frame f;
        f.id  = kKick[i].id;
        f.ext = false;
        f.len = 8;
        memcpy(f.data, kKick[i].d, 8);
        if (g_link.send(f)) ++sent;
        delay(4);        // the factory spaces its registration probes; do not burst them
      }
      logmsg("KICK: %u/%u handshake frames on the wire (hello x3 + reg 151/1C1/1F1)",
             static_cast<unsigned>(sent), static_cast<unsigned>(kKickCount));
    }
    return;
  }
  if (g_shoutPhase == 2 && affa::expired(now, g_shoutUntil)) {
    g_link.setTxEnabled(false);
    g_gateOpen   = false;
    g_reopenAtMs = now + 3600000UL;      // stay put; this is a measurement, not a retry loop
    g_link.setListenOnly(true);
    g_shoutPhase = 0;
    logmsg("SHOUT done - back to listen-only. Compare /api/frames with before: a panel that "
           "heard us stops repeating 61 11 01");
  }
}

void pumpGate(uint32_t now) {
  if (g_shoutPhase) return;              // the shout owns the gate while it runs
  uint32_t rxSeen, lastRx;
  portENTER_CRITICAL(&g_frameMux);
  rxSeen = g_rxSeen;
  lastRx = g_lastRxMs;
  portEXIT_CRITICAL(&g_frameMux);

  if (!g_gateOpen) {
    if (!affa::expired(now, g_reopenAtMs)) return;      // serving a backoff
    if (now < kQuietMinMs) return;                      // never speak in the first seconds
    if (rxSeen < kHeardEnough) return;                  // we have not heard enough to trust it

    g_gateOpen   = true;
    g_gateOpenMs = now;
    g_link.setTxEnabled(true);
    logmsg("gate OPEN after %lu frames heard (silent for %lu ms) - the library may talk now",
           static_cast<unsigned long>(rxSeen), static_cast<unsigned long>(now));
    return;
  }

  // ---- talking. Watch for the bus going quiet under us. -------------------
  if (rxSeen == 0) return;                              // cannot go deaf without ever hearing

  // HEARD SINCE WE OPENED, or not yet? These are different situations and collapsing them
  // was a bug: `lastRx` from BEFORE the gate opened made every open look instantly deaf, so
  // the gate slammed shut in the same loop iteration it opened in and we could never hold a
  // conversation long enough to see whether the other end reacted.
  if (lastRx < g_gateOpenMs) {
    if (!affa::expired(now, g_gateOpenMs + kShoutMs)) return;   // give it a fair hearing
  } else if (!affa::expired(now, lastRx + kDeafMs)) {
    return;                                             // still hearing it
  }

  ++g_deafEvents;
  // FREEZING IS OPT-IN NOW. It was built to catch the first moment of a fault nobody
  // understood, and it earned its keep — but that fault is settled (single-shot), and a
  // trace that stops recording four seconds after the last RX is useless as a WIRE LOG:
  // it self-destructs exactly when you want to watch a handshake retry. Default is a
  // rolling ring; set kFreezeTraceOnDeaf to get the old death-snapshot behaviour back.
  bool froze = false;
  if (kFreezeTraceOnDeaf) {
    portENTER_CRITICAL(&g_frameMux);
    froze = !g_traceFrozen;
    if (froze) { g_traceFrozen = true; g_frozenAtMs = now; }
    portEXIT_CRITICAL(&g_frameMux);
  }

  logmsg("DEAF after %lu ms of talking, %lu frames heard - trace %s",
         static_cast<unsigned long>(now - g_gateOpenMs),
         static_cast<unsigned long>(rxSeen),
         froze ? "FROZEN (see /api/trace)" : "already frozen");

  // ---- AND THAT IS ALL. The reinstall is THE LIBRARY'S JOB now. -------------------------
  //
  // This handler used to cycle the driver through listen-only and blind-kick the handshake
  // itself, because "RUNNING but deaf" was a state the library's recovery could not see —
  // pumpLink() watched healthy(), which is only "state == RUNNING". Since 0.4.0 the library
  // owns exactly this case: AFFA_RX_STALL_MS latches the silence, recover(force=true)
  // reinstalls even a RUNNING controller, and the handshake is re-run from the top. Keeping
  // a SECOND reinstall here would race the library's — two unsynchronised
  // uninstall/install cycles from different tasks against one driver is how the
  // dangling-handle teardown bug fires. The example's remaining job is what the library
  // cannot do: freeze the trace so a human can read what the transition looked like.
  g_gateOpenMs = now;          // one deaf event per silence, not one per kDeafMs tick
}

void soakReport(uint32_t now) {
  if (!affa::expired(now, g_nextSoakMs)) return;
  g_nextSoakMs = now + kSoakReportMs;

  const affa::rtos::Status st  = g_task.status();
  const affa::Stats        ls  = g_link.stats();
  const auto               lh  = g_display.linkHealth();
  const auto               drv = g_link.driverState();

  logmsg("soak up=%lus paints=%lu popups=%lu fail=%lu tmo=%lu synclost=%lu "
         "rx=%lu tx=%lu rxerr=%lu txerr=%lu buserr=%lu st=%u ovf=%lu flaps=%lu rec=%lu/%lu "
         "late=%luus stack=%lu",
         static_cast<unsigned long>(now / 1000),
         static_cast<unsigned long>(g_repaints),
         static_cast<unsigned long>(g_popupCount),
         static_cast<unsigned long>(g_renderFails),
         static_cast<unsigned long>(g_timeouts),
         static_cast<unsigned long>(g_syncLosses),
         static_cast<unsigned long>(ls.rxFrames),
         static_cast<unsigned long>(ls.txFrames),
         static_cast<unsigned long>(ls.rxErr),
         static_cast<unsigned long>(ls.txErr),
         static_cast<unsigned long>(drv.busErr),
         static_cast<unsigned>(drv.valid ? drv.state : 9),
         static_cast<unsigned long>(ls.ringOverflow),
         static_cast<unsigned long>(lh.flaps),
         static_cast<unsigned long>(lh.recoveries),
         static_cast<unsigned long>(lh.failures),
         static_cast<unsigned long>(st.pollLateMaxUs),
         static_cast<unsigned long>(st.stackFreeBytes));
}

}  // namespace

// ===========================================================================
// THE DEPLOYMENT SCAFFOLDING — WiFi, the console and OTA
// ===========================================================================
// Everything below this line is about reaching a board with no cable on it. None of it is
// AffaDisplay: delete it, drop the WiFi includes, and the example above still runs exactly
// as it does now with the speeds compiled in.

namespace {

constexpr const char* kWifiNamespace = "megaopen";   // read-only: ssid / pass
constexpr const char* kApSsid        = "AffaRows";
constexpr const char* kApPass        = "affarows1";
constexpr const char* kMdnsName      = "affarows";
constexpr uint32_t    kStaJoinMs     = 15000;

PsychicHttpServer g_server;
bool     g_otaRunning = false;
uint32_t g_rebootAt   = 0;

const char kPage[] PROGMEM = R"HTML(<!doctype html><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>AffaDisplay 04_rows</title>
<style>
body{font:14px/1.5 system-ui,sans-serif;margin:0;padding:16px;background:#111;color:#ddd}
h1{font-size:16px;margin:0 0 12px}
fieldset{border:1px solid #333;border-radius:6px;margin:0 0 12px;padding:10px}
legend{color:#8ab;padding:0 6px}
button{font:13px system-ui;background:#243;color:#cfe;border:1px solid #465;border-radius:4px;
padding:5px 10px;margin:2px;cursor:pointer}
button:hover{background:#365}
pre{background:#000;border:1px solid #333;border-radius:6px;padding:8px;overflow:auto;
max-height:40vh;font:12px/1.4 ui-monospace,monospace;color:#9c9}
input{background:#000;color:#ddd;border:1px solid #444;border-radius:4px;padding:4px;width:60%}
.row{margin:6px 0}
a{color:#8ab}
</style>
<h1>AffaDisplay <span id=v>04_rows</span> &middot; <a href=/update>OTA</a></h1>

<fieldset><legend>display</legend>
<button onclick=go('/api/power?on=1')>power on</button>
<button onclick=go('/api/power?on=0')>power off</button>
<button onclick=go('/api/popup?now=1')>popup now</button>
<button onclick=go('/api/reboot')>reboot</button>
</fieldset>

<fieldset><legend>wire &mdash; is the panel talking?</legend>
<button onclick=go('/api/kick')>KICK &mdash; answer the panel blind</button>
<button onclick=go('/api/listen?on=1')>LISTEN-ONLY (do not ACK)</button>
<button onclick=go('/api/listen?on=0')>NORMAL (ACK again)</button>
<button onclick=go('/api/txgate?on=0')>TX gate SHUT (go silent)</button>
<button onclick=go('/api/txgate?on=1')>TX gate open</button>
<button onclick=go('/api/trace')>re-arm trace</button>
<div class=row>The board boots ARMED &mdash; NORMAL mode, gate open, ACKing and answering
from the panel's very first frame, because the seconds after power-on are the only window
in which a fresh panel is polite. If reception stops while it is talking, the trace below
<b>freezes</b> so the last frames before the bus died stay readable. Identical frames are
collapsed into one row with a repeat count.</div>
<pre id=f>...</pre>
</fieldset>

<fieldset><legend>rows</legend>
<div class=row>row 0
<button onclick=go('/api/speed?row=0&ms=200')>200</button>
<button onclick=go('/api/speed?row=0&ms=400')>400</button>
<button onclick=go('/api/speed?row=0&ms=800')>800</button>
<button onclick=go('/api/speed?row=0&ms=0')>stop</button>
<button onclick=go('/api/dir?row=0&rev=0')>&rarr;</button>
<button onclick=go('/api/dir?row=0&rev=1')>&larr;</button></div>
<div class=row>row 1
<button onclick=go('/api/speed?row=1&ms=400')>400</button>
<button onclick=go('/api/speed?row=1&ms=800')>800</button>
<button onclick=go('/api/speed?row=1&ms=1600')>1600</button>
<button onclick=go('/api/speed?row=1&ms=0')>stop</button>
<button onclick=go('/api/dir?row=1&rev=0')>&rarr;</button>
<button onclick=go('/api/dir?row=1&rev=1')>&larr;</button></div>
<div class=row>row 2
<button onclick=go('/api/speed?row=2&ms=600')>600</button>
<button onclick=go('/api/speed?row=2&ms=1200')>1200</button>
<button onclick=go('/api/speed?row=2&ms=2400')>2400</button>
<button onclick=go('/api/speed?row=2&ms=0')>stop</button>
<button onclick=go('/api/dir?row=2&rev=0')>&rarr;</button>
<button onclick=go('/api/dir?row=2&rev=1')>&larr;</button></div>
<div class=row><input id=t0 placeholder="text for row 0">
<button onclick="go('/api/text?row=0&t='+encodeURIComponent(t0.value))">set</button></div>
<div class=row><input id=t1 placeholder="text for row 1">
<button onclick="go('/api/text?row=1&t='+encodeURIComponent(t1.value))">set</button></div>
</fieldset>

<fieldset><legend>status</legend><pre id=s>...</pre></fieldset>
<fieldset><legend>log</legend><pre id=l>...</pre></fieldset>

<script>
// ONE TIMER, AND IT IS CHAINED RATHER THAN setInterval. A board whose HTTP task is busy will
// answer slowly; setInterval would keep stacking requests on top of it and hold every socket
// in the table, which is how this board used to become permanently unreachable.
async function go(u){ try{ await fetch(u) }catch(e){} tick() }
async function tick(){
  try{
    s.textContent = await (await fetch('/api/status')).text()
    f.textContent = await (await fetch('/api/frames')).text()
    l.textContent = await (await fetch('/api/log')).text()
  }catch(e){ s.textContent = 'offline' }
  setTimeout(tick, 2000)
}
tick()
</script>
)HTML";

// twai_state_t, spelled. `valid == false` means twai_get_status_info() itself failed, which
// on this driver means it is mid-reinstall — a snapshot, not a verdict.
const char* drvStateName(bool valid, uint8_t s) {
  if (!valid) return "reinstalling";
  switch (s) {
    case 0: return "STOPPED";
    case 1: return "running";
    case 2: return "BUS-OFF";
    case 3: return "recovering";
    default: return "?";
  }
}

uint8_t rowArg(PsychicRequest* r) {
  const int v = r->getParam("row") ? r->getParam("row")->value().toInt() : 0;
  return static_cast<uint8_t>(v < 0 ? 0 : (v > 2 ? 2 : v));
}

void routes() {
  g_server.on("/", HTTP_GET, [](PsychicRequest* r) {
    return r->reply(200, "text/html", kPage);
  });

  g_server.on("/api/status", HTTP_GET, [](PsychicRequest* r) {
    const affa::rtos::Status st  = g_task.status();
    const affa::Stats        ls  = g_link.stats();
    const auto               lh  = g_display.linkHealth();
    const auto               drv = g_link.driverState();
    uint32_t rxSeen, txSeen, lastRx;
    portENTER_CRITICAL(&g_frameMux);
    rxSeen = g_rxSeen; txSeen = g_txSeen; lastRx = g_lastRxMs;
    portEXIT_CRITICAL(&g_frameMux);
    const uint32_t nowMs = millis();

    char b[900];
    snprintf(b, sizeof(b),
             "uptimeMs   %lu\n"
             "step       %s\n"
             "           power %s   popup %s (#%lu)   inFlight %lu   txGate %s   clock %s\n"
             "heard      rx %lu   tx %lu   lastRx %lu ms ago%s\n"
             "mode       %s\n"
             "gate       %s   deafEvents %lu   trace %s\n"
             "sync       0x%02X   registered %u   busy %u\n"
             "speeds     %lu / %lu / %lu ms   dir %c%c%c\n"
             "paints     %lu   fails %lu   timeouts %lu   syncLost %lu\n"
             "link       rx %lu  tx %lu  rxErr %lu  txErr %lu  txFailed %lu  drop %lu  ovf %lu\n"
             // THE LINE THAT SEPARATES A SILENT BUS FROM AN ERROR STORM, and it comes from
             // the driver rather than from us. busErr FROZEN with rxErr 0 is a bus with
             // nothing on it; busErr climbing by hundreds a second is a bus full of traffic
             // we cannot decode. Both look identical from rxFrames alone, which is 0 either
             // way, and confusing the two costs hours.
             "driver     %s state %u (%s)  busErr %lu  arbLost %lu  rxMissed %lu  restarts %lu\n"
             "linkHealth flaps %lu  recovered %lu  failed %lu  downMs %lu\n"
             "task       iters %lu  lateMax %luus @%lu  dropped %lu  stackFree %lu\n",
             static_cast<unsigned long>(nowMs),
             stepName(g_step),
             g_powerIsOn ? "on" : "off",
             g_popupUp ? "up" : "down", static_cast<unsigned long>(g_popupCount),
             static_cast<unsigned long>(g_inFlight), g_gateOpen ? "open" : "SHUT",
             g_clockOk ? "SET 10:00" : "not set",
             static_cast<unsigned long>(rxSeen), static_cast<unsigned long>(txSeen),
             static_cast<unsigned long>(rxSeen ? nowMs - lastRx : 0),
             rxSeen ? "" : "   <-- WE HAVE NEVER HEARD THE PANEL",
             g_link.listenOnly() ? "LISTEN-ONLY  we do not acknowledge anything"
                                : "NORMAL       we ACK every frame in hardware",
             g_gateOpen ? "OPEN (we are talking)" : "shut (listening only)",
             static_cast<unsigned long>(g_deafEvents),
             g_traceFrozen ? "FROZEN - see /api/trace" : "live",
             static_cast<unsigned>(st.sync), st.registered ? 1u : 0u, st.busy ? 1u : 0u,
             static_cast<unsigned long>(g_rows.scroll(0)),
             static_cast<unsigned long>(g_rows.scroll(1)),
             static_cast<unsigned long>(g_rows.scroll(2)),
             g_rows.direction(0) == Marquee::Direction::Forward ? '>' : '<',
             g_rows.direction(1) == Marquee::Direction::Forward ? '>' : '<',
             g_rows.direction(2) == Marquee::Direction::Forward ? '>' : '<',
             static_cast<unsigned long>(g_repaints),
             static_cast<unsigned long>(g_renderFails),
             static_cast<unsigned long>(g_timeouts),
             static_cast<unsigned long>(g_syncLosses),
             static_cast<unsigned long>(ls.rxFrames), static_cast<unsigned long>(ls.txFrames),
             static_cast<unsigned long>(ls.rxErr), static_cast<unsigned long>(ls.txErr),
             static_cast<unsigned long>(ls.txFailed),
             static_cast<unsigned long>(ls.txDropped),
             static_cast<unsigned long>(ls.ringOverflow),
             drv.valid ? "ok" : "UNREADABLE", static_cast<unsigned>(drv.state),
             drvStateName(drv.valid, drv.state),
             static_cast<unsigned long>(drv.busErr),
             static_cast<unsigned long>(drv.arbLost),
             static_cast<unsigned long>(drv.rxMissed),
             static_cast<unsigned long>(g_link.restarts()),
             static_cast<unsigned long>(lh.flaps),
             static_cast<unsigned long>(lh.recoveries),
             static_cast<unsigned long>(lh.failures),
             static_cast<unsigned long>(lh.downMs),
             static_cast<unsigned long>(st.iterations),
             static_cast<unsigned long>(st.pollLateMaxUs),
             static_cast<unsigned long>(st.pollLateAtMs),
             static_cast<unsigned long>(st.queueDropped),
             static_cast<unsigned long>(st.stackFreeBytes));
    return r->reply(200, "text/plain", b);
  });

  g_server.on("/api/log", HTTP_GET, [](PsychicRequest* r) {
    // Copy under the lock, format outside it. Formatting inside a critical section on the
    // HTTP task is how you stall the CAN poll task from a browser refresh.
    LogRec snap[kLogRing];
    uint8_t head;
    portENTER_CRITICAL(&g_logMux);
    memcpy(snap, g_log, sizeof(snap));
    head = g_logHead;
    portEXIT_CRITICAL(&g_logMux);

    String out;
    out.reserve(kLogRing * 72);
    for (uint8_t i = 0; i < kLogRing; ++i) {
      const LogRec& e = snap[(head + i) % kLogRing];
      if (!e.msg[0]) continue;
      char line[128];
      snprintf(line, sizeof(line), "%8lu  %s\n", static_cast<unsigned long>(e.ms), e.msg);
      out += line;
    }
    return r->reply(200, "text/plain", out.c_str());
  });

  // THE RAW WIRE, oldest first. This is the answer to "what does the display send us": if
  // this shows only TX lines, the other end is not talking and no amount of render debugging
  // will help.
  g_server.on("/api/frames", HTTP_GET, [](PsychicRequest* r) {
    // STATIC, not a local. This snapshot is ~900 B and esp_http_server's task stack is what
    // the config above sets; a handler that overflows it dies silently and returns an empty
    // body. Every handler runs on that one task, so two snapshots are never taken at once.
    static FrameRec snap[kFrameRing];
    uint8_t  head;
    uint32_t seq, frozenAt;
    bool     frozen;
    portENTER_CRITICAL(&g_frameMux);
    memcpy(snap, g_frames, sizeof(snap));
    head     = g_frameHead;
    seq      = g_frameSeq;
    frozen   = g_traceFrozen;
    frozenAt = g_frozenAtMs;
    portEXIT_CRITICAL(&g_frameMux);

    String out;
    out.reserve(kFrameRing * 64 + 128);
    char hdr[160];
    snprintf(hdr, sizeof(hdr), "%lu frames observed. trace %s%s\n"
                               "  t-first   t-last   xN  dir  id  data\n",
             static_cast<unsigned long>(seq),
             frozen ? "FROZEN at " : "live",
             frozen ? String(frozenAt).c_str() : "");
    out += hdr;
    for (uint8_t i = 0; i < kFrameRing; ++i) {
      const FrameRec& f = snap[(head + i) % kFrameRing];
      if (!f.count) continue;                     // never written
      char line[112];
      int n = snprintf(line, sizeof(line), "%9lu %8lu %5lu  %s  %03X ",
                       static_cast<unsigned long>(f.firstMs),
                       static_cast<unsigned long>(f.lastMs),
                       static_cast<unsigned long>(f.count),
                       f.dir ? "TX" : "RX", static_cast<unsigned>(f.id));
      for (uint8_t b = 0; b < f.len && b < 8 && n < static_cast<int>(sizeof(line)) - 4; ++b)
        n += snprintf(line + n, sizeof(line) - n, " %02X", static_cast<unsigned>(f.d[b]));
      out += line;
      out += '\n';
    }
    return r->reply(200, "text/plain", out.c_str());
  });

  // Re-arm: clear the ring and let it record again. The whole ring, not just the flag —
  // a re-armed trace that still holds the last death is worse than no trace.
  // RAW FRAME INJECTION — drive the handshake by hand.
  //   /api/send?id=3AF&d=BA00000000000000        one BA, exactly as the OEM radio sends it
  //   /api/send?id=3AF&d=B90000000000000000      alive
  //   /api/send?id=3AF&d=B014110 01F000000       (spaces/odd nibbles are rejected, not padded)
  //   /api/send?id=151&d=0556313030300000        the clock, "1000"
  // Bytes are zero-padded to DLC 8, which is what every frame on this bus is. The frame goes
  // out through the SAME link the library uses, so it appears in /api/frames as TX and is
  // subject to the same TX gate.
  g_server.on("/api/send", HTTP_GET, [](PsychicRequest* r) {
    const String idStr  = r->getParam("id")  ? r->getParam("id")->value()  : String();
    const String hexStr = r->getParam("d")   ? r->getParam("d")->value()   : String();
    if (idStr.isEmpty() || hexStr.isEmpty())
      return r->reply(400, "text/plain", "usage: /api/send?id=3AF&d=BA00000000000000\n");

    const long id = strtol(idStr.c_str(), nullptr, 16);
    if (id <= 0 || id > 0x7FF)
      return r->reply(400, "text/plain", "id must be 1..7FF hex\n");
    if ((hexStr.length() % 2) != 0 || hexStr.length() > 16)
      return r->reply(400, "text/plain", "d must be an even number of hex digits, max 16\n");

    affa::Frame f{};
    f.id  = static_cast<uint32_t>(id);
    f.len = 8;                                   // every frame on this bus is DLC 8
    for (size_t i = 0; i < hexStr.length(); i += 2) {
      char byteHex[3] = {hexStr[i], hexStr[i + 1], 0};
      char* end = nullptr;
      const long v = strtol(byteHex, &end, 16);
      if (end != byteHex + 2)
        return r->reply(400, "text/plain", "d contains a non-hex digit\n");
      f.data[i / 2] = static_cast<uint8_t>(v);
    }

    const affa::TxDisposition sent = g_link.trySend(f);
    char msg[96];
    snprintf(msg, sizeof(msg), "0x%03lX %s\n", static_cast<unsigned long>(f.id),
             sent == affa::TxDisposition::Accepted ? "accepted by the controller"
             : sent == affa::TxDisposition::Busy   ? "BUSY - controller queue full"
                                                   : "REJECTED - link down");
    logmsg("console: raw send 0x%03lX %s", static_cast<unsigned long>(f.id),
           sent == affa::TxDisposition::Accepted ? "accepted" : "refused");
    return r->reply(sent == affa::TxDisposition::Accepted ? 200 : 503, "text/plain", msg);
  });

  g_server.on("/api/trace", HTTP_GET, [](PsychicRequest* r) {
    portENTER_CRITICAL(&g_frameMux);
    memset(g_frames, 0, sizeof(g_frames));
    g_frameHead   = 0;
    g_traceFrozen = false;
    g_frozenAtMs  = 0;
    portEXIT_CRITICAL(&g_frameMux);
    logmsg("console: trace re-armed");
    return r->reply(200, "text/plain", "re-armed");
  });

  // THE PAD AUDIT. The ECC verdict is "every dominant we drive reads back recessive" — this
  // answers whether the dominant ever leaves the chip. Dumps the GPIO matrix routing and
  // output-enable for the TX pad, then samples both pads at register-read speed (~25 M/s)
  // for ~16 ms. CRX (GPIO3) is the control: the storm guarantees it pulses low constantly,
  // so if the sampler sees GPIO3 move and GPIO4 never leave HIGH while the controller is
  // ACKing in NORMAL, the dominant is not reaching the pad — a chip-side routing fault.
  // GPIO4 pulsing low here while ECC still reports ACK-slot bit errors would mean the level
  // IS on the pad and the loop back through CRX is where it dies.
  //
  // FUN_IE (the pad's input buffer) is force-enabled for both pins first: the driver
  // configures TX as output-only, and with the buffer off GPIO.in reads a constant 0 which
  // looks exactly like "held dominant". Enabling it does not disturb the output path.
  g_server.on("/api/pad", HTTP_GET, [](PsychicRequest* r) {
    static char out[1700];
    size_t n = 0;
    twai_status_info_t st;
    const bool up = twai_get_status_info(&st) == ESP_OK;
    const uint32_t iomux3 = REG_READ(IO_MUX_GPIO3_REG);
    const uint32_t iomux4 = REG_READ(IO_MUX_GPIO4_REG);
    n += snprintf(out + n, sizeof(out) - n,
                  "driver     %s state %d\n"
                  "out_sel[4] func %u inv %u oen_sel %u oen_inv %u   enable.4 %u\n"
                  "in_sel[74] gpio %u sig_in_sel %u inv %u          (74 = TWAI RX/TX idx)\n"
                  "iomux4     0x%08X (MCU_SEL %u FUN_IE %u)  iomux3 0x%08X (MCU_SEL %u FUN_IE %u)\n",
                  up ? "ok" : "ABSENT", up ? static_cast<int>(st.state) : -1,
                  static_cast<unsigned>(GPIO.func_out_sel_cfg[4].func_sel),
                  static_cast<unsigned>(GPIO.func_out_sel_cfg[4].inv_sel),
                  static_cast<unsigned>(GPIO.func_out_sel_cfg[4].oen_sel),
                  static_cast<unsigned>(GPIO.func_out_sel_cfg[4].oen_inv_sel),
                  static_cast<unsigned>((GPIO.enable.val >> 4) & 1),
                  static_cast<unsigned>(GPIO.func_in_sel_cfg[TWAI_RX_IDX].func_sel),
                  static_cast<unsigned>(GPIO.func_in_sel_cfg[TWAI_RX_IDX].sig_in_sel),
                  static_cast<unsigned>(GPIO.func_in_sel_cfg[TWAI_RX_IDX].sig_in_inv),
                  static_cast<unsigned>(iomux4),
                  static_cast<unsigned>((iomux4 >> 12) & 7),
                  static_cast<unsigned>((iomux4 >> 9) & 1),
                  static_cast<unsigned>(iomux3),
                  static_cast<unsigned>((iomux3 >> 12) & 7),
                  static_cast<unsigned>((iomux3 >> 9) & 1));

    PIN_INPUT_ENABLE(IO_MUX_GPIO3_REG);
    PIN_INPUT_ENABLE(IO_MUX_GPIO4_REG);

    uint32_t low3 = 0, low4 = 0, edges4 = 0, run4 = 0, maxRun4 = 0;
    uint32_t bothLow = 0, drive4crx3High = 0;
    bool last4 = true;
    constexpr uint32_t kSamples = 400000;
    const uint32_t t0 = micros();
    for (uint32_t i = 0; i < kSamples; ++i) {
      const uint32_t v = GPIO.in.val;
      const bool b3 = v & (1u << 3);
      const bool b4 = v & (1u << 4);
      if (!b4) {
        ++low4; if (++run4 > maxRun4) maxRun4 = run4;
        // THE VERDICT COUNTERS: while WE hold TXD dominant, what does CRX say? Same
        // register read, so the two bits are the same instant. Loop delay through the
        // transceiver is ~1 sample; anything beyond edge effects is the answer.
        if (!b3) ++bothLow; else ++drive4crx3High;
      } else run4 = 0;
      if (!b3) ++low3;
      if (b4 != last4) { ++edges4; last4 = b4; }
    }
    const uint32_t us = micros() - t0;
    n += snprintf(out + n, sizeof(out) - n,
                  "sampled    %lu reads in %lu us (%lu ns/read)\n"
                  "GPIO4 CTX  low %lu (%lu.%04lu%%)  edges %lu  longest-low-run %lu reads\n"
                  "GPIO3 CRX  low %lu (%lu.%04lu%%)   <- control: must move during the storm\n"
                  "correlate  while CTX low: CRX low %lu, CRX HIGH %lu   <- HIGH means the "
                  "dominant never came back\n",
                  static_cast<unsigned long>(kSamples), static_cast<unsigned long>(us),
                  static_cast<unsigned long>((us * 1000UL) / kSamples),
                  static_cast<unsigned long>(low4),
                  static_cast<unsigned long>((low4 * 100ULL) / kSamples),
                  static_cast<unsigned long>(((low4 * 1000000ULL) / kSamples) % 10000),
                  static_cast<unsigned long>(edges4), static_cast<unsigned long>(maxRun4),
                  static_cast<unsigned long>(low3),
                  static_cast<unsigned long>((low3 * 100ULL) / kSamples),
                  static_cast<unsigned long>(((low3 * 1000000ULL) / kSamples) % 10000),
                  static_cast<unsigned long>(bothLow),
                  static_cast<unsigned long>(drive4crx3High));
    return r->reply(200, "text/plain", out);
  });

  // The ECC report. Protocol: `/api/ecc?clear=1`, one `/api/kick?ms=3000`, then `/api/ecc`.
  // The histogram's dominant bucket names the failing bit-field; the transition ring shows
  // the SEED error of each episode with the error counters at that instant.
  g_server.on("/api/ecc", HTTP_GET, [](PsychicRequest* r) {
    if (r->getParam("clear") && r->getParam("clear")->value() == "1") {
      ecc::g_clear = true;
      logmsg("console: ecc cleared");
      return r->reply(200, "text/plain", "clearing");
    }
    // ?mask=0 hands the bus-error interrupt back to the driver: busErr resumes counting,
    // the histogram goes blind. ?mask=1 restores the instrument. See ecc::g_maskBeie.
    if (r->getParam("mask")) {
      ecc::g_maskBeie = r->getParam("mask")->value() != "0";
      logmsg("console: ecc mask %s", ecc::g_maskBeie ? "ON (histogram)" : "OFF (busErr)");
      return r->reply(200, "text/plain", ecc::g_maskBeie ? "masked" : "unmasked");
    }
    static char out[3400];
    size_t n = 0;
    twai_status_info_t st;
    const bool up = twai_get_status_info(&st) == ESP_OK;
    n += snprintf(out + n, sizeof(out) - n,
                  "driver     %s state %d  toRx %lu  toTx %lu   beie mask %s\n",
                  up ? "ok" : "ABSENT", up ? static_cast<int>(st.state) : -1,
                  up ? static_cast<unsigned long>(st.msgs_to_rx) : 0UL,
                  up ? static_cast<unsigned long>(st.msgs_to_tx) : 0UL,
                  ecc::g_maskBeie ? "ON (busErr frozen)" : "off");
    ecc::RegSnapshot snap;
    if (up && ecc::readTwaiRegs(snap, ecc::g_maskBeie)) {
      n += snprintf(out + n, sizeof(out) - n,
                    "mode       rm %u  lom %u  stm %u  afm %u   rec %u  tec %u\n",
                    static_cast<unsigned>(snap.mode & 1),
                    static_cast<unsigned>((snap.mode >> 1) & 1),
                    static_cast<unsigned>((snap.mode >> 2) & 1),
                    static_cast<unsigned>((snap.mode >> 3) & 1),
                    static_cast<unsigned>(snap.rec), static_cast<unsigned>(snap.tec));
    }
    n += snprintf(out + n, sizeof(out) - n,
                  "polls      %lu ok (%lu in normal)  %lu absent  %lu standoff  %lu empty  "
                  "%lu transitions  %lu remasks\n",
                  static_cast<unsigned long>(ecc::g_samples),
                  static_cast<unsigned long>(ecc::g_inNormal),
                  static_cast<unsigned long>(ecc::g_absent),
                  static_cast<unsigned long>(ecc::g_standoff),
                  static_cast<unsigned long>(ecc::g_empty),
                  static_cast<unsigned long>(ecc::g_changes),
                  static_cast<unsigned long>(ecc::g_remasks));

    n += snprintf(out + n, sizeof(out) - n, "histogram  (top 12, count desc)\n");
    bool printed[4][2][32] = {};
    for (int rank = 0; rank < 12; ++rank) {
      uint32_t best = 0;
      int bc = -1, bd = -1, bs = -1;
      for (int c = 0; c < 4; ++c)
        for (int d = 0; d < 2; ++d)
          for (int s = 0; s < 32; ++s)
            if (!printed[c][d][s] && ecc::g_hist[c][d][s] > best) {
              best = ecc::g_hist[c][d][s]; bc = c; bd = d; bs = s;
            }
      if (bc < 0) break;
      printed[bc][bd][bs] = true;
      n += snprintf(out + n, sizeof(out) - n, "  %-5s %s @ %-16s (raw 0x%02X)  x %lu\n",
                    ecc::errcName(static_cast<uint8_t>(bc)), bd ? "RX" : "TX",
                    ecc::segName(static_cast<uint8_t>(bs)),
                    static_cast<unsigned>((bc << 6) | (bd << 5) | bs),
                    static_cast<unsigned long>(best));
    }

    n += snprintf(out + n, sizeof(out) - n, "ring       (distinct codes, oldest first)\n");
    const uint8_t cnt = sizeof(ecc::g_ring) / sizeof(ecc::g_ring[0]);
    for (uint8_t i = 0; i < cnt; ++i) {
      const ecc::Change& c = ecc::g_ring[(ecc::g_ringHead + i) % cnt];
      if (!c.ms) continue;
      n += snprintf(out + n, sizeof(out) - n,
                    "  t=%-8lu 0x%02X %-5s %s @ %-16s lom %u stm %u  rec %3u tec %3u\n",
                    static_cast<unsigned long>(c.ms), c.raw,
                    ecc::errcName((c.raw >> 6) & 0x03), (c.raw & 0x20) ? "RX" : "TX",
                    ecc::segName(c.raw & 0x1F),
                    static_cast<unsigned>((c.mode >> 1) & 1),
                    static_cast<unsigned>((c.mode >> 2) & 1), c.rec, c.tec);
      if (n > sizeof(out) - 100) break;
    }
    return r->reply(200, "text/plain", out);
  });

  // THE IS-IT-US TEST, and it is the first thing to reach for when the bus looks dead.
  //
  // Shutting the gate stops every frame WE would send, in software. The controller keeps
  // ACKing other nodes in hardware, which a two-node bus requires — so if rx STILL does not
  // climb with the gate shut, we are silent and cannot be the cause, and the fault is on the
  // wire or at the other end. If rx starts climbing the moment we go quiet, we were
  // trampling the bus.
  //
  // Direct TWAI supports listen-only both at begin() and through setListenOnly(). This
  // console switches at runtime so it can compare the same powered session in both modes.
  // THE A/B THAT SETTLES IT, on one boot, with the trace running.
  //
  // Listen-only observes the bus WITHOUT ACKNOWLEDGING IT. Normal acknowledges every frame
  // in hardware whatever the software gate says. If frames pour in with `on=1` and stop dead
  // with `on=0` while we are still transmitting nothing, then our ACKNOWLEDGEMENT is what
  // destroys them — which is a different fault from a silent panel and needs the opposite
  // fix. Nothing else in this console can distinguish those two.
  g_server.on("/api/listen", HTTP_GET, [](PsychicRequest* r) {
    const bool on = !(r->getParam("on") && r->getParam("on")->value() == "0");
    // Shut the gate first and leave it shut: entering listen-only with renders queued would
    // silently drop them, and coming BACK from it into a live sequence is not what this
    // switch is for. pumpGate() re-opens on its own once frames are flowing again.
    g_gateOpen   = false;
    g_reopenAtMs = millis() + kBackoffMs;
    g_link.setTxEnabled(false);

    const bool ok = g_link.setListenOnly(on);
    logmsg("console: listen-only %s -> %s", on ? "ON" : "OFF", ok ? "ok" : "REFUSED");
    return r->reply(200, "text/plain", ok ? "ok" : "refused");
  });

  // The full blind handshake, then straight back to listen-only to read the answer.
  g_server.on("/api/kick", HTTP_GET, [](PsychicRequest* r) {
    const long ms = r->getParam("ms") ? r->getParam("ms")->value().toInt() : 1500;
    g_shoutMs    = static_cast<uint32_t>(ms < 300 ? 300 : (ms > 10000 ? 10000 : ms));
    g_kickWanted = true;
    g_shoutPhase = 1;
    return r->reply(200, "text/plain", "kicking");
  });

  g_server.on("/api/shout", HTTP_GET, [](PsychicRequest* r) {
    const long ms = r->getParam("ms") ? r->getParam("ms")->value().toInt() : 2500;
    g_shoutMs    = static_cast<uint32_t>(ms < 200 ? 200 : (ms > 10000 ? 10000 : ms));
    g_shoutPhase = 1;                    // loop() does the work; see pumpShout()
    return r->reply(200, "text/plain", "shouting");
  });

  g_server.on("/api/txgate", HTTP_GET, [](PsychicRequest* r) {
    const bool on = !(r->getParam("on") && r->getParam("on")->value() == "0");
    // Go THROUGH the FSM's state rather than around it. Setting the link directly would be
    // undone by pumpGate() on its next pass, and a console switch that silently reverts is
    // worse than no switch.
    g_gateOpen = on;
    g_link.setTxEnabled(on);
    if (on) g_gateOpenMs = millis();
    else    g_reopenAtMs = millis() + 3600000UL;   // an hour: "shut, and stay shut"
    logmsg("console: TX gate %s", on ? "OPEN" : "SHUT - we are silent now");
    return r->reply(200, "text/plain", "ok");
  });

  g_server.on("/api/power", HTTP_GET, [](PsychicRequest* r) {
    g_powerWanted = !(r->getParam("on") && r->getParam("on")->value() == "0");
    logmsg("console: power %s", g_powerWanted ? "on" : "off");
    return r->reply(200, "text/plain", "ok");
  });

  g_server.on("/api/speed", HTTP_GET, [](PsychicRequest* r) {
    const uint8_t  row = rowArg(r);
    const uint32_t ms  = r->getParam("ms")
                             ? static_cast<uint32_t>(r->getParam("ms")->value().toInt()) : 0;
    // 0 makes the row STATIC rather than fast: it shows its first `width` characters and
    // never moves, which costs no repaints at all.
    g_rows.setScroll(row, ms);
    logmsg("console: row %u speed %lu ms", row, static_cast<unsigned long>(ms));
    return r->reply(200, "text/plain", "ok");
  });

  g_server.on("/api/dir", HTTP_GET, [](PsychicRequest* r) {
    const uint8_t row = rowArg(r);
    const bool    rev = r->getParam("rev") && r->getParam("rev")->value() == "1";
    g_rows.setDirection(row, rev ? Marquee::Direction::Reverse : Marquee::Direction::Forward,
                        millis());
    logmsg("console: row %u %s", row, rev ? "reverse" : "forward");
    return r->reply(200, "text/plain", "ok");
  });

  g_server.on("/api/text", HTTP_GET, [](PsychicRequest* r) {
    const uint8_t row = rowArg(r);
    const String  t   = r->getParam("t") ? r->getParam("t")->value() : String();
    // Row 2 is generated every iteration, so writing to it would be overwritten immediately.
    // Say so rather than accepting it and appearing to do nothing.
    if (row == 2) return r->reply(200, "text/plain", "row 2 carries the clock; it is generated");
    snprintf(row == 0 ? g_text0 : g_text1, AFFA_TEXT_MAX, "%s", t.c_str());
    logmsg("console: row %u text '%s'", row, t.c_str());
    return r->reply(200, "text/plain", "ok");
  });

  g_server.on("/api/popup", HTTP_GET, [](PsychicRequest* r) {
    g_popupNextMs = millis();          // due now; loop() picks it up on its next pass
    return r->reply(200, "text/plain", "ok");
  });

  g_server.on("/api/reboot", HTTP_GET, [](PsychicRequest* r) {
    g_rebootAt = millis() + 300;       // after the response has actually gone out
    return r->reply(200, "text/plain", "rebooting");
  });
}

void startNetwork() {
  // Read-only open of the namespace this board already holds credentials in. READ-ONLY
  // matters: an NVS write stalls CAN reception outright for the duration of the flash write,
  // because the TWAI ISR is not in IRAM. This example must never be why they change.
  Preferences p;
  String ssid, pass;
  if (p.begin(kWifiNamespace, /*readOnly=*/true)) {
    ssid = p.getString("ssid", "");
    pass = p.getString("pass", "");
    p.end();
  }

  WiFi.persistent(false);
  WiFi.setSleep(true);          // the C3 has one radio and interleaves WiFi with CAN timing

  bool sta = false;
  if (ssid.length()) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());
    const uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < kStaJoinMs) delay(100);
    sta = (WiFi.status() == WL_CONNECTED);
  }
  if (!sta) {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(kApSsid, kApPass);
  }
  if (MDNS.begin(kMdnsName)) MDNS.addService("http", "tcp", 80);

  const String ip = sta ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  Serial.printf("\n[wifi] %s ip=%s  console http://%s/  OTA http://%s/update  mdns %s.local\n",
                sta ? "STA" : "AP (fallback)", ip.c_str(), ip.c_str(), ip.c_str(), kMdnsName);
}

void startHttp() {
  // THE FOUR LINES THAT KEEP THIS BOARD REACHABLE.
  //
  // With lru_purge_enable false — esp_http_server's default, and what PsychicHttp leaves it
  // at unless ENABLE_ASYNC is defined — a full socket table is PERMANENT: httpd stops
  // calling accept() and never resumes. The board still answers ping and still answers mDNS,
  // so it looks alive from every angle except the one that matters, and OTA is gone with it.
  g_server.config.lru_purge_enable  = true;
  g_server.config.max_open_sockets  = 7;
  g_server.config.recv_wait_timeout = 3;
  g_server.config.send_wait_timeout = 3;
  // The URI table is a FIXED ARRAY and registering past its end fails SILENTLY — PsychicHttp
  // does not check the return, so the route is simply absent. This example has nine routes
  // plus ElegantOTA's three; the headroom is deliberate, because the failure mode is a board
  // that needs a cable.
  g_server.config.max_uri_handlers  = 32;
  g_server.config.stack_size        = 8192;

  g_server.listen(80);

  // OTA FIRST, ALWAYS — before any route of ours can crowd its three registrations off the
  // end of that table. It is the only way back into a board with no cable.
  //
  // An OTA write stalls CAN reception outright, so gate our transmitter for the duration:
  // there is no point shouting at a bus we cannot hear. Expect a resync after the reboot.
  ElegantOTA.onStart([]() {
    g_otaRunning = true;
    g_link.setTxEnabled(false);
    logmsg("ota started - CAN TX gated");
  });
  ElegantOTA.onEnd([](bool ok) {
    // CLEAR THE FLAG ON FAILURE. A latched "OTA in progress" that nothing resets leaves every
    // console command silently ignored until somebody reboots, and it took a while to find.
    if (!ok) { g_otaRunning = false; g_link.setTxEnabled(true); }
    logmsg("ota %s", ok ? "ok, rebooting" : "FAILED - TX ungated");
  });
  ElegantOTA.begin(&g_server);

  routes();
}

// Runs on the LIBRARY'S OWN TASK, not on loop(). logmsg() is safe there — it takes a
// spinlock and formats into a fixed buffer — but nothing heavier belongs here: a blocking
// callback shows up directly as Status::pollLateMaxUs and eventually as a missed ACK.
void onSync(affa::SyncState s, void*) {
  logmsg("sync: state 0x%02X%s", static_cast<unsigned>(s),
         affa::hasFlag(s, affa::SyncState::Failed) ? " (FAILED)" : "");
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\nAffaDisplay 04_rows");

  // ===========================================================================
  // CAN FIRST. WIFI SECOND. THIS ORDER IS THE POINT OF THIS BUILD.
  // ===========================================================================
  // Every previous version joined WiFi before touching the bus — and WiFi.begin() blocks for
  // up to kStaJoinMs, FIFTEEN SECONDS. On a shared supply the panel and this board power up
  // together, so those fifteen seconds are spent off the bus entirely.
  //
  // AND THE FIRST SECONDS ARE THE ONLY ONES THAT MATTER. A freshly powered panel is POLITE:
  // the run that worked measured 498 frames over 250 s, about 2/s. A panel that has concluded
  // it has no master hammers its sync request at line rate — 1470/s, 92 % bus occupancy —
  // and that is the state every measurement since has been fighting. The panel does not
  // recover from it on its own; only a power cycle resets it.
  //
  // So the window in which this link can be established is the moment after power-on, and we
  // have been sleeping through it in WiFi.begin(). CAN now comes up within milliseconds of
  // boot, ready to answer the panel's very first request.
  //
  // The cost is that a CAN failure now precedes OTA. That is acceptable because begin() does
  // not block — it installs a driver and returns — whereas WiFi does.
  if (!g_link.begin(kPins, kBitrate))
    Serial.println("[can] controller did not come up");

  // THE ECC PROBE IS OFF BY DEFAULT NOW, AND THAT IS A RESULT, NOT A REGRESSION.
  //
  // It existed to find a fault that is now settled: the bench's chronic transmit failures
  // were `frame.ss = 1` (single-shot) discarding every recoverable collision, not anything
  // the ECC register could see. See docs/CARMINAT-HANDSHAKE-GROUND-TRUTH.md.
  //
  // Left running it is no longer an instrument, it is the fault. The task polls the TWAI
  // peripheral every millisecond at priority 3 — ABOVE the library's poll task at 2, so it
  // preempts the code that drains the RX FIFO — and it clears BEIE underneath a live driver.
  // Measured 2026-08-04: with the probe on, this example went deaf inside four seconds
  // ("2 frames heard"); with it off, the identical library ran the full opening and put text
  // on the glass. Turn it on only to diagnose a NEW fault, and expect it to cost reception
  // while it is on.
  if (ecc::kEnableEccProbe) ecc::start();

  // AND THE GATE STARTS OPEN, which reverses the previous build deliberately.
  //
  // Booting silent was the right answer to "are we poisoning a sulking panel?" — and the
  // answer measured out as no: with the gate shut and zero frames transmitted, reception was
  // exactly as dead. The panel is not sulking at us. It is asking for a master, and the only
  // thing that stops it asking is being ANSWERED. Staying quiet through the one window in
  // which it is still willing to listen is the opposite of what we want.
  g_gateOpen   = true;
  g_gateOpenMs = 0;

  g_rows.setScroll(0, 0);            // static: SUCCESS holds still and stays readable
  g_rows.setScroll(1, kSpeedMedium);
  g_rows.setScroll(2, kSpeedSlow);

  // THE ORDER IS THE CONTRACT: callbacks, then begin(), then start(). start() refuses a
  // display that was never begun, and a callback installed after the task is running would
  // miss whatever it had already delivered.
  g_task.onComplete(&onRenderComplete, nullptr);
  g_display.onSync(&onSync, nullptr);
  g_display.onFrame(&onTap, nullptr);       // Layer 0: everything, both directions
  g_display.begin();

  if (!g_task.start(g_display))
    Serial.println("[task] start() FAILED - nothing will be polled");

  // Network AFTER the bus. OTA is still reached long before anyone can need it.
  startNetwork();
  startHttp();
  logmsg("CAN was up %lu ms before WiFi finished - that is the whole change",
         static_cast<unsigned long>(millis()));

  logmsg("up: rows static/%lu/%lu ms, clock %s, popup every %lus for %lus",
         static_cast<unsigned long>(kSpeedMedium),
         static_cast<unsigned long>(kSpeedSlow), kClockHHMM,
         static_cast<unsigned long>(kPopupEveryMs / 1000),
         static_cast<unsigned long>(kPopupHoldMs / 1000));
}

void loop() {
  // NO poll() HERE. The library polls itself on its own task, so loop() may block — for an
  // OTA write, for a slow HTTP response — without costing a timed-out ACK or a lost render.
  ElegantOTA.loop();

  const uint32_t now = millis();

  if (g_rebootAt && affa::expired(now, g_rebootAt)) ESP.restart();

  // An OTA write stalls the CAN ISR; do not fight it for the bus.
  if (g_otaRunning) { delay(10); return; }

  // BEFORE ANYTHING ELSE. Whether we are allowed to transmit at all outranks every render
  // decision below it, and the deaf-detection has to run even when the sequence is parked.
  pumpShout(now);
  pumpGate(now);

  const affa::rtos::Status st = g_task.status();

  // ---- one place where a render's fate is accounted for -------------------
  // `wasInFlight` is sampled BEFORE pollRender(), which zeroes g_inFlight on completion —
  // it is the only way to still know WHOSE completion this was.
  affa::Result res = affa::Result::Ok;
  const uint32_t wasInFlight = g_inFlight;
  switch (pollRender(res)) {
    case 1:
      if (g_clockReq && wasInFlight == g_clockReq) {
        g_clockOk  = true;
        g_clockReq = 0;
        logmsg("panel clock reads %c%c:%c%c",
               kClockHHMM[0], kClockHHMM[1], kClockHHMM[2], kClockHHMM[3]);
      }
      break;
    case 2:
      ++g_renderFails;
      logmsg("render failed: %s", resultName(res));
      if (g_clockReq && wasInFlight == g_clockReq) {
        g_clockReq     = 0;                       // Live re-issues it after the pause
        g_clockRetryAt = now + kClockRetryMs;
      }
      // LinkDown and Cancelled mean the world under us changed; anything else is a single
      // screen we can simply draw again. Force the redraw either way.
      g_rows.invalidate();
      break;
    case 0:
      if (affa::expired(now, g_flightSince + kRenderTimeoutMs)) {
        ++g_timeouts;
        logmsg("render %lu never completed in %lu ms - abandoning it",
               static_cast<unsigned long>(g_inFlight),
               static_cast<unsigned long>(kRenderTimeoutMs));
        g_inFlight = 0;
        if (g_clockReq && wasInFlight == g_clockReq) {
          g_clockReq     = 0;
          g_clockRetryAt = now + kClockRetryMs;
        }
        g_rows.invalidate();
      }
      break;
    default:
      break;
  }

  // ---- losing sync invalidates everything behind it -----------------------
  // The panel forgets its registration, and it may have been powered down entirely. Start
  // from the top rather than carrying on drawing into a screen that is not there.
  // Losing FUNCSREG counts too, and not only Failed: a panel that re-opened with `61 11`
  // drops our registrations without necessarily parking sync in Failed long enough for a
  // 2 ms poll to see it. Drawing into a session whose functions are gone is the flood above.
  if (g_step != Step::WaitPanel &&
      (affa::hasFlag(st.sync, affa::SyncState::Failed) ||
       !affa::hasFlag(st.sync, affa::SyncState::FuncsReg))) {
    ++g_syncLosses;
    restart("panel stopped answering");
  }

  switch (g_step) {
    case Step::WaitPanel:
      if (affa::hasFlag(st.sync, affa::SyncState::Failed)) break;
      // AND WAIT FOR REGISTRATION, WHICH IS NO LONGER LAZY. It used to happen on the first
      // render, so waiting here would have waited for ever — that is why this used not to.
      // The captured opening emits `151 70` / `1F1 70` with the final B0 frame
      // (carminat::kSync.registerAfterHello), so FUNCSREG latches by itself a few
      // milliseconds after sync clears, with no render required.
      //
      // Painting before it latches is not merely early, it is ACTIVELY HARMFUL: each
      // fullscreen is fourteen frames, they queue faster than an unregistered panel will
      // take them, and the flood costs the handshake the arbitration it needs to finish.
      // Measured on the bench 2026-08-04 without this gate: arbLost 5577, drop 2117,
      // txErr pinned 128 (BUS-OFF), syncLost 29, and `registered 0` for ever.
      if (!affa::hasFlag(st.sync, affa::SyncState::FuncsReg)) break;
      logmsg("panel registered - powering the display on");
      g_step = Step::PowerOn;
      break;

    case Step::PowerOn:
      if (g_inFlight) break;
      if (!issue(g_task.setPower(true), now, "setPower")) break;
      g_powerIsOn = true;
      // NOT `now + kWarmUpMs`. The glass does not begin lighting when we ENQUEUE the power
      // command, it begins when the panel ACKNOWLEDGES it — and on a busy bus that ACK can
      // take longer than the warm-up itself, in which case a deadline armed here has already
      // expired by the time it lands and buys exactly nothing. 0 means "not armed yet".
      g_deadline  = 0;
      g_step      = Step::WarmUp;
      break;

    case Step::WarmUp:
      // TWO WAITS, IN SERIES, and the order is the whole point. First the power render must
      // be acknowledged; only THEN does the settling clock start. Running them concurrently
      // is what makes the first screen vanish and produces "setText does not work".
      if (g_inFlight) break;                       // still waiting for the ACK
      if (g_deadline == 0) {                       // it just landed — start the clock now
        g_deadline = now + kWarmUpMs;
        if (g_deadline == 0) g_deadline = 1;       // millis() wrap: 0 is the "unarmed" value
        logmsg("power acknowledged - warming the glass %lu ms",
               static_cast<unsigned long>(kWarmUpMs));
        break;
      }
      if (!affa::expired(now, g_deadline)) break;
      logmsg("glass warm - going live");
      g_popupNextMs  = now + kPopupEveryMs;
      g_nextSoakMs   = now + kSoakReportMs;
      g_clockRetryAt = now;      // clock due immediately; wrap-proof on the boot path too
      g_step         = Step::Live;
      break;

    case Step::Live:
      // The console's power switch is honoured here rather than in the handler, so it goes
      // through the same one-render-in-flight discipline as everything else.
      if (g_powerWanted != g_powerIsOn) {
        if (g_inFlight) break;
        if (!issue(g_task.setPower(g_powerWanted), now, "setPower")) break;
        g_powerIsOn = g_powerWanted;
        logmsg("display %s", g_powerIsOn ? "on" : "off");
        if (g_powerIsOn) g_rows.invalidate();   // it comes back blank
        break;
      }
      if (!g_powerIsOn) break;                  // nothing to draw on a dark panel
      // The clock goes out FIRST — before the rows ever take the render slot — and keeps
      // being re-issued (paced by kClockRetryMs) until one attempt completes Ok. The
      // completion accounting above is what latches g_clockOk and what schedules the
      // retry after a failure or an abandonment.
      if (!g_clockOk && !g_clockReq && !g_inFlight && affa::expired(now, g_clockRetryAt)) {
        if (issue(g_task.setTime(kClockHHMM), now, "setTime")) {
          g_clockReq     = g_inFlight;
          g_clockRetryAt = now + kClockRetryMs;
        }
        break;
      }
      pumpScreen(now);
      break;
  }

  soakReport(now);

  // loopTask runs at priority 1 and IDLE at 0, so a loop() that never yields starves IDLE and
  // the single-core C3 panics with "Task watchdog got triggered (IDLE)".
  delay(10);
}
