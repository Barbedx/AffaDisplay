// 05_pingpong — the Carminat handshake BY HAND. No AffaDisplay. One goal: SUCCESS on glass.
//
// THIS EXAMPLE DELIBERATELY DOES NOT LINK THE LIBRARY (build_src_filter = -<*>), for the
// same reason 02_canspy does not: when the screen stays blank, the question "is it the
// protocol layer?" needs an implementation with no protocol layer in it. What runs here is
// the Arduino core, esp32_can, and this file — every byte this firmware puts on the bus is
// visible in this file, built directly from docs/PROTOCOL.md, section by section.
//
// WHAT IT DOES, in order, entirely by itself:
//
//   1. Boots ARMED: CAN up in NORMAL mode within milliseconds of power-on, WiFi later.
//   2. Heartbeats B9 once a second from the first tick, plus BA while unsynced.  (§3.2)
//   3. Answers ANY 69 ping with a paced B9 pong — NO authorization gate: the proven
//      driver replies from any state, synced or not, registered or not.         (§3.6)
//   4. Answers 61 11 with the three-frame hello (paced against the storm).      (§3.4, §8)
//   5. Registers funcs {0x151, 0x1F1}: 70 -> wait 74 on id|0x400, sequential.   (§3.5)
//   6. Display ON (03 52 09 FF FF), waits for its ACK.                          (§5.1)
//   7. Waits one second for the glass to warm.
//   8. setText 0x77 "SUCCESS" — the capture-proven 3-frame message.             (§5.2)
//   9. Re-auths without being asked: 61 11 01 voids registration and re-runs it
//      (§3.3); peer-alive expiry (5 s without a ping) drops everything and the
//      sequence restarts when the panel returns; three consecutive transfer
//      failures assume the panel silently forgot us and re-register.
//  10. ACKs every application frame the panel sends us (74 on id|0x400), which is
//      what lets the panel register ITS channel 0x1C1 and send keys.            (§3.5, §4.2)
//
// WiFi + ElegantOTA stay up exactly as on every image this board runs — OTA registered
// FIRST (the URI table trap), lru_purge enabled (the socket-table trap) — so the board
// remains flashable with no cable.
//
//   pio run -e ex05_pingpong               build
//   pio run -e ex05_pingpong -t upload     first flash over USB; thereafter /update
//
// Console: http://<ip>/  — status, the coalesced frame trace, the log, set the text.

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <PsychicHttp.h>
#include <ElegantOTA.h>

#include <esp32_can.h>
#include <driver/twai.h>

// periph_module_reset() moved headers between IDF 4 and 5 — same dance as the library.
#if __has_include(<esp_private/periph_ctrl.h>)
#  include <esp_private/periph_ctrl.h>
#elif __has_include(<driver/periph_ctrl.h>)
#  include <driver/periph_ctrl.h>
#endif

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

constexpr const char* kVersion = "05_pingpong 0.2.0";

// ---------------------------------------------------------------------------
// Boot mode — ARMED (normal, talking) or LISTEN (pure sniffer, direct TWAI)
// ---------------------------------------------------------------------------
// Listen-only is decided AT BOOT, from NVS, and is implemented with the raw TWAI driver —
// no esp32_can at all in that mode. Deliberately: a RUNTIME switch to listen-only is
// disable()+enable(), and disable() joins a receive task that is blocked in twai_receive()
// — on a bus where nothing decodes (exactly when you want listen-only) that join never
// returns, loop() wedges, and the board needs a power cycle. It ate two flashes on this
// rig already. A reboot into a clean listen-only install has no teardown to hang.
constexpr const char* kOwnNamespace = "pingpong";    // ours: the boot mode, and only that
bool g_listenBoot = false;

// ---------------------------------------------------------------------------
// Board — ESP32-C3 SuperMini on the bench rig. RX FIRST.
// ---------------------------------------------------------------------------
constexpr gpio_num_t kRxPin   = GPIO_NUM_3;
constexpr gpio_num_t kTxPin   = GPIO_NUM_4;
constexpr uint32_t   kBitrate = 500000;

// 250 ms, MEASURED on this rig. 0 parks a bus-off in TWAI_STATE_STOPPED for ever;
// 2000 accumulates inside setup().
constexpr uint32_t kForceRecoveryMs = 250;

// ---------------------------------------------------------------------------
// Protocol constants — every one of these is a line in docs/PROTOCOL.md
// ---------------------------------------------------------------------------
constexpr uint16_t kSyncTx    = 0x3AF;   // us -> panel                         §2.3
constexpr uint16_t kSyncRx    = 0x3CF;   // panel -> us (NOT 3AF|0x400!)        §2.3
constexpr uint16_t kReplyFlag = 0x400;   // reply id = funcId | 0x400           §2.3
constexpr uint16_t kFuncs[]   = { 0x151, 0x1F1 };   // registration order is on the wire §3.5
constexpr uint8_t  kFuncCount = sizeof(kFuncs) / sizeof(kFuncs[0]);

// Session flags — §3.1. Initial state is FAILED.
constexpr uint8_t FAILED     = 0x01;
constexpr uint8_t PEER_ALIVE = 0x02;
constexpr uint8_t START      = 0x04;
constexpr uint8_t FUNCSREG   = 0x08;

// Timing — §8, plus the two pacing rules that keep us out of the hello storm.
constexpr uint32_t kTickMs      = 1000;   // B9 heartbeat cadence
constexpr uint32_t kAckMs       = 2000;   // per-frame ACK timeout, no retry
constexpr uint32_t kPeerAliveMs = 5000;   // 5 ticks without a ping -> FAILED (assignment)
constexpr uint32_t kHelloMinMs  = 250;    // an unACKed panel asks at line rate; reply paced
constexpr uint32_t kPongMinMs   = 250;    // 126 pings in 32 ms is one ping, not 126
constexpr uint32_t kWarmUpMs    = 1000;   // "turn on display, wait a sec"
constexpr uint32_t kRetryMs     = 1000;   // pause between registration passes
constexpr uint32_t kXferRetryMs = 500;    // pause before re-sending a failed message

// Three consecutive delivery failures = the panel has forgotten us even though nobody said
// so. Registration is the factory recovery (§3.5: re-register, re-power, redraw).
constexpr uint8_t kFailsBeforeReauth = 3;

// millis()-wrap-proof deadline test.
bool expired(uint32_t now, uint32_t at) { return static_cast<int32_t>(now - at) >= 0; }

// ---------------------------------------------------------------------------
// Log ring — this board is watched over HTTP, not over a cable
// ---------------------------------------------------------------------------
struct LogRec { uint32_t ms = 0; char msg[112] = {0}; };
constexpr size_t kLogRing = 64;
LogRec       g_log[kLogRing];
size_t       g_logHead = 0;
portMUX_TYPE g_logMux  = portMUX_INITIALIZER_UNLOCKED;

void logmsg(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void logmsg(const char* fmt, ...) {
  char buf[112];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  portENTER_CRITICAL(&g_logMux);
  LogRec& r = g_log[g_logHead];
  r.ms = ::millis();
  snprintf(r.msg, sizeof(r.msg), "%s", buf);
  g_logHead = (g_logHead + 1) % kLogRing;
  portEXIT_CRITICAL(&g_logMux);

  if (Serial) Serial.printf("[%lu] %s\n", static_cast<unsigned long>(r.ms), buf);
}

// ---------------------------------------------------------------------------
// The coalesced frame trace — a raw ring is useless against a storming panel
// ---------------------------------------------------------------------------
// The panel repeats one identical frame back to back at line rate, so identical consecutive
// frames collapse into one row with a repeat count. Our own frames stay visible next to it.
struct FrameRec {
  uint32_t firstMs = 0, lastMs = 0, count = 0;
  uint16_t id = 0;
  uint8_t  dir = 0;   // 0 = RX, 1 = TX (ours)
  uint8_t  len = 0;
  uint8_t  d[8] = {0};
};
constexpr uint8_t kFrameRing = 48;
FrameRec     g_frames[kFrameRing];
uint8_t      g_frameHead = 0;
portMUX_TYPE g_frameMux  = portMUX_INITIALIZER_UNLOCKED;

void tap(uint16_t id, const uint8_t* d, uint8_t len, bool tx) {
  const uint32_t ms = ::millis();
  portENTER_CRITICAL(&g_frameMux);
  FrameRec& last = g_frames[(g_frameHead + kFrameRing - 1) % kFrameRing];
  const bool same = last.count && last.dir == (tx ? 1 : 0) && last.id == id &&
                    last.len == len && memcmp(last.d, d, 8) == 0;
  if (same) {
    ++last.count;
    last.lastMs = ms;
  } else {
    FrameRec& r = g_frames[g_frameHead];
    r.firstMs = ms; r.lastMs = ms; r.count = 1;
    r.id = id; r.dir = tx ? 1 : 0; r.len = len;
    memcpy(r.d, d, 8);
    g_frameHead = static_cast<uint8_t>((g_frameHead + 1) % kFrameRing);
  }
  portEXIT_CRITICAL(&g_frameMux);
}

// ---------------------------------------------------------------------------
// RX FIFO — the CAN callback runs in task_CAN (prio 15); the protocol runs in loop()
// ---------------------------------------------------------------------------
// Copy and return, nothing else. loop() drains the whole FIFO every pass (5 ms), which is
// an order of magnitude faster than the storm can fill it.
struct RxF { uint16_t id; uint8_t len; uint8_t d[8]; };
constexpr size_t kFifo = 64;
RxF          g_fifo[kFifo];
volatile size_t g_fifoW = 0, g_fifoR = 0;
uint32_t     g_fifoDrop = 0;
portMUX_TYPE g_fifoMux  = portMUX_INITIALIZER_UNLOCKED;

uint32_t g_rxCount = 0, g_txCount = 0, g_txRefused = 0;
uint32_t g_lastRxMs = 0;

void onCanFrame(CAN_FRAME* f) {
  if (!f) return;
  // 11-bit data frames only: the protocol has no extended ids and no RTR (§1).
  if (f->extended || f->rtr) return;
  const uint8_t len = f->length > 8 ? 8 : f->length;

  tap(static_cast<uint16_t>(f->id), f->data.uint8, len, /*tx=*/false);

  portENTER_CRITICAL(&g_fifoMux);
  ++g_rxCount;
  g_lastRxMs = ::millis();
  const size_t next = (g_fifoW + 1) % kFifo;
  if (next == g_fifoR) {
    ++g_fifoDrop;
  } else {
    RxF& r = g_fifo[g_fifoW];
    r.id  = static_cast<uint16_t>(f->id);
    r.len = len;
    memset(r.d, 0, 8);
    memcpy(r.d, f->data.uint8, len);
    g_fifoW = next;
  }
  portEXIT_CRITICAL(&g_fifoMux);
}

bool fifoPop(RxF& out) {
  bool got = false;
  portENTER_CRITICAL(&g_fifoMux);
  if (g_fifoR != g_fifoW) {
    out = g_fifo[g_fifoR];
    g_fifoR = (g_fifoR + 1) % kFifo;
    got = true;
  }
  portEXIT_CRITICAL(&g_fifoMux);
  return got;
}

// ---------------------------------------------------------------------------
// TX — DLC always 8, short payloads padded with OUR filler 0x00 (§1, §1.1)
// ---------------------------------------------------------------------------
bool g_txGate = true;      // shut during OTA: an OTA write stalls the CAN ISR anyway

bool sendRaw(uint16_t id, const uint8_t* d, uint8_t n) {
  if (!g_txGate) { ++g_txRefused; return false; }
  CAN_FRAME f;
  f.id       = id;
  f.extended = false;
  f.rtr      = 0;
  f.length   = 8;                      // DLC always 8 (§1)
  memset(f.data.uint8, 0x00, 8);       // 0x00 is the radio's filler signature (§1.1)
  memcpy(f.data.uint8, d, n > 8 ? 8 : n);
  const bool ok = CAN0.sendFrame(f);
  if (ok) {
    ++g_txCount;
    tap(id, f.data.uint8, 8, /*tx=*/true);
  } else {
    ++g_txRefused;
  }
  return ok;
}

bool send1(uint16_t id, uint8_t b0) { return sendRaw(id, &b0, 1); }

// ---------------------------------------------------------------------------
// Session state
// ---------------------------------------------------------------------------
uint8_t  g_sync = FAILED;              // §3.1: initial state is FAILED
uint32_t g_lastPingMs  = 0;            // watchdog re-armed by ANY 69 (§3.6)
uint32_t g_lastPongMs  = 0;
uint32_t g_lastHelloMs = 0;
uint32_t g_nextTickMs  = 0;

// Counters the status page reports. Aligned 32-bit reads do not tear on this core.
uint32_t g_pings = 0, g_pongs = 0, g_syncReqs = 0, g_hellos = 0, g_regVoids = 0;
uint32_t g_autoAcks = 0, g_keys = 0, g_replyStray = 0;
uint32_t g_regOk = 0, g_regFail = 0, g_xferOk = 0, g_xferFail = 0, g_syncLosses = 0;

// ---------------------------------------------------------------------------
// The reply wait — stop-and-wait, one frame in flight, ever (§4.2)
// ---------------------------------------------------------------------------
struct Wait {
  bool     active = false;
  uint16_t replyId = 0;
  uint32_t since = 0;
  volatile bool    got = false;
  volatile uint8_t code = 0;           // d[0] of the reply
};
Wait g_wait;

void armWait(uint16_t funcId, uint32_t now) {
  g_wait.active  = true;
  g_wait.replyId = funcId | kReplyFlag;
  g_wait.since   = now;
  g_wait.got     = false;
  g_wait.code    = 0;
}

// ---------------------------------------------------------------------------
// The transfer engine — the not-ISO-TP of §4, sender side
// ---------------------------------------------------------------------------
// frame 0: 8 raw payload bytes. frame N: 0x20+N then 7 bytes. After EVERY frame the panel
// answers on funcId|0x400: 74 = stop-with-success, 30 = send the next one, anything else =
// abort, silence for 2 s = abort. PARTIAL on the last frame is SUCCESS (§4.2).
struct Xfer {
  uint8_t  p[32];
  uint8_t  len = 0, pos = 0, cont = 0;
  uint16_t funcId = 0;
  bool     active = false;
};
Xfer g_xfer;

bool xferStart(uint16_t funcId, const uint8_t* payload, uint8_t len, uint32_t now) {
  g_xfer.funcId = funcId;
  g_xfer.len    = len > sizeof(g_xfer.p) ? sizeof(g_xfer.p) : len;
  memcpy(g_xfer.p, payload, g_xfer.len);
  g_xfer.pos  = 0;
  g_xfer.cont = 0;
  if (!sendRaw(funcId, g_xfer.p, g_xfer.len < 8 ? g_xfer.len : 8)) return false;
  g_xfer.pos    = 8;
  g_xfer.active = true;
  armWait(funcId, now);
  return true;
}

// -1 failed, 0 in progress, +1 delivered.
int8_t xferOnReply(uint8_t code, uint32_t now) {
  const bool last = g_xfer.pos >= g_xfer.len;
  if (code == 0x74) return 1;                      // DONE means STOP, and stop is success
  if (code == 0x30) {
    if (last) return 1;                            // PARTIAL on the last frame = delivered
    uint8_t f[8];
    f[0] = static_cast<uint8_t>(0x20 + (++g_xfer.cont));   // 0x21, 0x22, ... no wrap (§4.1)
    for (uint8_t i = 0; i < 7; ++i)
      f[1 + i] = (g_xfer.pos + i < g_xfer.len) ? g_xfer.p[g_xfer.pos + i] : 0x00;
    g_xfer.pos = static_cast<uint8_t>(g_xfer.pos + 7);
    if (!sendRaw(g_xfer.funcId, f, 8)) return -1;
    armWait(g_xfer.funcId, now);
    return 0;
  }
  return -1;                                       // anything else is an error (§4.2)
}

// ---------------------------------------------------------------------------
// The goal sequence
// ---------------------------------------------------------------------------
enum class Goal : uint8_t { WaitSync, Register, PowerOn, WarmUp, ShowText, Done };

const char* goalName(Goal g) {
  switch (g) {
    case Goal::WaitSync: return "wait-sync   heartbeating; panel has not asked 61 11 yet";
    case Goal::Register: return "register    70 -> 74 on each func id, sequential";
    case Goal::PowerOn:  return "power-on    03 52 09 sent, waiting for its ACK";
    case Goal::WarmUp:   return "warm-up     display ON acked; letting the glass light";
    case Goal::ShowText: return "show-text   0x77 message in flight";
    case Goal::Done:     return "DONE        text delivered and acknowledged";
  }
  return "?";
}

Goal     g_goal = Goal::WaitSync;
uint8_t  g_regIdx = 0;
uint32_t g_retryAt = 0;      // shared pacing for register/xfer retries
uint32_t g_warmAt  = 0;
uint8_t  g_failRun = 0;      // consecutive delivery failures -> re-auth
uint32_t g_doneAtMs = 0;

char g_text[15] = "SUCCESS";           // 14 cells max; the glass shows the first 8 (§5.2)

void goalReset(Goal to, const char* why) {
  g_goal    = to;
  g_regIdx  = 0;
  g_failRun = 0;
  g_retryAt = ::millis();
  g_wait.active = false;
  g_xfer.active = false;
  logmsg("goal -> %.11s: %s", goalName(to), why);
}

// setText 0x77, byte for byte from §5.2. The declared 0x0E is WRONG ON PURPOSE — the panel
// depends on it; 22 payload bytes = 3 frames. 0x00 would terminate the text (§6.3), so the
// unused cells carry 0x20.
uint8_t buildText(uint8_t* p, const char* s) {
  p[0] = 0x10; p[1] = 0x0E; p[2] = 0x77;
  p[3] = 0x55;               // icon: none
  p[4] = 0x55;               // second icon bank, fixed, meaning unknown
  p[5] = 0xFF;               // srcIcon: none
  p[6] = 0x60;               // fmt: plain ASCII, no channel glyph (§6.2)
  p[7] = 0x01;               // control byte, always 01
  memset(p + 8, 0x20, 14);
  for (uint8_t i = 0; i < 14 && s[i]; ++i)
    p[8 + i] = (s[i] >= 0x20 && s[i] < 0x7F) ? static_cast<uint8_t>(s[i]) : '?';
  return 22;
}

// Display ON — single-frame command, still stop-and-wait (§4.2 applies to single frames).
constexpr uint8_t kDisplayOn[8] = {0x03, 0x52, 0x09, 0xFF, 0xFF, 0x00, 0x00, 0x00};

// ---------------------------------------------------------------------------
// RX handling — one function per §-numbered rule
// ---------------------------------------------------------------------------
void handleSyncRx(const RxF& f, uint32_t now) {
  // Sync request: data[0]==0x61 && data[1]==0x11, bytes 3..7 are filler (§3.3).
  if (f.d[0] == 0x61 && f.d[1] == 0x11) {
    ++g_syncReqs;

    // data[2]==0x01 — REGISTRATION IS NOT VALID, start over. Read only when len>=3.
    // The correct reaction (the one no legacy implementation performs): clear FUNCSREG
    // along with latching START, so the next pass re-registers from index 0 (§3.3).
    if (f.len >= 3 && f.d[2] == 0x01) {
      if (g_sync & FUNCSREG) {
        ++g_regVoids;
        g_sync = static_cast<uint8_t>((g_sync & ~FUNCSREG) | START);
        logmsg("panel says registration VOID (61 11 01) - re-registering from the top");
        goalReset(Goal::Register, "registration voided by panel");
      } else {
        g_sync |= START;
      }
    }

    // The hello — unconditional answer to an unconditional request, but PACED: an
    // unacknowledged panel asks at line rate and a 3-frame hello per request is the storm
    // that fills our own queue for ever (§8).
    if (now - g_lastHelloMs >= kHelloMinMs || g_lastHelloMs == 0) {
      g_lastHelloMs = now;
      ++g_hellos;
      const uint8_t h1[8] = {0x70, 0x1A, 0x11, 0x00, 0x00, 0x00, 0x00, 0x01};
      const uint8_t h2[8] = {0xB0, 0x14, 0x11, 0x00, 0x1F, 0x00, 0x00, 0x00};
      sendRaw(kSyncTx, h1, 8);
      sendRaw(kSyncTx, h2, 8);
      sendRaw(kSyncTx, h2, 8);         // IDENTICAL, twice, [CAP] — not a typo (§3.4)
    }

    // Sync is declared up without confirmation — every implementation does this (§3.4).
    const bool wasFailed = (g_sync & FAILED) != 0;
    g_sync = static_cast<uint8_t>(g_sync & ~FAILED);
    if (wasFailed) logmsg("61 11 received - hello sent, sync declared up");

    // A panel that asks to sync AFTER we finished has been reborn under us: it needs the
    // display re-powered and the text redrawn. Registration is kept unless the panel voids
    // it — if it is silently gone, the write fails and the fail-run re-auth catches it.
    if (g_goal == Goal::Done) goalReset(Goal::PowerOn, "panel re-synced after DONE");
    return;
  }

  // Peer-alive ping: data[0]==0x69 ONLY, rest never examined, DLC may be 1 (§3.3).
  if (f.d[0] == 0x69) {
    ++g_pings;
    g_lastPingMs = now ? now : 1;
    g_sync |= PEER_ALIVE;

    // THE PONG — no authorization gate, by design: the proven driver ticks from inside its
    // 69 handler in every state (§3.6). Paced, because 126 pings in 32 ms is one ping.
    if (now - g_lastPongMs >= kPongMinMs || g_lastPongMs == 0) {
      g_lastPongMs = now;
      ++g_pongs;
      send1(kSyncTx, 0xB9);
    }
    return;
  }
  // Other opcodes on the sync id exist (61 23 from clusters) and are deliberately ignored.
}

void handleReply(const RxF& f) {
  if (g_wait.active && f.id == g_wait.replyId && !g_wait.got) {
    g_wait.code = f.d[0];
    g_wait.got  = true;
    return;
  }
  // The reply-flag test is a bit test: every id in 0x400..0x7FF lands here and is consumed
  // silently (§2.3). Counted so a flood of strays is at least visible.
  ++g_replyStray;
}

void handleAppFrame(const RxF& f, uint32_t now) {
  (void)now;
  // Every frame is individually acknowledged at the application layer (§1, §4.2). This is
  // what lets the panel register its 0x1C1 and what answers its keys: 74, our filler.
  // Never on the sync id (handled above), never on a reply id (trap #7 needs a sender test
  // on a self-receiving link — TWAI in NORMAL does not self-receive, and the id split here
  // keeps our own channels out of the ACK path anyway).
  send1(f.id | kReplyFlag, 0x74);
  ++g_autoAcks;

  if (f.d[0] == 0x70) {
    logmsg("panel registered its channel 0x%03X - acked 74 on 0x%03X",
           f.id, f.id | kReplyFlag);
    return;
  }
  // Keys: 03 89 mandatory, else it is not a key (§7).
  if (f.d[0] == 0x03 && f.d[1] == 0x89) {
    const uint16_t raw = static_cast<uint16_t>((f.d[2] << 8) | f.d[3]);
    uint16_t key = raw;
    bool hold = false;
    if (raw != 0x0101 && raw != 0x0141) {          // encoder detents are EXEMPT (§7)
      hold = (f.d[3] & 0xC0) != 0;
      key  = raw & 0xFF3F;
    }
    ++g_keys;
    logmsg("key 0x%04X%s on 0x%03X", key, hold ? " (hold)" : "", f.id);
  }
}

void pumpRx(uint32_t now) {
  RxF f;
  while (fifoPop(f)) {
    if (f.id == kSyncRx)            { handleSyncRx(f, now); continue; }
    if (f.id & kReplyFlag)          { handleReply(f);       continue; }
    handleAppFrame(f, now);
  }
}

// ---------------------------------------------------------------------------
// The 1 Hz tick and the watchdog (§3.2, §3.6)
// ---------------------------------------------------------------------------
void pumpTick(uint32_t now) {
  if (!expired(now, g_nextTickMs)) return;
  g_nextTickMs = now + kTickMs;

  // B9 unconditionally — this doubles as the pong pacing floor, so a healthy link carries
  // two heartbeats a second: one paced here, one per ping (§3.6).
  send1(kSyncTx, 0xB9);
  g_lastPongMs = now;

  // BA while FAILED or START, then clear START (§3.2).
  if (g_sync & (FAILED | START)) {
    send1(kSyncTx, 0xBA);
    g_sync = static_cast<uint8_t>(g_sync & ~START);
  }

  // Watchdog: 5 s without a 69 -> state is ASSIGNED FAILED, which drops PEER_ALIVE, START
  // and FUNCSREG together (§3.6, trap #9 — the assignment is the specified behaviour).
  if (g_lastPingMs && !(g_sync & FAILED) && now - g_lastPingMs > kPeerAliveMs) {
    g_sync = FAILED;
    ++g_syncLosses;
    logmsg("peer-alive expired (%lu ms without a ping) - state = FAILED, all flags dropped",
           static_cast<unsigned long>(now - g_lastPingMs));
    goalReset(Goal::WaitSync, "peer-alive watchdog expired");
  }
}

// ---------------------------------------------------------------------------
// The goal FSM
// ---------------------------------------------------------------------------
void xferFailed(const char* what, uint8_t code, uint32_t now) {
  ++g_xferFail;
  ++g_failRun;
  g_wait.active = false;
  g_xfer.active = false;
  logmsg("%s failed (%s 0x%02X), consecutive failures %u",
         what, code ? "reply" : "timeout", code, g_failRun);
  if (g_failRun >= kFailsBeforeReauth) {
    // The factory recovery for a broken session: re-register, re-power, redraw (§3.5).
    g_sync = static_cast<uint8_t>(g_sync & ~FUNCSREG);
    goalReset(Goal::Register, "too many failures - assuming registration lost");
  } else {
    g_retryAt = now + kXferRetryMs;
  }
}

void pumpGoal(uint32_t now) {
  // A reply wait in progress outranks everything: stop-and-wait means ONE frame in flight.
  if (g_wait.active) {
    if (g_wait.got) {
      const uint8_t code = g_wait.code;
      g_wait.active = false;

      if (g_goal == Goal::Register) {
        if (code == 0x74) {
          logmsg("func 0x%03X registered (74 on 0x%03X)",
                 kFuncs[g_regIdx], kFuncs[g_regIdx] | kReplyFlag);
          if (++g_regIdx >= kFuncCount) {
            g_sync |= FUNCSREG;
            ++g_regOk;
            g_failRun = 0;
            logmsg("FUNCSREG latched - every function id acknowledged");
            goalReset(Goal::PowerOn, "registration complete");
          }
          // else: next probe goes out on the next pass, strictly sequential (§3.5)
        } else {
          ++g_regFail;
          g_regIdx  = 0;                 // any failure aborts the whole pass (§3.5)
          g_retryAt = now + kRetryMs;
          logmsg("registration rejected (0x%02X) - retrying the list from index 0", code);
        }
      } else if (g_xfer.active) {
        const int8_t v = xferOnReply(code, now);
        if (v > 0) {
          g_xfer.active = false;
          ++g_xferOk;
          g_failRun = 0;
          if (g_goal == Goal::PowerOn) {
            g_warmAt = now + kWarmUpMs;
            logmsg("display ON acknowledged - warming the glass %lu ms",
                   static_cast<unsigned long>(kWarmUpMs));
            g_goal = Goal::WarmUp;
          } else if (g_goal == Goal::ShowText) {
            g_goal = Goal::Done;
            g_doneAtMs = now ? now : 1;
            logmsg(">>> '%s' DELIVERED - the panel acknowledged the text <<<", g_text);
          }
        } else if (v < 0) {
          xferFailed(g_goal == Goal::PowerOn ? "display-on" : "setText", code, now);
        }
        // v == 0: next continuation is on the wire, wait re-armed
      }
      return;
    }
    if (expired(now, g_wait.since + kAckMs)) {
      if (g_goal == Goal::Register) {
        ++g_regFail;
        g_wait.active = false;
        const uint16_t func = kFuncs[g_regIdx];
        g_regIdx  = 0;
        g_retryAt = now + kRetryMs;
        logmsg("no 74 within %lu ms for func 0x%03X - registration pass aborted",
               static_cast<unsigned long>(kAckMs), func);
      } else {
        xferFailed(g_goal == Goal::PowerOn ? "display-on" : "setText", 0, now);
      }
    }
    return;
  }

  switch (g_goal) {
    case Goal::WaitSync:
      if (g_sync & FAILED) break;
      goalReset((g_sync & FUNCSREG) ? Goal::PowerOn : Goal::Register, "sync is up");
      break;

    case Goal::Register:
      if (g_sync & FAILED) break;                  // watchdog fired mid-pass
      if (g_sync & FUNCSREG) { goalReset(Goal::PowerOn, "already registered"); break; }
      if (!expired(now, g_retryAt)) break;
      // One probe, one ACK, then the next — registration cannot be performed blind (§3.5).
      if (send1(kFuncs[g_regIdx], 0x70)) armWait(kFuncs[g_regIdx], now);
      break;

    case Goal::PowerOn:
      if (g_sync & FAILED) break;
      if (!expired(now, g_retryAt)) break;
      if (!xferStart(kFuncs[0], kDisplayOn, 8, now)) g_retryAt = now + kXferRetryMs;
      break;

    case Goal::WarmUp:
      if (!expired(now, g_warmAt)) break;
      logmsg("glass warm - sending the text");
      g_goal    = Goal::ShowText;
      g_retryAt = now;
      break;

    case Goal::ShowText: {
      if (g_sync & FAILED) break;
      if (!expired(now, g_retryAt)) break;
      uint8_t p[32];
      const uint8_t n = buildText(p, g_text);
      if (!xferStart(kFuncs[0], p, n, now)) g_retryAt = now + kXferRetryMs;
      break;
    }

    case Goal::Done:
      break;                                       // heartbeats continue; re-auth is event-driven
  }
}

}  // namespace

// ===========================================================================
// THE DEPLOYMENT SCAFFOLDING — WiFi, console, OTA. Same contract as every image here:
// the board must stay reachable, or the next fix costs a cable.
// ===========================================================================

namespace {

constexpr const char* kWifiNamespace = "megaopen";   // read-only: ssid / pass
constexpr const char* kApSsid        = "AffaPong";
constexpr const char* kApPass        = "affapong1";
constexpr const char* kMdnsName      = "affapong";
constexpr uint32_t    kStaJoinMs     = 15000;

PsychicHttpServer g_server;
bool     g_otaRunning = false;
uint32_t g_rebootAt   = 0;

const char kPage[] PROGMEM = R"HTML(<!doctype html><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>AffaDisplay 05_pingpong</title>
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
input{background:#000;color:#ddd;border:1px solid #444;border-radius:4px;padding:4px;width:50%}
</style>
<h1>AffaDisplay <span>05_pingpong</span> &middot; <a href=/update style="color:#8ab">OTA</a></h1>
<fieldset><legend>control</legend>
<input id=t placeholder="text (14 max, 8 visible)">
<button onclick="go('/api/text?t='+encodeURIComponent(t.value))">set text</button>
<button onclick=go('/api/restart')>redo sequence</button>
<button onclick=go('/api/reboot')>reboot board</button>
</fieldset>
<fieldset><legend>wire tests</legend>
<button onclick=go('/api/txgate?on=0')>TX gate SHUT</button>
<button onclick=go('/api/txgate?on=1')>TX gate open</button>
<button onclick=go('/api/mode?listen=1')>reboot into LISTEN-ONLY sniffer</button>
<button onclick=go('/api/mode?listen=0')>reboot ARMED</button>
</fieldset>
<fieldset><legend>status</legend><pre id=s>...</pre></fieldset>
<fieldset><legend>wire (identical frames coalesced)</legend><pre id=f>...</pre></fieldset>
<fieldset><legend>log</legend><pre id=l>...</pre></fieldset>
<script>
async function go(u){ try{ await fetch(u) }catch(e){} }
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

const char* drvStateName(bool valid, uint8_t s) {
  if (!valid) return "reinstalling";
  switch (s) {
    case 0: return "STOPPED";
    case 1: return "running";
    case 2: return "BUS-OFF";
    case 3: return "recovering";
  }
  return "?";
}

void routes() {
  g_server.on("/", HTTP_GET, [](PsychicRequest* r) {
    return r->reply(200, "text/html", kPage);
  });

  g_server.on("/api/status", HTTP_GET, [](PsychicRequest* r) {
    twai_status_info_t st{};
    const bool valid = twai_get_status_info(&st) == ESP_OK;
    const uint32_t now = millis();

    char out[1600];
    int n = 0;
    n += snprintf(out + n, sizeof(out) - n, "%s  up %lus\n", kVersion,
                  static_cast<unsigned long>(now / 1000));
    n += snprintf(out + n, sizeof(out) - n, "mode    %s  txGate %s\n",
                  g_listenBoot ? "LISTEN-ONLY sniffer (no TX, no ACK, protocol parked)"
                               : "ARMED (normal, ACKing, talking)",
                  g_txGate ? "open" : "SHUT");
    n += snprintf(out + n, sizeof(out) - n,
                  "sync    0x%02X %s%s%s%s\n",
                  g_sync,
                  (g_sync & FAILED)     ? "FAILED "     : "",
                  (g_sync & PEER_ALIVE) ? "PEER_ALIVE " : "",
                  (g_sync & START)      ? "START "      : "",
                  (g_sync & FUNCSREG)   ? "FUNCSREG "   : "");
    n += snprintf(out + n, sizeof(out) - n, "goal    %s\n", goalName(g_goal));
    n += snprintf(out + n, sizeof(out) - n, "text    '%s'%s\n", g_text,
                  g_doneAtMs ? "  (delivered)" : "");
    n += snprintf(out + n, sizeof(out) - n,
                  "panel   pings %lu (last %ld ms ago)  syncReqs %lu  regVoids %lu  keys %lu\n",
                  static_cast<unsigned long>(g_pings),
                  g_lastPingMs ? static_cast<long>(now - g_lastPingMs) : -1,
                  static_cast<unsigned long>(g_syncReqs),
                  static_cast<unsigned long>(g_regVoids),
                  static_cast<unsigned long>(g_keys));
    n += snprintf(out + n, sizeof(out) - n,
                  "us      pongs %lu  hellos %lu  autoAcks %lu  regOk %lu/%lu  xferOk %lu/%lu\n",
                  static_cast<unsigned long>(g_pongs),
                  static_cast<unsigned long>(g_hellos),
                  static_cast<unsigned long>(g_autoAcks),
                  static_cast<unsigned long>(g_regOk),
                  static_cast<unsigned long>(g_regOk + g_regFail),
                  static_cast<unsigned long>(g_xferOk),
                  static_cast<unsigned long>(g_xferOk + g_xferFail));
    n += snprintf(out + n, sizeof(out) - n,
                  "frames  rx %lu  tx %lu  txRefused %lu  fifoDrop %lu  syncLosses %lu  "
                  "strayReplies %lu\n",
                  static_cast<unsigned long>(g_rxCount),
                  static_cast<unsigned long>(g_txCount),
                  static_cast<unsigned long>(g_txRefused),
                  static_cast<unsigned long>(g_fifoDrop),
                  static_cast<unsigned long>(g_syncLosses),
                  static_cast<unsigned long>(g_replyStray));
    n += snprintf(out + n, sizeof(out) - n,
                  "drv     %s  txErr %lu  rxErr %lu  busErr %lu  arbLost %lu  rxMissed %lu\n",
                  drvStateName(valid, static_cast<uint8_t>(st.state)),
                  static_cast<unsigned long>(st.tx_error_counter),
                  static_cast<unsigned long>(st.rx_error_counter),
                  static_cast<unsigned long>(st.bus_error_count),
                  static_cast<unsigned long>(st.arb_lost_count),
                  static_cast<unsigned long>(st.rx_missed_count));
    n += snprintf(out + n, sizeof(out) - n, "wifi    %s  rssi %d\n",
                  WiFi.isConnected() ? WiFi.localIP().toString().c_str() : "AP mode",
                  WiFi.RSSI());
    return r->reply(200, "text/plain", out);
  });

  g_server.on("/api/frames", HTTP_GET, [](PsychicRequest* r) {
    char out[2600];
    int n = 0;
    portENTER_CRITICAL(&g_frameMux);
    for (uint8_t i = 0; i < kFrameRing; ++i) {
      const FrameRec& f = g_frames[(g_frameHead + i) % kFrameRing];
      if (!f.count) continue;
      n += snprintf(out + n, sizeof(out) - n,
                    "%s %03X  %02X %02X %02X %02X %02X %02X %02X %02X",
                    f.dir ? "TX" : "RX", f.id,
                    f.d[0], f.d[1], f.d[2], f.d[3], f.d[4], f.d[5], f.d[6], f.d[7]);
      if (f.count > 1)
        n += snprintf(out + n, sizeof(out) - n, "  x%lu (t=%lu..%lu)\n",
                      static_cast<unsigned long>(f.count),
                      static_cast<unsigned long>(f.firstMs),
                      static_cast<unsigned long>(f.lastMs));
      else
        n += snprintf(out + n, sizeof(out) - n, "  (t=%lu)\n",
                      static_cast<unsigned long>(f.firstMs));
      if (n > static_cast<int>(sizeof(out)) - 80) break;
    }
    portEXIT_CRITICAL(&g_frameMux);
    if (n == 0) n = snprintf(out, sizeof(out), "(nothing heard or sent yet)\n");
    return r->reply(200, "text/plain", out);
  });

  g_server.on("/api/log", HTTP_GET, [](PsychicRequest* r) {
    char out[3400];
    int n = 0;
    portENTER_CRITICAL(&g_logMux);
    for (size_t i = 0; i < kLogRing; ++i) {
      const LogRec& rec = g_log[(g_logHead + i) % kLogRing];
      if (!rec.ms && !rec.msg[0]) continue;
      n += snprintf(out + n, sizeof(out) - n, "%8lu  %s\n",
                    static_cast<unsigned long>(rec.ms), rec.msg);
      if (n > static_cast<int>(sizeof(out)) - 140) break;
    }
    portEXIT_CRITICAL(&g_logMux);
    if (n == 0) n = snprintf(out, sizeof(out), "(empty)\n");
    return r->reply(200, "text/plain", out);
  });

  // THE IS-IT-US TEST. Shutting the gate stops every frame WE would hand the driver, in
  // software; the controller keeps ACKing in hardware. One caveat this rig taught: a frame
  // ALREADY in the TX queue keeps retransmitting until the next bus-off reinstall clears
  // it, so wait ~30 s after gating before reading the busErr slope as our-TX-free.
  g_server.on("/api/txgate", HTTP_GET, [](PsychicRequest* r) {
    const bool on = !(r->getParam("on") && r->getParam("on")->value() == "0");
    g_txGate = on;
    logmsg("console: TX gate %s", on ? "OPEN" : "SHUT - we hand the driver nothing now");
    return r->reply(200, "text/plain", "ok");
  });

  // Reboot into the other mode. NVS write stalls CAN reception for the flash write, which
  // costs nothing here — we are about to reboot anyway.
  g_server.on("/api/mode", HTTP_GET, [](PsychicRequest* r) {
    const bool listen = r->getParam("listen") && r->getParam("listen")->value() == "1";
    Preferences p;
    if (!p.begin(kOwnNamespace, /*readOnly=*/false))
      return r->reply(500, "text/plain", "nvs open failed");
    p.putUChar("listen", listen ? 1 : 0);
    p.end();
    logmsg("console: rebooting into %s mode", listen ? "LISTEN-ONLY sniffer" : "ARMED");
    g_rebootAt = millis() + 300;
    return r->reply(200, "text/plain", listen ? "rebooting into listen-only" : "rebooting armed");
  });

  g_server.on("/api/text", HTTP_GET, [](PsychicRequest* r) {
    const String t = r->getParam("t") ? r->getParam("t")->value() : String();
    if (!t.length()) return r->reply(200, "text/plain", "need t=");
    snprintf(g_text, sizeof(g_text), "%s", t.c_str());
    logmsg("console: text '%s'", g_text);
    if (g_goal == Goal::Done) { g_doneAtMs = 0; goalReset(Goal::ShowText, "new text"); }
    return r->reply(200, "text/plain", "ok");
  });

  g_server.on("/api/restart", HTTP_GET, [](PsychicRequest* r) {
    g_doneAtMs = 0;
    goalReset(Goal::WaitSync, "console asked for a full redo");
    return r->reply(200, "text/plain", "ok");
  });

  g_server.on("/api/reboot", HTTP_GET, [](PsychicRequest* r) {
    g_rebootAt = millis() + 300;
    return r->reply(200, "text/plain", "rebooting");
  });
}

void startNetwork() {
  // READ-ONLY: an NVS write stalls CAN reception for the duration of the flash write.
  Preferences p;
  String ssid, pass;
  if (p.begin(kWifiNamespace, /*readOnly=*/true)) {
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
  // The two lockouts that have each cost a cable: a full socket table is PERMANENT without
  // lru_purge_enable, and the URI table is a fixed array that fails silently when overrun.
  g_server.config.lru_purge_enable  = true;
  g_server.config.max_open_sockets  = 7;
  g_server.config.recv_wait_timeout = 3;
  g_server.config.send_wait_timeout = 3;
  g_server.config.max_uri_handlers  = 32;
  g_server.config.stack_size        = 8192;

  g_server.listen(80);

  // OTA FIRST, ALWAYS — its three routes take their slots before anything else can crowd
  // them off the end of the table. An OTA write stalls the CAN ISR, so gate our transmitter.
  ElegantOTA.onStart([]() {
    g_otaRunning = true;
    g_txGate = false;
    logmsg("ota started - CAN TX gated");
  });
  ElegantOTA.onEnd([](bool ok) {
    if (!ok) { g_otaRunning = false; g_txGate = true; }
    logmsg("ota %s", ok ? "ok, rebooting" : "FAILED - TX ungated");
  });
  ElegantOTA.begin(&g_server);

  routes();
}

}  // namespace

void setup() {
  // THE VERY FIRST INSTRUCTION. A floating TXD is not guaranteed recessive; if it drifts
  // low the transceiver holds the whole bus dominant while we boot. Claim it and hold it
  // recessive before anything else runs.
  pinMode(kTxPin, OUTPUT);
  digitalWrite(kTxPin, HIGH);

  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  delay(300);
  Serial.printf("\n%s - bare protocol, no AffaDisplay\n", kVersion);

  {
    Preferences p;
    if (p.begin(kOwnNamespace, /*readOnly=*/true)) {
      g_listenBoot = p.getUChar("listen", 0) != 0;
      p.end();
    }
  }

  // CAN FIRST, WIFI SECOND. The seconds after a shared-supply power-on are the only window
  // in which a fresh panel is polite, and WiFi.begin() blocks for up to fifteen of them.
  //
  // Warm boots do not reset peripherals: after OTA the TWAI block still holds the previous
  // run's configuration and degrades a little further with every reflash. Reset it while no
  // driver is installed — this is the one safe moment.
  periph_module_reset(PERIPH_TWAI_MODULE);

  if (g_listenBoot) {
    // PURE SNIFFER: raw TWAI in LISTEN_ONLY, no esp32_can, no tasks of anybody's to join,
    // nothing transmitted ever (the controller does not even ACK). loop() polls.
    g_txGate = false;
    twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(kTxPin, kRxPin,
                                                          TWAI_MODE_LISTEN_ONLY);
    g.rx_queue_len = 32;
    twai_timing_config_t t = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    const bool ok = twai_driver_install(&g, &t, &f) == ESP_OK && twai_start() == ESP_OK;
    Serial.printf("[can] %s at %lu bit/s LISTEN-ONLY sniffer (rx=GPIO%d tx=GPIO%d)\n",
                  ok ? "RUNNING" : "NOT RUNNING", static_cast<unsigned long>(kBitrate),
                  static_cast<int>(kRxPin), static_cast<int>(kTxPin));
  } else {
    // BEFORE begin(): it only writes two members the watchdog task reads later. 0 would
    // park a bus-off in STOPPED for ever; 2000 accumulates. 250 is measured for this rig.
    CAN0.setForceRecovery(true, kForceRecoveryMs);

    // Order is load-bearing: pins, begin(), callback, watchFor() LAST.
    CAN0.setCANPins(kRxPin, kTxPin);
    CAN0.begin(kBitrate);
    CAN0.setGeneralCallback(&onCanFrame);
    CAN0.watchFor();

    twai_status_info_t st{};
    const bool ok = twai_get_status_info(&st) == ESP_OK && st.state == TWAI_STATE_RUNNING;
    Serial.printf("[can] %s at %lu bit/s NORMAL (rx=GPIO%d tx=GPIO%d) - armed from power-on\n",
                  ok ? "RUNNING" : "NOT RUNNING", static_cast<unsigned long>(kBitrate),
                  static_cast<int>(kRxPin), static_cast<int>(kTxPin));
  }

  // Network AFTER the bus. OTA is still reachable long before anyone can need it.
  startNetwork();
  startHttp();

  logmsg("%s up - CAN was live %lu ms before WiFi finished", kVersion,
         static_cast<unsigned long>(millis()));
  logmsg("armed: B9+BA heartbeat, pong every ping (no auth gate), hello on 61 11, "
         "then register -> power -> 1s -> '%s'", g_text);
}

void loop() {
  ElegantOTA.loop();

  const uint32_t now = millis();
  if (g_rebootAt && expired(now, g_rebootAt)) ESP.restart();

  // An OTA write stalls the CAN ISR; do not fight it for the bus.
  if (g_otaRunning) { delay(10); return; }

  if (g_listenBoot) {
    // Sniffer: drain the raw driver into the trace ring and do nothing else. The protocol
    // FSM stays parked — a node that cannot transmit cannot hold up its end of it.
    twai_message_t m;
    while (twai_receive(&m, 0) == ESP_OK) {
      if (m.extd || m.rtr) continue;
      ++g_rxCount;
      g_lastRxMs = now;
      tap(static_cast<uint16_t>(m.identifier), m.data,
          m.data_length_code > 8 ? 8 : m.data_length_code, /*tx=*/false);
    }
    delay(2);
    return;
  }

  pumpRx(now);       // pongs, hellos, ACKs, reply routing — everything event-driven
  pumpTick(now);     // the 1 Hz heartbeat and the peer-alive watchdog
  pumpGoal(now);     // register -> power on -> warm up -> SUCCESS

  // 5 ms: an order of magnitude faster than the storm can fill the FIFO, and IDLE still
  // gets fed (loopTask is prio 1, IDLE is 0 — starving it panics the single-core C3).
  delay(5);
}
