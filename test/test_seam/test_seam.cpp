// The three-layer observation seam.
//
// Layer 0 — one frame tap, every frame in and out, in wire order.
// Layer 1 — a fixed table of FrameMatch subscriptions, purely observational.
// Layer 2 — decoded protocol events.
//
// The two silent failure modes this suite exists to catch: a stale SubHandle unsubscribing
// the slot's NEXT owner (the failure mode of a bare index, and it is silent), and a full
// table returning kNoSub to a caller who ignored the return value — which is a subscription
// that never fires and looks exactly like a decoder bug.

#include "../affa_test_support.h"

#include "carminat/CarminatDisplay.h"
#include <cstring>

using namespace affa;
using affatest::mk;
using affatest::drain;
using affatest::pumpUntilIdle;

namespace {

// ---------------------------------------------------------------------------
// Recorders
// ---------------------------------------------------------------------------

struct Counters {
  int hits[8] = {0};
};
Counters g_c;

template <int N>
void hit(const Frame&, void*) { ++g_c.hits[N]; }

int g_tapRx = 0, g_tapTx = 0;
uint32_t g_tapLastId = 0;
void tapAll(const Frame& f, Direction d, void*) {
  if (d == Direction::Rx) ++g_tapRx; else ++g_tapTx;
  g_tapLastId = f.id;
}
int g_tap2 = 0;
void tapSecond(const Frame&, Direction, void*) { ++g_tap2; }

struct EventLog {
  int sync = 0, registered = 0, peerLost = 0, key = 0;
  int txComplete = 0, linkError = 0;
  SyncState prev = SyncState::None, now = SyncState::None;
  Key       lastKey = Key::Load;
  KeyEdge   lastEdge = KeyEdge::Click;
  TxTicket  lastTicket = kNoTicket;
  Result    lastResult = Result::Ok;
  LinkErrorKind lastError = LinkErrorKind::RingOverflow;
  int       ringOverflows = 0, txDrops = 0;
};
EventLog g_ev;

void record(const Event& e, void*) {
  switch (e.kind) {
    case EventKind::SyncChanged:
      ++g_ev.sync; g_ev.prev = e.sync.prev; g_ev.now = e.sync.now; break;
    case EventKind::Registered:    ++g_ev.registered; break;
    case EventKind::PeerLost:      ++g_ev.peerLost; break;
    case EventKind::Key:
      ++g_ev.key; g_ev.lastKey = e.key.key; g_ev.lastEdge = e.key.edge; break;
    case EventKind::TxComplete:
      ++g_ev.txComplete; g_ev.lastTicket = e.tx.ticket; g_ev.lastResult = e.tx.result; break;
    case EventKind::LinkError:
      ++g_ev.linkError;
      g_ev.lastError = e.error.kind;
      if (e.error.kind == LinkErrorKind::RingOverflow) ++g_ev.ringOverflows;
      if (e.error.kind == LinkErrorKind::TxDropped)    ++g_ev.txDrops;
      break;
  }
}

struct Rig {
  LoopbackLink<256> link;
  affatest::FakeClock clk;
  CarminatDisplay d;
  Rig() : d(link, clk) {}

  void sync() {
    d.begin();
    link.inject(affatest::panelSyncRequest());
    d.poll();
    TEST_ASSERT_TRUE(d.synced());
    drain(link);
  }
  void up() {
    sync();
    d.setSelfAck(true);
    (void)d.setPower(true);
    pumpUntilIdle(d);
    TEST_ASSERT_TRUE(d.registered());
    drain(link);
  }
};

FrameMatch exactId(uint32_t id, Direction dir = Direction::Rx) {
  FrameMatch m{};
  m.id     = id;
  m.idMask = 0x7FF;
  m.dir    = dir;
  return m;
}

}  // namespace

// ---------------------------------------------------------------------------
// Layer 1 — matching
// ---------------------------------------------------------------------------

void test_a_default_match_is_inert_rather_than_a_firehose(void) {
  // A DEFAULT-CONSTRUCTED FrameMatch matches id 0x000 inbound and nothing else. AFFA uses
  // no such id, so a half-filled match fires never instead of on everything.
  Rig r;
  r.sync();
  g_c = Counters{};

  FrameMatch m{};
  TEST_ASSERT_TRUE(r.d.subscribe(m, &hit<0>, nullptr).valid());
  r.link.inject(mk(0x151, {1, 2, 3, 4, 5, 6, 7, 8}));
  r.link.inject(mk(0x1C1, {1, 2, 3, 4, 5, 6, 7, 8}));
  r.d.poll();
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_c.hits[0], "an unfilled match must be inert");

  // Matching every id is the explicit opt-in idMask = 0.
  FrameMatch any{};
  any.idMask = 0;
  TEST_ASSERT_TRUE(r.d.subscribe(any, &hit<1>, nullptr).valid());
  r.link.inject(mk(0x151, {1, 2, 3, 4, 5, 6, 7, 8}));
  r.link.inject(mk(0x1C1, {1, 2, 3, 4, 5, 6, 7, 8}));
  r.d.poll();
  TEST_ASSERT_EQUAL_INT_MESSAGE(2, g_c.hits[1], "idMask = 0 is 'any id'");
}

void test_the_id_mask_selects_a_range(void) {
  Rig r;
  r.sync();
  g_c = Counters{};

  FrameMatch m{};
  m.id     = 0x150;
  m.idMask = 0x7F0;               // 0x150 .. 0x15F
  m.dir    = Direction::Rx;
  TEST_ASSERT_TRUE(r.d.subscribe(m, &hit<0>, nullptr).valid());

  r.link.inject(mk(0x151, {0}));
  r.link.inject(mk(0x15F, {0}));
  r.link.inject(mk(0x161, {0}));  // outside the range
  r.d.poll();
  TEST_ASSERT_EQUAL_INT(2, g_c.hits[0]);
}

void test_the_payload_mask_selects_bytes_and_bits(void) {
  Rig r;
  r.sync();
  g_c = Counters{};

  // The example from docs/API.md: the exact frame that armed the legacy password sequence.
  FrameMatch m = exactId(0x151);
  const uint8_t want[8] = {0x21, 0x20, 0x20, 0xB0, 0x30, 0x30, 0x30, 0x20};
  std::memcpy(m.data, want, 8);
  std::memset(m.dataMask, 0xFF, 8);
  m.len = 8;
  TEST_ASSERT_TRUE(r.d.subscribe(m, &hit<0>, nullptr).valid());

  // Two bytes matched, the rest don't-care.
  FrameMatch loose = exactId(0x151);
  loose.data[0]     = 0x10;
  loose.dataMask[0] = 0xFF;
  loose.data[2]     = 0x21;
  loose.dataMask[2] = 0xFF;
  loose.len         = 3;
  TEST_ASSERT_TRUE(r.d.subscribe(loose, &hit<1>, nullptr).valid());

  // A BIT mask inside one byte: the top nibble of the ISO-TP PCI.
  FrameMatch bits = exactId(0x151);
  bits.data[0]     = 0x20;
  bits.dataMask[0] = 0xF0;
  bits.len         = 1;
  TEST_ASSERT_TRUE(r.d.subscribe(bits, &hit<2>, nullptr).valid());

  r.link.inject(mk(0x151, {0x21, 0x20, 0x20, 0xB0, 0x30, 0x30, 0x30, 0x20}));
  r.link.inject(mk(0x151, {0x21, 0x20, 0x20, 0xB0, 0x30, 0x30, 0x30, 0x21}));  // 1 byte off
  r.link.inject(mk(0x151, {0x10, 0x5A, 0x21, 0x01, 0x7E, 0x80, 0x00, 0x00}));  // a menu FF
  r.link.inject(mk(0x151, {0x2C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));
  r.d.poll();

  TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_c.hits[0], "a full 8-byte match is exact");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_c.hits[1], "byte 1 is don't-care under the mask");
  TEST_ASSERT_EQUAL_INT_MESSAGE(3, g_c.hits[2], "0x2n continuations, by top nibble");
}

void test_a_short_frame_never_matches_a_payload_rule(void) {
  Rig r;
  r.sync();
  g_c = Counters{};

  FrameMatch m = exactId(0x3CF);
  m.data[0]     = 0x61;
  m.dataMask[0] = 0xFF;
  m.data[2]     = 0x01;
  m.dataMask[2] = 0xFF;
  m.len         = 3;
  TEST_ASSERT_TRUE(r.d.subscribe(m, &hit<0>, nullptr).valid());

  Frame shortDlc = mk(0x3CF, {0x61, 0x11, 0x01, 0, 0, 0, 0, 0});
  shortDlc.len = 2;
  r.link.inject(shortDlc);
  r.d.poll();
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_c.hits[0],
                                "data[2] was never transmitted, so it cannot match");

  r.link.inject(mk(0x3CF, {0x61, 0x11, 0x01, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3}));
  r.d.poll();
  TEST_ASSERT_EQUAL_INT(1, g_c.hits[0]);
}

void test_direction_selects_which_side_of_the_bus(void) {
  Rig r;
  r.up();
  g_c = Counters{};

  TEST_ASSERT_TRUE(r.d.subscribe(exactId(0x151, Direction::Rx),   &hit<0>, nullptr).valid());
  TEST_ASSERT_TRUE(r.d.subscribe(exactId(0x151, Direction::Tx),   &hit<1>, nullptr).valid());
  TEST_ASSERT_TRUE(r.d.subscribe(exactId(0x151, Direction::Both), &hit<2>, nullptr).valid());

  r.link.inject(mk(0x151, {0x10, 0x5A, 0x21, 0x01, 0x7E, 0x80, 0x00, 0x00}));  // inbound
  ASSERT_RESULT(Ok, r.d.setTime("1234"));                                       // outbound
  pumpUntilIdle(r.d);

  TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_c.hits[0], "Rx sees only what the other node sent");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_c.hits[1], "Tx sees only what we sent");
  TEST_ASSERT_EQUAL_INT_MESSAGE(2, g_c.hits[2], "Both sees the whole channel");
}

void test_subscribe_refuses_an_unsatisfiable_match(void) {
  Rig r;
  r.sync();

  FrameMatch bad = exactId(0x151);
  bad.len = 9;                                     // longer than any CAN frame
  TEST_ASSERT_FALSE(r.d.subscribe(bad, &hit<0>, nullptr).valid());

  FrameMatch noDir = exactId(0x151);
  noDir.dir = static_cast<Direction>(0);            // matches nothing, ever
  TEST_ASSERT_FALSE(r.d.subscribe(noDir, &hit<0>, nullptr).valid());

  TEST_ASSERT_FALSE(r.d.subscribe(exactId(0x151), nullptr, nullptr).valid());
  TEST_ASSERT_EQUAL_UINT8(0, r.d.subscriptions());
}

// ---------------------------------------------------------------------------
// Layer 1 — handles and the table
// ---------------------------------------------------------------------------

void test_a_stale_handle_cannot_unsubscribe_the_slots_next_owner(void) {
  // The silent failure mode of a bare index: hand out slot 0, free it, hand slot 0 to
  // somebody else, then let the first owner's forgotten handle free the second owner's
  // subscription. The generation counter is what makes that impossible.
  Rig r;
  r.sync();
  g_c = Counters{};

  const SubHandle first = r.d.subscribe(exactId(0x151), &hit<0>, nullptr);
  TEST_ASSERT_TRUE(first.valid());
  TEST_ASSERT_TRUE(r.d.unsubscribe(first));
  TEST_ASSERT_FALSE_MESSAGE(r.d.unsubscribe(first), "a handle cannot be redeemed twice");

  const SubHandle second = r.d.subscribe(exactId(0x151), &hit<1>, nullptr);
  TEST_ASSERT_TRUE(second.valid());
  TEST_ASSERT_NOT_EQUAL_MESSAGE(first.v, second.v,
                                "the reused slot must hand out a DIFFERENT handle");

  TEST_ASSERT_FALSE_MESSAGE(r.d.unsubscribe(first),
                            "the stale handle must not free the slot's new owner");
  TEST_ASSERT_EQUAL_UINT8(1, r.d.subscriptions());

  r.link.inject(mk(0x151, {0}));
  r.d.poll();
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_c.hits[0], "the freed subscription is gone");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_c.hits[1], "and the live one still fires");
}

void test_a_full_table_returns_kNoSub_without_disturbing_the_others(void) {
  Rig r;
  r.sync();
  g_c = Counters{};

  SubHandle h[AFFA_MAX_SUBSCRIPTIONS];
  for (uint8_t i = 0; i < AFFA_MAX_SUBSCRIPTIONS; ++i) {
    h[i] = r.d.subscribe(exactId(0x151), &hit<0>, nullptr);
    TEST_ASSERT_TRUE(h[i].valid());
  }
  TEST_ASSERT_EQUAL_UINT8(AFFA_MAX_SUBSCRIPTIONS, r.d.subscriptions());

  const SubHandle overflow = r.d.subscribe(exactId(0x1C1), &hit<1>, nullptr);
  TEST_ASSERT_FALSE_MESSAGE(overflow.valid(), "a full table returns kNoSub");
  TEST_ASSERT_EQUAL_UINT16(kNoSub.v, overflow.v);
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(AFFA_MAX_SUBSCRIPTIONS, r.d.subscriptions(),
                                  "and does not consume or corrupt a slot");

  r.link.inject(mk(0x151, {0}));
  r.link.inject(mk(0x1C1, {0x70, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3}));
  r.d.poll();
  TEST_ASSERT_EQUAL_INT_MESSAGE(AFFA_MAX_SUBSCRIPTIONS, g_c.hits[0],
                                "every existing subscription still fires");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_c.hits[1], "the rejected one never fires — check valid()");

  // Freeing one makes room again.
  TEST_ASSERT_TRUE(r.d.unsubscribe(h[3]));
  TEST_ASSERT_TRUE(r.d.subscribe(exactId(0x1C1), &hit<1>, nullptr).valid());
  drain(r.link);
  r.link.inject(mk(0x1C1, {0x70, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3}));
  r.d.poll();
  TEST_ASSERT_EQUAL_INT(1, g_c.hits[1]);
}

namespace {
SubHandle g_victim;
CarminatDisplay* g_display = nullptr;
void unsubscribeVictim(const Frame&, void*) {
  ++g_c.hits[0];
  g_display->unsubscribe(g_victim);
}
}  // namespace

void test_unsubscribing_from_inside_a_callback_is_well_defined(void) {
  // subscribe()/unsubscribe() are legal from inside a FrameCb. A slot freed mid-walk is
  // SKIPPED rather than mis-dispatched, and a slot added mid-walk does not receive the
  // frame currently being dispatched.
  Rig r;
  r.sync();
  g_c = Counters{};
  g_display = &r.d;

  TEST_ASSERT_TRUE(r.d.subscribe(exactId(0x151), &unsubscribeVictim, nullptr).valid());
  g_victim = r.d.subscribe(exactId(0x151), &hit<1>, nullptr);
  TEST_ASSERT_TRUE(g_victim.valid());

  r.link.inject(mk(0x151, {0}));
  r.d.poll();
  TEST_ASSERT_EQUAL_INT(1, g_c.hits[0]);
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_c.hits[1], "a slot freed mid-walk is skipped");
}

// ---------------------------------------------------------------------------
// Layer 1 — re-entrancy from a Direction::Tx callback
//
// docs/API.md §4.2 permits rendering from a FrameCb and §4.3 permits enqueue(),
// abortPending() and abortAll() from ANY callback. A Direction::Tx FrameCb fires from
// inside observe(), which txFrame() calls from inside pumpTx() — i.e. while the job that
// produced the frame is sitting at _queue[0] and the transmit pump still holds a
// reference to it. Everything that decides whether a job is preemptable keys off
// TxJob::started, so `started` MUST already be true by the time that callback can run.
// ---------------------------------------------------------------------------

namespace {

struct TxReentry {
  CarminatDisplay* d       = nullptr;
  int              hits    = 0;
  uint8_t          aborted = 0xFF;
  Result           second  = Result::NotSupported;
};
TxReentry g_re;

void abortFromTxCallback(const Frame&, void*) {
  if (++g_re.hits != 1) return;              // only on the very first transmitted frame
  g_re.aborted = g_re.d->abortPending();
}

void renderFromTxCallback(const Frame&, void*) {
  if (++g_re.hits != 1) return;
  g_re.second = g_re.d->showMenu("XXX", "YYY", "ZZZ");
}

}  // namespace

void test_abortPending_from_a_tx_callback_spares_the_frame_it_is_watching(void) {
  Rig r;
  r.up();
  g_re = TxReentry{};
  g_re.d = &r.d;

  ASSERT_RESULT(Ok, r.d.showMenu("ONE", "TWO", "SIX"));   // multi-frame, RenderSlot::Menu
  const TxTicket menu = r.d.lastEnqueued();
  ASSERT_RESULT(Ok, r.d.setTime("1234"));                 // queued behind it
  TEST_ASSERT_EQUAL_UINT8(1, r.d.queued());

  TEST_ASSERT_TRUE(
      r.d.subscribe(exactId(0x151, Direction::Tx), &abortFromTxCallback, nullptr).valid());

  r.d.poll();   // pumpTx sends the menu's frame 0; the Tx callback fires inside that send

  TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_re.hits, "the Tx callback must have fired");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(
      1, g_re.aborted,
      "only the queued clock render is preemptable — the menu already has a frame on the "
      "wire and abortPending() must not touch it");
  TEST_ASSERT_TRUE_MESSAGE(r.d.busy(), "the in-flight menu survives its own Tx callback");

  pumpUntilIdle(r.d);
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(menu, r.d.lastTicket(),
                                   "and completes, rather than being reported Aborted");
  ASSERT_RESULT(Ok, r.d.lastResult());
}

void test_a_render_from_a_tx_callback_cannot_coalesce_into_the_frame_on_the_wire(void) {
  // Same slot, same funcId, coalescing on. findCoalescable() must skip the started job:
  // overwriting its payload mid-ISO-TP would send the tail of a DIFFERENT screen at the
  // offsets the first one already declared.
  Rig r;
  r.up();
  g_re = TxReentry{};
  g_re.d = &r.d;

  ASSERT_RESULT(Ok, r.d.showMenu("ONE", "TWO", "SIX"));
  TEST_ASSERT_EQUAL_UINT8(0, r.d.queued());

  TEST_ASSERT_TRUE(
      r.d.subscribe(exactId(0x151, Direction::Tx), &renderFromTxCallback, nullptr).valid());

  r.d.poll();

  TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_re.hits, "the Tx callback must have fired");
  ASSERT_RESULT(Ok, g_re.second);
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(
      1, r.d.queued(),
      "the second render must QUEUE behind the message on the wire, not replace it");
}

// ---------------------------------------------------------------------------
// Layer 0 — the tap
// ---------------------------------------------------------------------------

void test_the_tap_sees_both_directions_and_only_one_tap_exists(void) {
  Rig r;
  r.up();
  g_tapRx = g_tapTx = 0;
  g_tap2 = 0;
  r.d.onFrame(&tapAll, nullptr);

  r.link.inject(mk(0x1C1, {0x70, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3}));
  r.d.poll();                     // one inbound, one auto-ACK out
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_tapRx, "the inbound frame");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_tapTx, "and the 0x5C1 ACK it produced");
  TEST_ASSERT_EQUAL_HEX32(0x5C1, g_tapLastId);

  // A second call REPLACES the first; there is exactly one tap.
  r.d.onFrame(&tapSecond, nullptr);
  const int rxBefore = g_tapRx;
  r.link.inject(mk(0x1C1, {0x70, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3}));
  r.d.poll();
  TEST_ASSERT_EQUAL_INT_MESSAGE(rxBefore, g_tapRx, "the first tap was replaced");
  TEST_ASSERT_EQUAL_INT(2, g_tap2);

  // nullptr removes it.
  r.d.onFrame(nullptr, nullptr);
  r.link.inject(mk(0x1C1, {0x70, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3}));
  r.d.poll();
  TEST_ASSERT_EQUAL_INT(2, g_tap2);
  drain(r.link);
}

void test_a_refused_transmission_is_never_observed(void) {
  // A frame the link REFUSED never existed on the bus, so the tap must not see it — a
  // sniffer that showed it would be showing traffic that was not there.
  Rig r;
  r.sync();
  g_tapRx = g_tapTx = 0;
  r.d.onFrame(&tapAll, nullptr);

  r.link.setLive(false);
  r.clk.advance(1000);
  r.d.poll();                     // pumpSync tries to transmit the heartbeat and fails
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_tapTx, "a refused frame is not observed");
}

// ---------------------------------------------------------------------------
// Layer 2 — events
// ---------------------------------------------------------------------------

void test_sync_registered_and_txcomplete_fire_at_the_right_moment(void) {
  Rig r;
  g_ev = EventLog{};
  r.d.begin();
  r.d.onEvent(&record, nullptr);

  // begin() writes SyncState::Failed DIRECTLY rather than transitioning to it: firing a
  // callback from inside setup() would surprise every application.
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_ev.sync, "begin() is a reset, not a transition");

  r.link.inject(affatest::panelSyncRequest());
  r.d.poll();
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_ev.sync, "the handshake is one transition");
  TEST_ASSERT_TRUE(hasFlag(g_ev.prev, SyncState::Failed));
  TEST_ASSERT_FALSE(hasFlag(g_ev.now, SyncState::Failed));
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_ev.registered, "FUNCSREG has not latched yet");

  r.d.setSelfAck(true);
  ASSERT_RESULT(Ok, r.d.setPower(true));
  const TxTicket t = r.d.lastEnqueued();
  TEST_ASSERT_NOT_EQUAL(kNoTicket, t);
  pumpUntilIdle(r.d);

  TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_ev.registered, "Registered fires once, on the latch");
  TEST_ASSERT_EQUAL_INT_MESSAGE(2, g_ev.sync,
                                "and it is ALSO a SyncChanged — the extra event is additional");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_ev.txComplete, "one payload ticket completed");
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(t, g_ev.lastTicket,
                                   "registration jobs carry kNoTicket and are invisible");
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Result::Ok),
                          static_cast<uint8_t>(g_ev.lastResult));
}

void test_peerlost_fires_on_the_deadline_and_says_so_twice(void) {
  Rig r;
  r.up();
  g_ev = EventLog{};
  r.d.onEvent(&record, nullptr);

  r.clk.t = AFFA_PEER_TIMEOUT_MS + 1;
  r.d.poll();
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_ev.peerLost, "PeerLost");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_ev.sync, "and the SyncChanged that carries the state");
  TEST_ASSERT_TRUE(hasFlag(g_ev.now, SyncState::Failed));
  TEST_ASSERT_FALSE_MESSAGE(hasFlag(g_ev.now, SyncState::FuncsReg),
                            "FUNCSREG is dropped with the peer");
}

void test_the_key_event_fires_whether_or_not_the_menu_consumed_it(void) {
  Rig r;
  r.up();
  g_ev = EventLog{};
  r.d.onEvent(&record, nullptr);

  r.link.inject(mk(0x1C1, {0x03, 0x89, 0x00, 0x05, 0xA3, 0xA3, 0xA3, 0xA3}));
  r.d.poll();
  TEST_ASSERT_EQUAL_INT(1, g_ev.key);
  TEST_ASSERT_EQUAL_HEX16(0x0005, static_cast<uint16_t>(g_ev.lastKey));
  TEST_ASSERT_TRUE(g_ev.lastEdge == KeyEdge::Click);

  // A key that OPENS the menu is consumed by the menu and still fires the event, because
  // ev.key is what ARRIVED rather than what was left over.
  MenuItem it{};
  it.label = "Item";
  r.d.getMenu().addItem(it);
  r.link.inject(mk(0x1C1, {0x03, 0x89, 0x00, 0xC0, 0xA3, 0xA3, 0xA3, 0xA3}));
  r.d.poll();
  TEST_ASSERT_TRUE_MESSAGE(r.d.getMenu().isOpen(), "hold-Load is the OEM open gesture");
  TEST_ASSERT_EQUAL_INT_MESSAGE(2, g_ev.key, "the consumed key still fires the event");
  pumpUntilIdle(r.d);
  drain(r.link);
}

void test_linkerror_reports_a_dropped_transmission(void) {
  Rig r;
  r.sync();
  g_ev = EventLog{};
  r.d.onEvent(&record, nullptr);

  r.link.setLive(false);
  r.clk.advance(1000);
  r.d.poll();
  TEST_ASSERT_GREATER_OR_EQUAL_MESSAGE(1, g_ev.txDrops, "ICanLink::send() refused a frame");
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(LinkErrorKind::TxDropped),
                          static_cast<uint8_t>(g_ev.lastError));
}

void test_linkerror_reports_a_ring_overflow(void) {
  // Non-zero ringOverflow means frames were LOST: poll() is not being called often enough
  // or the ring is too small. Both are the application's to fix, so it must be told.
  //
  // 0x151 is in the function table, so an inbound frame there is neither auto-ACKed nor
  // answered — the traffic exists purely to overrun the ring.
  LoopbackLink<4> link;
  affatest::FakeClock clk;
  CarminatDisplay d(link, clk);
  g_ev = EventLog{};

  d.begin();
  d.onEvent(&record, nullptr);
  for (int i = 0; i < 8; ++i) link.inject(mk(0x151, {0x10, 0x5A, 0x21, 0x01, 0, 0, 0, 0}));
  d.poll();

  TEST_ASSERT_GREATER_OR_EQUAL_MESSAGE(1, g_ev.ringOverflows, "the loss must be reported");
  TEST_ASSERT_GREATER_OR_EQUAL(1u, d.stats().ringOverflow);
}

// ---------------------------------------------------------------------------
// Layer 2b — onText, the inbound text callback
// ---------------------------------------------------------------------------
// test_radiotext_and_screenchanged_have_no_emitter_yet was here. It asserted that two
// declared EventKinds were never emitted, and existed to make the choice between wiring an
// emitter and deleting the enumerators unavoidable. The choice was made twice: the two
// enumerators went, and then inbound text came back as onText WITH the emitter these tests
// exercise. UpdateListBase's protected onRadioText(bool) hook is a different thing and is
// unaffected — a single-frame AUX heuristic on a subclass seam, not a published event.

#if AFFA_ENABLE_ISOTP_RX
int  g_textN = 0;
char g_textLast[64];
void recordText(const char* t, void*) {
  ++g_textN;
  std::strncpy(g_textLast, t, sizeof(g_textLast) - 1);
  g_textLast[sizeof(g_textLast) - 1] = '\0';
}

// The three frames another head unit puts on 0x151 for setText("RENAULT") — the golden
// vector from test_carminat_wire, played back at us instead of by us.
void injectRenault(LoopbackLink<256>& link) {
  link.inject(mk(0x151, {0x10, 0x0E, 0x77, 0x55, 0x55, 0xFF, 0x60, 0x01}));
  link.inject(mk(0x151, {0x21, 0x52, 0x45, 0x4E, 0x41, 0x55, 0x4C, 0x54}));
  link.inject(mk(0x151, {0x22, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));
}

void test_onText_delivers_a_reassembled_inbound_string(void) {
  Rig r; g_textN = 0; g_textLast[0] = '\0';
  r.d.begin();
  r.d.onText(&recordText, nullptr);

  injectRenault(r.link);
  r.d.poll();

  TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_textN, "one complete message must deliver exactly once");
  TEST_ASSERT_EQUAL_STRING("RENAULT", g_textLast);
}

// The failure this guards: emitting on every appended frame instead of at the declared
// length, which delivers the same screen once per continuation.
void test_onText_does_not_fire_on_a_partial_message(void) {
  Rig r; g_textN = 0;
  r.d.begin();
  r.d.onText(&recordText, nullptr);

  r.link.inject(mk(0x151, {0x10, 0x0E, 0x77, 0x55, 0x55, 0xFF, 0x60, 0x01}));
  r.link.inject(mk(0x151, {0x21, 0x52, 0x45, 0x4E, 0x41, 0x55, 0x4C, 0x54}));
  r.d.poll();
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_textN, "an incomplete transfer must deliver nothing");

  r.link.inject(mk(0x151, {0x22, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));
  r.d.poll();
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_textN, "the last frame completes it");
}

// Our OWN renders come back off an echoing link stamped fromSelf. Decoding those into
// onText would report every string we transmit as inbound text.
void test_onText_never_reports_our_own_render(void) {
  Rig r; g_textN = 0;
  r.up();                                  // synced + registered, self-ACK on
  r.link.setEcho(true);
  r.d.onText(&recordText, nullptr);

  ASSERT_RESULT(Ok, r.d.setText("HELLO", 255));
  pumpUntilIdle(r.d);
  r.d.poll();

  TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_textN, "a self-sent render is not inbound text");
}

// A screen is not a text line. The 0x21 windowed menu shares the id and must not be
// delivered as a string by the 0x74/0x77 decoder.
void test_onText_ignores_a_payload_that_is_not_text(void) {
  Rig r; g_textN = 0;
  r.d.begin();
  r.d.onText(&recordText, nullptr);

  r.link.inject(mk(0x151, {0x10, 0x04, 0x21, 0x01, 0x7E, 0x80, 0x00, 0x00}));
  r.d.poll();
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_textN, "a menu screen is not a text line");
}
#endif  // AFFA_ENABLE_ISOTP_RX

// ---------------------------------------------------------------------------

void setUp(void) {}
void tearDown(void) {}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_default_match_is_inert_rather_than_a_firehose);
  RUN_TEST(test_the_id_mask_selects_a_range);
  RUN_TEST(test_the_payload_mask_selects_bytes_and_bits);
  RUN_TEST(test_a_short_frame_never_matches_a_payload_rule);
  RUN_TEST(test_direction_selects_which_side_of_the_bus);
  RUN_TEST(test_subscribe_refuses_an_unsatisfiable_match);
  RUN_TEST(test_a_stale_handle_cannot_unsubscribe_the_slots_next_owner);
  RUN_TEST(test_a_full_table_returns_kNoSub_without_disturbing_the_others);
  RUN_TEST(test_unsubscribing_from_inside_a_callback_is_well_defined);
  RUN_TEST(test_abortPending_from_a_tx_callback_spares_the_frame_it_is_watching);
  RUN_TEST(test_a_render_from_a_tx_callback_cannot_coalesce_into_the_frame_on_the_wire);
  RUN_TEST(test_the_tap_sees_both_directions_and_only_one_tap_exists);
  RUN_TEST(test_a_refused_transmission_is_never_observed);
  RUN_TEST(test_sync_registered_and_txcomplete_fire_at_the_right_moment);
  RUN_TEST(test_peerlost_fires_on_the_deadline_and_says_so_twice);
  RUN_TEST(test_the_key_event_fires_whether_or_not_the_menu_consumed_it);
  RUN_TEST(test_linkerror_reports_a_dropped_transmission);
  RUN_TEST(test_linkerror_reports_a_ring_overflow);
#if AFFA_ENABLE_ISOTP_RX
  RUN_TEST(test_onText_delivers_a_reassembled_inbound_string);
  RUN_TEST(test_onText_does_not_fire_on_a_partial_message);
  RUN_TEST(test_onText_never_reports_our_own_render);
  RUN_TEST(test_onText_ignores_a_payload_that_is_not_text);
#endif
  return UNITY_END();
}
