// Host smoke test for the AffaDisplay core.
//
// It is deliberately not the full suite the specification calls for (test_sync,
// test_isotp, test_text and test_link land with the panels). What it does do is pin the
// four things that would otherwise be claims: the ring drops the NEWEST frame, the sync
// heartbeat is paced by the clock and not by the call count, the transmit FSM puts the
// registration probes and the ISO-TP frames on the wire in the exact legacy byte order,
// and a self-echoed key fires exactly once.
//
// Everything here runs against LoopbackLink + FakeClock. No hardware, no Arduino.

#include <unity.h>
#include <cstring>
#include <initializer_list>

#include "AffaConfig.h"
#include "core/AffaDisplayBase.h"
#include "core/AffaRing.h"
#include "link/LoopbackLink.h"
#include "util/AffaText.h"

using namespace affa;

namespace {

// ---------------------------------------------------------------------------
// Harness
// ---------------------------------------------------------------------------

struct FakeClock final : IClock {
  uint32_t t = 0;
  uint32_t millis() const override { return t; }
  void advance(uint32_t ms) { t += ms; }
};

// The Carminat profile, spelled out here rather than included: the panel folder is a
// later phase, and the core must be testable without it.
constexpr uint8_t kHello[3][8] = {
  {0x70, 0x1A, 0x11, 0x00, 0x00, 0x00, 0x00, 0x01},
  {0xB0, 0x14, 0x11, 0x00, 0x1F, 0x00, 0x00, 0x00},
  {0xB0, 0x14, 0x11, 0x00, 0x1F, 0x00, 0x00, 0x00},
};
constexpr SyncProfile kProfile{0x3AF, 0x3CF, 0x0400, 0xB9, 0xBA, 0x00, 0x00, kHello, 3};
constexpr uint16_t kFuncIds[] = {0x151, 0x1F1};

int g_keyCount = 0;
Key g_lastKey = Key::Load;
KeyEdge g_lastEdge = KeyEdge::Click;

class TestDisplay final : public AffaDisplayBase {
 public:
  TestDisplay(ICanLink& l, IClock& c) : AffaDisplayBase(l, c, kProfile, kFuncIds, 2) {}
  bool supports(Feature f) const override { return f == Feature::KeyTx; }

 protected:
  uint8_t  packetFiller() const override { return 0x00; }
  uint16_t keyTxId()      const override { return 0x1C1; }

  bool onFrame(const Frame& f) override {
    if (f.id != 0x1C1) return false;
    Key k; KeyEdge e;
    if (!decodeKeyFrame(f, k, e)) return false;
    routeKey(k, e);
    return true;
  }
};

void countKey(Key k, KeyEdge e, void*) { ++g_keyCount; g_lastKey = k; g_lastEdge = e; }

Frame mk(uint32_t id, std::initializer_list<uint8_t> bytes) {
  Frame f;
  f.id = id;
  f.len = 8;
  uint8_t i = 0;
  for (uint8_t b : bytes) { if (i < 8) f.data[i++] = b; }
  return f;
}

void drainSent(LoopbackLink<>& l) { Frame f; while (l.takeSent(f)) {} }

// Pops the next transmitted frame and asserts it byte for byte.
void expectFrame(LoopbackLink<>& l, uint32_t id, const uint8_t (&want)[8],
                 const char* what) {
  Frame f;
  TEST_ASSERT_TRUE_MESSAGE(l.takeSent(f), what);
  TEST_ASSERT_EQUAL_HEX32_MESSAGE(id, f.id, what);
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(8, f.len, what);
  TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(want, f.data, 8, what);
}

// Brings the link to "panel has answered": the handshake is what gates every send.
void establishSync(TestDisplay& d, LoopbackLink<>& l) {
  d.begin();
  l.inject(mk(0x3CF, {0x61, 0x11, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3}));
  d.poll();
  drainSent(l);
  TEST_ASSERT_TRUE(d.synced());
}

} // namespace

// ---------------------------------------------------------------------------
// AffaRing
// ---------------------------------------------------------------------------

void test_ring_is_fifo_and_drops_the_newest(void) {
  AffaRing<int, 4> r;
  TEST_ASSERT_TRUE(r.empty());
  for (int i = 0; i < 4; ++i) TEST_ASSERT_TRUE(r.push(i));

  // Full. The fifth push is refused and counted — the OLDEST is kept, because a hole in
  // the middle of an ISO-TP transfer decodes into a plausible-looking wrong screen,
  // whereas a lost tail shows up as a missed ACK the transmit FSM already handles.
  TEST_ASSERT_FALSE(r.push(99));
  TEST_ASSERT_EQUAL_UINT32(1, r.overflow());

  int v = -1;
  for (int i = 0; i < 4; ++i) {
    TEST_ASSERT_TRUE(r.pop(v));
    TEST_ASSERT_EQUAL_INT(i, v);
  }
  TEST_ASSERT_FALSE(r.pop(v));

  // The counters are free-running, so the ring must still work after wrapping past the
  // capacity many times over.
  for (int i = 0; i < 1000; ++i) {
    TEST_ASSERT_TRUE(r.push(i));
    TEST_ASSERT_TRUE(r.pop(v));
    TEST_ASSERT_EQUAL_INT(i, v);
  }
}

// ---------------------------------------------------------------------------
// AffaText
// ---------------------------------------------------------------------------

void test_text_transliterates_and_truncates_safely(void) {
  char out[64];

  TEST_ASSERT_EQUAL_UINT32(5, affa::toAscii("HELLO", out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("HELLO", out);

  // Polish, Cyrillic, Ukrainian, and the '?' fallback for an unmapped codepoint.
  affa::toAscii("\xC5\x81\xC3\xB3" "d" "\xC5\xBA", out, sizeof(out)); // "Lodz"
  TEST_ASSERT_EQUAL_STRING("Lodz", out);
  affa::toAscii("\xD0\x96\xD1\x83\xD0\xBA", out, sizeof(out));        // Zh u k
  TEST_ASSERT_EQUAL_STRING("Zhuk", out);
  affa::toAscii("\xD0\x87\xD0\xB6", out, sizeof(out));                // Yi zh
  TEST_ASSERT_EQUAL_STRING("Yizh", out);
  affa::toAscii("\xE2\x82\xAC", out, sizeof(out));                    // EURO SIGN
  TEST_ASSERT_EQUAL_STRING("?", out);

  // Truncation always NUL-terminates and never emits half of a replacement: "Sch" is
  // three bytes and only two fit, so it is dropped whole rather than written as "Sc".
  char small[4];
  affa::toAscii("A\xD0\xA9", small, sizeof(small));                   // 'A' + Shcha
  TEST_ASSERT_EQUAL_STRING("A", small);

  // A zero-sized buffer writes nothing at all, including no NUL.
  TEST_ASSERT_EQUAL_UINT32(0, affa::toAscii("X", small, 0));

  // normalizeTitle strips the annotation and trims.
  affa::normalizeTitle("Song \xE2\x80\xA2 Video ", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("Song", out);
}

// ---------------------------------------------------------------------------
// Sync FSM
// ---------------------------------------------------------------------------

void test_heartbeat_is_paced_by_the_clock_not_the_call_count(void) {
  LoopbackLink<> link;
  FakeClock clk;
  TestDisplay d(link, clk);

  d.begin();
  link.inject(mk(0x3CF, {0x61, 0x11, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3}));
  d.poll();                    // clears Failed, answers with the three hello frames
  drainSent(link);

  // One million poll() calls across one simulated second. The panel keeps pinging, so
  // the link must survive; and the heartbeat is a wall-clock deadline, so exactly one
  // 0xB9 may leave. A call-counting implementation emits a storm of them and then tears
  // the link down — which is precisely the defect this library exists to prevent.
  // begin() armed the heartbeat at t = 0 and the poll above consumed it, so the window
  // swept here is (0, 1000] and must contain exactly one more.
  for (uint32_t i = 0; i < 1000000; ++i) {
    clk.t = 1 + (i * 1000) / 1000000;   // sweep 1 .. 1000 ms
    if ((i % 250000) == 0)
      link.inject(mk(0x3CF, {0x69, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3}));
    d.poll();
    TEST_ASSERT_TRUE_MESSAGE(d.synced(), "sync dropped inside one simulated second");
  }

  int alive = 0, other = 0;
  Frame f;
  while (link.takeSent(f)) {
    if (f.id == 0x3AF && f.data[0] == 0xB9) ++alive; else ++other;
  }
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, alive, "exactly one heartbeat per second, always");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, other, "nothing else belongs on the sync id here");
}

void test_heartbeat_frame_is_byte_exact(void) {
  LoopbackLink<> link;
  FakeClock clk;
  TestDisplay d(link, clk);

  d.begin();
  d.poll();                    // still FAILED: heartbeat, then the sync request

  static const uint8_t kAlive[8]   = {0xB9, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  static const uint8_t kRequest[8] = {0xBA, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  expectFrame(link, 0x3AF, kAlive,   "1 Hz alive heartbeat");
  expectFrame(link, 0x3AF, kRequest, "sync request, emitted only while FAILED");
  Frame f;
  TEST_ASSERT_FALSE_MESSAGE(link.takeSent(f), "nothing else leaves on a FAILED poll");
}

void test_hello_reply_is_three_frames_the_last_two_identical(void) {
  LoopbackLink<> link;
  FakeClock clk;
  TestDisplay d(link, clk);

  d.begin();
  clk.t = 5;                   // past begin()'s heartbeat arming, but under 1000 ms
  d.poll();
  drainSent(link);
  clk.t = 6;

  link.inject(mk(0x3CF, {0x61, 0x11, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3}));
  d.poll();

  static const uint8_t kH0[8] = {0x70, 0x1A, 0x11, 0x00, 0x00, 0x00, 0x00, 0x01};
  static const uint8_t kH1[8] = {0xB0, 0x14, 0x11, 0x00, 0x1F, 0x00, 0x00, 0x00};
  expectFrame(link, 0x3AF, kH0, "hello / announce");
  expectFrame(link, 0x3AF, kH1, "capability announce");
  expectFrame(link, 0x3AF, kH1, "capability announce, sent a SECOND time — observed");
  TEST_ASSERT_TRUE(d.synced());
}

void test_peer_watchdog_is_a_millisecond_deadline(void) {
  LoopbackLink<> link;
  FakeClock clk;
  TestDisplay d(link, clk);
  establishSync(d, link);

  // Four seconds of pings at 1 Hz, polled hard. Still up.
  for (uint32_t s = 1; s <= 4; ++s) {
    link.inject(mk(0x3CF, {0x69, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3}));
    clk.t += 1000;
    for (int i = 0; i < 100; ++i) d.poll();
    TEST_ASSERT_TRUE_MESSAGE(d.synced(), "a ping per second must hold the link up");
  }

  // Now the panel goes quiet. The deadline is AFFA_PEER_TIMEOUT_MS of WALL CLOCK, not
  // five calls of anything.
  clk.t += AFFA_PEER_TIMEOUT_MS + AFFA_SYNC_INTERVAL_MS + 1;
  d.poll();
  TEST_ASSERT_FALSE_MESSAGE(d.synced(), "peer loss must be declared on the clock");
  TEST_ASSERT_FALSE_MESSAGE(d.registered(), "FUNCSREG does not survive a resync");
}

// ---------------------------------------------------------------------------
// Transmit FSM: registration order and ISO-TP framing
// ---------------------------------------------------------------------------

void test_lazy_registration_then_isotp_frames_are_byte_exact(void) {
  LoopbackLink<> link;
  FakeClock clk;
  TestDisplay d(link, clk);
  establishSync(d, link);

  // Self-ACK drives the whole sequence out without a panel: PARTIAL while bytes remain,
  // DONE on the last. The wire frames are identical to a real send.
  d.setSelfAck(true);

  uint8_t payload[22];
  for (uint8_t i = 0; i < 22; ++i) payload[i] = static_cast<uint8_t>(0x01 + i);

  TxOptions opt;
  opt.slot = RenderSlot::Text;
  const TxTicket t = d.enqueue(0x151, payload, sizeof(payload), opt);
  TEST_ASSERT_NOT_EQUAL(kNoTicket, t);
  TEST_ASSERT_FALSE_MESSAGE(d.registered(), "FUNCSREG must not latch before the probes");

  for (int i = 0; i < 8; ++i) d.poll();   // clock frozen: no heartbeat interferes

  // Registration walks the WHOLE function table in declaration order, one byte 0x70 each,
  // padded with the Carminat filler. That order is on the wire and must not change.
  static const uint8_t kReg[8] = {0x70, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  expectFrame(link, 0x151, kReg, "register 0x151 first");
  expectFrame(link, 0x1F1, kReg, "register 0x1F1 second");

  // Frame 0 carries EIGHT raw payload bytes with no PCI added by the transport.
  static const uint8_t kF0[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
  static const uint8_t kF1[8] = {0x21, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F};
  static const uint8_t kF2[8] = {0x22, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16};
  expectFrame(link, 0x151, kF0, "ISO-TP frame 0: 8 payload bytes, no PCI");
  expectFrame(link, 0x151, kF1, "continuation 0x21: 7 payload bytes");
  expectFrame(link, 0x151, kF2, "continuation 0x22: 7 payload bytes");

  Frame f;
  TEST_ASSERT_FALSE_MESSAGE(link.takeSent(f), "22 bytes is exactly three frames");
  TEST_ASSERT_TRUE_MESSAGE(d.registered(), "FUNCSREG latches after the last probe");
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Result::Ok),
                          static_cast<uint8_t>(d.lastResult()));
  TEST_ASSERT_EQUAL_UINT16(t, d.lastTicket());
  TEST_ASSERT_FALSE(d.busy());
}

void test_enqueue_is_gated_and_bounded(void) {
  LoopbackLink<> link;
  FakeClock clk;
  TestDisplay d(link, clk);
  d.begin();

  uint8_t one = 0x11;
  // FAILED gates everything: nothing may reach the wire before the panel has answered.
  TEST_ASSERT_EQUAL_UINT16(kNoTicket, d.enqueue(0x151, &one, 1));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Result::NoSync),
                          static_cast<uint8_t>(d.lastResult()));

  establishSync(d, link);
  TEST_ASSERT_EQUAL_UINT16(kNoTicket, d.enqueue(0x999, &one, 1));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Result::UnknownFunc),
                          static_cast<uint8_t>(d.lastResult()));
  TEST_ASSERT_EQUAL_UINT16(kNoTicket, d.enqueue(0x151, nullptr, 4));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Result::BadArgument),
                          static_cast<uint8_t>(d.lastResult()));

  uint8_t big[AFFA_MAX_PAYLOAD];
  std::memset(big, 0x5A, sizeof(big));
  TEST_ASSERT_EQUAL_UINT16(kNoTicket, d.enqueue(0x151, big, AFFA_MAX_PAYLOAD + 1));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Result::TooLong),
                          static_cast<uint8_t>(d.lastResult()));
}

void test_coalescing_keeps_one_slot_and_aborts_the_rest(void) {
  static int aborted;
  aborted = 0;

  LoopbackLink<> link;
  FakeClock clk;
  TestDisplay d(link, clk);
  establishSync(d, link);
  d.setSelfAck(true);

  // Latch FUNCSREG first, so the queue is not carrying registration probes.
  uint8_t seed = 0xAA;
  (void)d.enqueue(0x151, &seed, 1);
  for (int i = 0; i < 8; ++i) d.poll();
  TEST_ASSERT_TRUE(d.registered());
  drainSent(link);

  d.onComplete([](TxTicket, Result r, void*) { if (r == Result::Aborted) ++aborted; },
               nullptr);

  TxOptions opt;
  opt.slot = RenderSlot::Text;
  uint8_t v[4] = {0, 0, 0, 0};
  for (int i = 0; i < 100; ++i) {
    v[0] = static_cast<uint8_t>(i);
    TEST_ASSERT_NOT_EQUAL(kNoTicket, d.enqueue(0x151, v, sizeof(v), opt));
  }

  // A repeated render of one slot occupies exactly ONE queue slot regardless of render
  // rate, and it always holds the newest value. Without this the panel keeps displaying
  // superseded values for a second after the key, which reads as a key-handling defect
  // and is really a queueing one.
  TEST_ASSERT_EQUAL_INT_MESSAGE(99, aborted, "99 superseded renders must report Aborted");
  TEST_ASSERT_TRUE(d.busy());
  TEST_ASSERT_EQUAL_UINT8(0, d.queued());        // one job, nothing behind it
  Frame f;
  TEST_ASSERT_FALSE_MESSAGE(link.takeSent(f), "coalescing must not transmit anything");

  d.poll();
  TEST_ASSERT_TRUE(link.takeSent(f));
  TEST_ASSERT_EQUAL_HEX8_MESSAGE(99, f.data[0], "the surviving job holds the 100th value");
}

void test_abort_pending_leaves_the_in_flight_job_alone(void) {
  LoopbackLink<> link;
  FakeClock clk;
  TestDisplay d(link, clk);
  establishSync(d, link);
  d.setSelfAck(true);

  uint8_t seed = 0xAA;
  (void)d.enqueue(0x151, &seed, 1);
  for (int i = 0; i < 8; ++i) d.poll();
  drainSent(link);

  uint8_t big[22];
  std::memset(big, 0x33, sizeof(big));
  TxOptions menu;  menu.slot = RenderSlot::Menu;
  TxOptions text;  text.slot = RenderSlot::Text;

  const TxTicket inFlight = d.enqueue(0x151, big, sizeof(big), menu);
  d.poll();                                  // frame 0 of the menu is on the wire
  const TxTicket queued = d.enqueue(0x151, &seed, 1, text);
  TEST_ASSERT_NOT_EQUAL(kNoTicket, queued);
  TEST_ASSERT_TRUE(d.pending(RenderSlot::Text));

  TEST_ASSERT_EQUAL_UINT8(1, d.abortPending());
  TEST_ASSERT_FALSE(d.pending(RenderSlot::Text));
  TEST_ASSERT_EQUAL_UINT16(queued, d.lastTicket());
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Result::Aborted),
                          static_cast<uint8_t>(d.lastResult()));

  // A nested-equivalent second call finds nothing.
  TEST_ASSERT_EQUAL_UINT8(0, d.abortPending());

  // The menu finishes untouched.
  for (int i = 0; i < 8; ++i) d.poll();
  TEST_ASSERT_EQUAL_UINT16(inFlight, d.lastTicket());
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Result::Ok),
                          static_cast<uint8_t>(d.lastResult()));
}

// ---------------------------------------------------------------------------
// Keys
// ---------------------------------------------------------------------------

void test_key_decode_matches_the_wire_vectors(void) {
  struct V { uint16_t raw; uint16_t key; bool hold; };
  static const V kV[] = {
    {0x0000, 0x0000, false},   // Load, click
    {0x00C0, 0x0000, true },   // Load, hold: data[3] |= 0xC0
    {0x0001, 0x0001, false},
    {0x00C1, 0x0001, true },
    {0x0005, 0x0005, false},   // Pause
    {0x0101, 0x0101, false},   // RollUp   — EXEMPT from the mask
    {0x0141, 0x0141, false},   // RollDown — EXEMPT; masking would report it as RollUp
    {0x01C1, 0x0101, true },   // held detent: 0x0101|0xC0 and 0x0141|0xC0 collide here,
                               // so direction is unrecoverable and it decodes as RollUp
  };

  LoopbackLink<> link;
  FakeClock clk;
  TestDisplay d(link, clk);
  establishSync(d, link);
  d.onKey(&countKey, nullptr);

  for (const V& v : kV) {
    g_keyCount = 0;
    link.inject(mk(0x1C1, {0x03, 0x89, static_cast<uint8_t>(v.raw >> 8),
                           static_cast<uint8_t>(v.raw & 0xFF), 0, 0, 0, 0}));
    d.poll();
    TEST_ASSERT_EQUAL_INT(1, g_keyCount);
    TEST_ASSERT_EQUAL_HEX16(v.key, static_cast<uint16_t>(g_lastKey));
    TEST_ASSERT_EQUAL_INT(v.hold ? 1 : 0, g_lastEdge == KeyEdge::Hold ? 1 : 0);
  }
}

void test_non_key_traffic_on_the_key_id_is_rejected(void) {
  LoopbackLink<> link;
  FakeClock clk;
  TestDisplay d(link, clk);
  establishSync(d, link);
  d.onKey(&countKey, nullptr);
  g_keyCount = 0;

  // All three are real panel-to-radio payloads on 0x1C1. Without the 03 89 guard a
  // decoder invents keys 0x640F and 0x3030 out of them.
  link.inject(mk(0x1C1, {0x70, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3}));
  link.inject(mk(0x1C1, {0x02, 0x64, 0x0F, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3}));
  link.inject(mk(0x1C1, {0x05, 0x63, 0x30, 0x30, 0x33, 0x37, 0xA3, 0xA3}));
  link.inject(mk(0x1C1, {0x03, 0x88, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));
  d.poll();
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_keyCount, "no phantom keys from non-key traffic");
}

void test_auto_ack_answers_the_key_id_and_never_the_sync_id(void) {
  LoopbackLink<> link;
  FakeClock clk;
  TestDisplay d(link, clk);
  establishSync(d, link);

  link.inject(mk(0x1C1, {0x70, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3}));
  d.poll();

  static const uint8_t kAck[8] = {0x74, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  expectFrame(link, 0x5C1, kAck, "RX 1C1 70 .. -> TX 5C1 74 ..");

  // The sync channel has no ACK semantics. 0x7AF must never appear on the wire — it has
  // once, from an early build that ACKed its own transmissions, and nothing was listening.
  link.inject(mk(0x3CF, {0x69, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3}));
  d.poll();
  Frame f;
  while (link.takeSent(f)) TEST_ASSERT_NOT_EQUAL_MESSAGE(0x7AF, f.id, "never ACK the sync id");
}

void test_self_sent_key_fires_exactly_once_on_an_echoing_link(void) {
  LoopbackLink<> link;
  FakeClock clk;
  TestDisplay d(link, clk);
  establishSync(d, link);
  d.onKey(&countKey, nullptr);

  // A real controller never receives its own transmissions; LoopbackLink can. Without
  // the fromSelf rule, pressKey(..., Both) would fire once on hardware and twice here,
  // and every host test would be lying about the target.
  link.setEcho(true);
  g_keyCount = 0;
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Result::Ok),
                          static_cast<uint8_t>(d.pressKey(Key::Pause, KeyEdge::Click,
                                                          KeySource::Both)));
  for (int i = 0; i < 4; ++i) d.poll();
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_keyCount, "the echo must not deliver a second key");

  static const uint8_t kWire[8] = {0x03, 0x89, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00};
  expectFrame(link, 0x1C1, kWire, "emulated key: 03 89 hi lo, bytes 4..7 literal zero");

  // A hold on a wheel code has no wire representation at all, so it is refused rather
  // than silently downgraded to a click — a fine step where the caller asked for a
  // coarse one is a wrong screen the caller cannot detect.
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Result::NotSupported),
                          static_cast<uint8_t>(d.pressKey(Key::RollUp, KeyEdge::Hold,
                                                          KeySource::Wire)));
}

void test_key_delivery_does_not_wait_for_the_transmit_queue(void) {
  LoopbackLink<> link;
  FakeClock clk;
  TestDisplay d(link, clk);
  establishSync(d, link);
  d.onKey(&countKey, nullptr);

  d.setSelfAck(true);
  uint8_t seed = 0xAA;
  (void)d.enqueue(0x151, &seed, 1);
  for (int i = 0; i < 8; ++i) d.poll();
  TEST_ASSERT_TRUE(d.registered());
  d.setSelfAck(false);                   // now nothing will ACK: the job sits in WaitAck
  drainSent(link);

  uint8_t big[AFFA_MAX_PAYLOAD];
  std::memset(big, 0x77, sizeof(big));
  (void)d.enqueue(0x151, big, sizeof(big));
  d.poll();                              // frame 0 out, WaitAck with a 2000 ms deadline
  TEST_ASSERT_TRUE(d.busy());

  // The guarantee, stated as the sentence a test asserts: the number of poll() calls
  // between a key frame entering the RX ring and the key callback firing is exactly one,
  // regardless of transmit queue depth or of any message in flight.
  g_keyCount = 0;
  link.inject(mk(0x1C1, {0x03, 0x89, 0x00, 0x05, 0, 0, 0, 0}));
  d.poll();
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_keyCount, "one poll, whatever the TX FSM is doing");
  TEST_ASSERT_TRUE_MESSAGE(d.busy(), "and the in-flight job is untouched");
}

// ---------------------------------------------------------------------------
// Observation seam
// ---------------------------------------------------------------------------

void test_subscription_matches_id_and_payload_under_a_mask(void) {
  static int hits;
  hits = 0;

  LoopbackLink<> link;
  FakeClock clk;
  TestDisplay d(link, clk);
  establishSync(d, link);

  FrameMatch m{};
  m.id     = 0x151;
  m.idMask = 0x7FF;
  m.dir    = Direction::Rx;
  const uint8_t want[8] = {0x21, 0x20, 0x20, 0xB0, 0x30, 0x30, 0x30, 0x20};
  std::memcpy(m.data, want, 8);
  std::memset(m.dataMask, 0xFF, 8);
  m.len = 8;

  const SubHandle h = d.subscribe(m, [](const Frame&, void*) { ++hits; }, nullptr);
  TEST_ASSERT_TRUE_MESSAGE(h.valid(), "an ignored kNoSub is a subscription that never fires");
  TEST_ASSERT_EQUAL_UINT8(1, d.subscriptions());

  link.inject(mk(0x151, {0x21, 0x20, 0x20, 0xB0, 0x30, 0x30, 0x30, 0x20}));
  link.inject(mk(0x151, {0x21, 0x20, 0x20, 0xB0, 0x30, 0x30, 0x30, 0x21}));  // one byte off
  link.inject(mk(0x1F1, {0x21, 0x20, 0x20, 0xB0, 0x30, 0x30, 0x30, 0x20}));  // wrong id
  d.poll();
  TEST_ASSERT_EQUAL_INT(1, hits);

  TEST_ASSERT_TRUE(d.unsubscribe(h));
  TEST_ASSERT_FALSE_MESSAGE(d.unsubscribe(h), "a stale handle must not free a reused slot");
  link.inject(mk(0x151, {0x21, 0x20, 0x20, 0xB0, 0x30, 0x30, 0x30, 0x20}));
  d.poll();
  TEST_ASSERT_EQUAL_INT(1, hits);
}

void test_unsupported_calls_report_instead_of_pretending(void) {
  LoopbackLink<> link;
  FakeClock clk;
  TestDisplay d(link, clk);
  d.begin();

  // The legacy IDisplay returned NoError from silently no-op bodies, so calling one on a
  // panel that could not do it looked exactly like success.
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Result::NotSupported),
                          static_cast<uint8_t>(d.setText("X")));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Result::NotSupported),
                          static_cast<uint8_t>(d.showConfirmBox("a", "b", "c")));
  TEST_ASSERT_FALSE(d.supports(Feature::Menu));
  TEST_ASSERT_TRUE(d.supports(Feature::KeyTx));
}

// ---------------------------------------------------------------------------

void setUp(void) {}
void tearDown(void) {}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_ring_is_fifo_and_drops_the_newest);
  RUN_TEST(test_text_transliterates_and_truncates_safely);
  RUN_TEST(test_heartbeat_frame_is_byte_exact);
  RUN_TEST(test_hello_reply_is_three_frames_the_last_two_identical);
  RUN_TEST(test_heartbeat_is_paced_by_the_clock_not_the_call_count);
  RUN_TEST(test_peer_watchdog_is_a_millisecond_deadline);
  RUN_TEST(test_lazy_registration_then_isotp_frames_are_byte_exact);
  RUN_TEST(test_enqueue_is_gated_and_bounded);
  RUN_TEST(test_coalescing_keeps_one_slot_and_aborts_the_rest);
  RUN_TEST(test_abort_pending_leaves_the_in_flight_job_alone);
  RUN_TEST(test_key_decode_matches_the_wire_vectors);
  RUN_TEST(test_non_key_traffic_on_the_key_id_is_rejected);
  RUN_TEST(test_auto_ack_answers_the_key_id_and_never_the_sync_id);
  RUN_TEST(test_self_sent_key_fires_exactly_once_on_an_echoing_link);
  RUN_TEST(test_key_delivery_does_not_wait_for_the_transmit_queue);
  RUN_TEST(test_subscription_matches_id_and_payload_under_a_mask);
  RUN_TEST(test_unsupported_calls_report_instead_of_pretending);
  return UNITY_END();
}
