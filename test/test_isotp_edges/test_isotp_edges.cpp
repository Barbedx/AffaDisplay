// The transport's edges: the 113-byte ceiling, the frame that would need a 17th
// continuation counter, and the ACK semantics that make or break every menu render.
//
// 113 IS A WIRE LIMIT, NOT A BUDGET: frame 0 carries 8 raw payload bytes and each
// continuation 7, with the counter 0x20 | (num & 0x0F), so 8 + 15*7 = 113 and the last PCI
// is 0x2F. showConfirmBox sits at exactly that with zero headroom.
//
// And finding #2, which is the whole reason the menu works at all:
// "DONE WHILE BYTES REMAIN" IS SUCCESS, NOT A SHORT WRITE.

#include "../affa_test_support.h"

#include "carminat/CarminatDisplay.h"
#include "proto/IsoTp.h"

using namespace affa;
using affatest::drain;
using affatest::pumpUntilIdle;

namespace {

struct Rig {
  LoopbackLink<256> link;
  affatest::FakeClock clk;
  CarminatDisplay d;
  Rig() : d(link, clk) {}

  void up() {
    d.begin();
    link.inject(affatest::panelSyncRequest());
    d.poll();
    d.setSelfAck(true);
    d.setPower(true);
    pumpUntilIdle(d);
    TEST_ASSERT_TRUE(d.registered());
    drain(link);
  }
};

void fill(uint8_t* p, uint8_t n) {
  for (uint8_t i = 0; i < n; ++i) p[i] = static_cast<uint8_t>(0x40 + i);
}

}  // namespace

// ---------------------------------------------------------------------------
// The ceiling
// ---------------------------------------------------------------------------

void test_113_bytes_is_16_frames_ending_at_PCI_2F(void) {
  Rig r;
  r.up();

  uint8_t p[113];
  fill(p, sizeof(p));
  const TxTicket t = r.d.enqueue(0x151, p, sizeof(p));
  TEST_ASSERT_NOT_EQUAL_MESSAGE(kNoTicket, t, "113 bytes is exactly the transport ceiling");
  pumpUntilIdle(r.d);
  ASSERT_RESULT(Ok, r.d.lastResult());

  Frame f;
  int n = 0;
  uint8_t lastPci = 0;
  while (r.link.takeSent(f)) {
    if (n == 0) {
      // Frame 0 carries EIGHT raw payload bytes with NO PCI added by the transport.
      TEST_ASSERT_EQUAL_HEX8_MESSAGE(p[0], f.data[0], "frame 0 has no transport PCI");
    } else {
      lastPci = f.data[0];
    }
    ++n;
  }
  TEST_ASSERT_EQUAL_INT_MESSAGE(16, n, "8 + 15*7 = 113");
  TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x2F, lastPci, "the counter's absolute maximum");
}

void test_114_bytes_is_refused_with_nothing_on_the_wire(void) {
  Rig r;
  r.up();

  uint8_t p[AFFA_MAX_PAYLOAD];
  fill(p, sizeof(p));
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(kNoTicket,
                                   r.d.enqueue(0x151, p, AFFA_MAX_PAYLOAD + 1),
                                   "114 bytes would need a 17th continuation counter");
  ASSERT_RESULT(TooLong, r.d.lastResult());

  // The refusal is synchronous and total: not one byte may reach the link, because a
  // partially transmitted screen is worse than none.
  pumpUntilIdle(r.d);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, r.link.sentCount(), "a TooLong send transmits nothing");
}

void test_showConfirmBox_at_exactly_113_still_succeeds(void) {
  // 2 + 6 + 105 = 113. There is no headroom at all: a single extra byte anywhere in this
  // builder turns a working screen into Result::TooLong.
  Rig r;
  r.up();
  ASSERT_RESULT(Ok, r.d.showConfirmBox("OK", "Line one", "Line two"));
  pumpUntilIdle(r.d);
  ASSERT_RESULT(Ok, r.d.lastResult());

  Frame f;
  int n = 0;
  uint8_t lastPci = 0;
  while (r.link.takeSent(f)) { if (n) lastPci = f.data[0]; ++n; }
  TEST_ASSERT_EQUAL_INT_MESSAGE(16, n, "showConfirmBox is sixteen frames");
  TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x2F, lastPci, "and it ends at the counter ceiling");
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(113, AFFA_MAX_PAYLOAD,
                                   "AFFA_MAX_PAYLOAD is the validated wire limit");
}

// ---------------------------------------------------------------------------
// ACK semantics
// ---------------------------------------------------------------------------

void test_done_while_bytes_remain_completes_with_Ok(void) {
  // The panel ends the transfer as soon as it holds the number of content bytes the first
  // frame DECLARED, which is fewer than showMenu's builder holds. This is therefore the
  // NORMAL path on every single menu render, not an edge case: reporting SendFailed would
  // make the menu look permanently broken.
  Rig r;
  r.up();
  r.d.setSelfAck(false);
  r.link.setAutoAck(true);
  r.link.setAutoAckPartials(0);        // DONE to the very first frame

  uint8_t p[96];
  fill(p, sizeof(p));
  const TxTicket t = r.d.enqueue(0x151, p, sizeof(p));
  TEST_ASSERT_NOT_EQUAL(kNoTicket, t);
  pumpUntilIdle(r.d);

  ASSERT_RESULT(Ok, r.d.lastResult());
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(t, r.d.lastTicket(), "the ticket that completed");
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, r.link.sentCount(),
                                   "the sender stops the moment the panel says DONE");
}

void test_partial_after_the_last_frame_is_SendFailed(void) {
  // The mirror image, and the reason AckMode::Partial is not the twin's default: the panel
  // asked for more and there is none.
  Rig r;
  r.up();
  r.d.setSelfAck(false);
  r.link.setAutoAck(true);
  r.link.setAutoAckPartials(500);      // never DONE

  uint8_t p[22];
  fill(p, sizeof(p));
  const TxTicket t = r.d.enqueue(0x151, p, sizeof(p));
  TEST_ASSERT_NOT_EQUAL(kNoTicket, t);
  pumpUntilIdle(r.d);

  ASSERT_RESULT(SendFailed, r.d.lastResult());
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(3, r.link.sentCount(), "all three frames still went out");
}

void test_an_unrecognised_ack_byte_fails_the_job(void) {
  Rig r;
  r.up();
  r.d.setSelfAck(false);

  uint8_t p[22];
  fill(p, sizeof(p));
  const TxTicket t = r.d.enqueue(0x151, p, sizeof(p));
  r.d.poll();                                   // frame 0 out, WaitAck
  TEST_ASSERT_TRUE(r.d.busy());

  // 0x7F is neither DONE (0x74) nor PARTIAL (30 01 00). The trailing bytes are the panel's
  // 0xA3 filler and are NOT inspected — nothing anywhere may match on a received filler.
  r.link.inject(affatest::mk(0x551, {0x7F, 0x00, 0x00, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3}));
  r.d.poll();
  ASSERT_RESULT(SendFailed, r.d.lastResult());
  TEST_ASSERT_EQUAL_UINT16(t, r.d.lastTicket());
}

void test_an_ack_for_a_function_that_is_not_waiting_is_dropped(void) {
  // What stops a stale late ACK from completing the wrong ticket.
  Rig r;
  r.up();
  r.d.setSelfAck(false);

  uint8_t p[22];
  fill(p, sizeof(p));
  const TxTicket t = r.d.enqueue(0x151, p, sizeof(p));
  r.d.poll();

  r.link.inject(affatest::mk(0x5F1, {0x74, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3}));
  r.d.poll();
  TEST_ASSERT_TRUE_MESSAGE(r.d.busy(), "an ACK on 0x1F1's reply id must not credit 0x151");
  TEST_ASSERT_NOT_EQUAL(t, r.d.lastTicket());
}

// ---------------------------------------------------------------------------
// The fence between isotp::fragment() and the transmit FSM
// ---------------------------------------------------------------------------

void test_fragment_matches_the_transmit_fsm_for_every_length(void) {
  // proto/IsoTp.h states the layout once and AffaDisplayBase::pumpTx() builds it inline;
  // the core deliberately does not call into proto/. This is the test that stops the two
  // from drifting — docs/API.md §2.13 names it as the fence and it was not written.
  Rig r;
  r.up();

  uint8_t payload[AFFA_MAX_PAYLOAD];
  fill(payload, sizeof(payload));

  Frame want[isotp::frameCount(AFFA_MAX_PAYLOAD)];
  char msg[96];

  for (uint8_t len = 1; len <= AFFA_MAX_PAYLOAD; ++len) {
    const uint8_t n = isotp::fragment(0x151, payload, len, carminat::kFiller,
                                      want, static_cast<uint8_t>(sizeof(want) / sizeof(want[0])));
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(isotp::frameCount(len), n, "frameCount() disagrees");

    TEST_ASSERT_NOT_EQUAL(kNoTicket, r.d.enqueue(0x151, payload, len));
    pumpUntilIdle(r.d);

    for (uint8_t i = 0; i < n; ++i) {
      Frame got;
      std::snprintf(msg, sizeof(msg), "len %u, frame %u", unsigned(len), unsigned(i));
      TEST_ASSERT_TRUE_MESSAGE(r.link.takeSent(got), msg);
      TEST_ASSERT_EQUAL_HEX32_MESSAGE(want[i].id, got.id, msg);
      TEST_ASSERT_EQUAL_UINT8_MESSAGE(want[i].len, got.len, msg);
      TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(want[i].data, got.data, 8, msg);
    }
    std::snprintf(msg, sizeof(msg), "len %u: FSM emitted more frames than fragment()",
                  unsigned(len));
    Frame extra;
    TEST_ASSERT_FALSE_MESSAGE(r.link.takeSent(extra), msg);
  }
}

void test_the_continuation_counter_wraps_rather_than_reaching_0x30(void) {
  // Legacy wrote `0x20 + num`, which produces 0x30 — the ISO-TP flow-control PCI — at
  // num == 16. Nothing in our repertoire is that long, so the two are byte-identical
  // today; the wrapping form is what the OEM head unit uses on its 302-byte message.
  TEST_ASSERT_EQUAL_HEX8(0x21, isoTpCf(1));
  TEST_ASSERT_EQUAL_HEX8(0x2F, isoTpCf(15));
  TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x20, isoTpCf(16), "the counter WRAPS; it never reaches 0x30");
  TEST_ASSERT_EQUAL_HEX8(0x21, isoTpCf(17));

  TEST_ASSERT_EQUAL_UINT8(1, isoTpFrameCount(1));
  TEST_ASSERT_EQUAL_UINT8(1, isoTpFrameCount(8));
  TEST_ASSERT_EQUAL_UINT8(2, isoTpFrameCount(9));
  TEST_ASSERT_EQUAL_UINT8(16, isoTpFrameCount(113));
}

void test_the_reassembler_stops_at_the_ceiling_rather_than_wrapping(void) {
  // A wrapped reassembler decodes a plausible-looking WRONG screen, which is the one
  // failure mode a semantic oracle must never have.
  isotp::Reassembler ra;
  Frame f;
  f.id = 0x151;
  f.len = 8;
  f.data[0] = 0x10;
  TEST_ASSERT_TRUE(ra.onFrame(f));

  for (int i = 0; i < 40; ++i) {                 // far past 113 bytes
    f.data[0] = isoTpCf(static_cast<uint8_t>(i + 1));
    ra.onFrame(f);
  }
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(AFFA_MAX_PAYLOAD, ra.len(), "appends stop at the ceiling");

  // A continuation with no first frame in front of it is noise from some other transfer
  // and must not be appended to the previous message.
  ra.reset();
  f.data[0] = 0x21;
  TEST_ASSERT_FALSE_MESSAGE(ra.onFrame(f), "an orphan continuation is not consumed");
  TEST_ASSERT_EQUAL_UINT8(0, ra.len());
}

// ---------------------------------------------------------------------------

void setUp(void) {}
void tearDown(void) {}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_113_bytes_is_16_frames_ending_at_PCI_2F);
  RUN_TEST(test_114_bytes_is_refused_with_nothing_on_the_wire);
  RUN_TEST(test_showConfirmBox_at_exactly_113_still_succeeds);
  RUN_TEST(test_done_while_bytes_remain_completes_with_Ok);
  RUN_TEST(test_partial_after_the_last_frame_is_SendFailed);
  RUN_TEST(test_an_unrecognised_ack_byte_fails_the_job);
  RUN_TEST(test_an_ack_for_a_function_that_is_not_waiting_is_dropped);
  RUN_TEST(test_fragment_matches_the_transmit_fsm_for_every_length);
  RUN_TEST(test_the_continuation_counter_wraps_rather_than_reaching_0x30);
  RUN_TEST(test_the_reassembler_stops_at_the_ceiling_rather_than_wrapping);
  return UNITY_END();
}
