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
    // Self-ACK BEFORE the opening: on this family the 0x70 registrations leave with the
    // final hello frame, so a rig that arms the emulator afterwards leaves the first
    // registration parked in WaitAck for ever.
    d.setSelfAck(true);
    affatest::completeCarminatAuth(d, link, clk);
    (void)d.setPower(true);
    affatest::settleCarminatRegistration(d, clk);
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

// A 12-bit first frame must open a message. ISO 15765-2 puts the top four bits of FF_DL
// in the PCI's low nibble, so anything longer than 255 bytes arrives as 0x11..0x1F rather
// than 0x10. Both of OUR builders are short enough to always emit 0x10, which is why an
// exact `data[0] == 0x10` test survived — but AFFA_ENABLE_ISOTP_RX exists to decode
// SOMEBODY ELSE'S traffic, and the OEM head unit's 302-byte 0x1F1 screen opens with
// `11 2E 21 0B 00 25 41 42`. Against the exact test that first frame was refused, _active
// stayed false, and all 43 continuations were refused behind it: the message decoded to
// nothing while the documentation claimed the case was handled.
void test_a_12_bit_first_frame_opens_a_message(void) {
  isotp::Reassembler ra;
  Frame f;
  f.id  = 0x1F1;
  f.len = 8;

  // The real OEM first frame, verbatim.
  const uint8_t ff[8] = {0x11, 0x2E, 0x21, 0x0B, 0x00, 0x25, 0x41, 0x42};
  for (uint8_t i = 0; i < 8; ++i) f.data[i] = ff[i];

  TEST_ASSERT_TRUE_MESSAGE(ra.onFrame(f), "0x11 must be accepted as a first frame");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(8, ra.len(), "the PCI byte is payload byte 0");
  TEST_ASSERT_EQUAL_UINT8(0x11, ra.buffer()[0]);

  // And the continuation behind it is now accepted, which is the half that actually
  // mattered: refusing the first frame silently discarded the whole 43-frame message.
  f.data[0] = isoTpCf(1);
  TEST_ASSERT_TRUE_MESSAGE(ra.onFrame(f), "continuations follow a 12-bit first frame");
  TEST_ASSERT_EQUAL_UINT8(15, ra.len());

  // The whole 0x11..0x1F range opens a message, not just 0x11.
  for (uint8_t lo = 0x1; lo <= 0xF; ++lo) {
    isotp::Reassembler r2;
    Frame g = f;
    g.data[0] = static_cast<uint8_t>(0x10 | lo);
    TEST_ASSERT_TRUE(r2.onFrame(g));
  }

  // Why widening the test is safe, for the record: a single frame's PCI IS its length,
  // and every single-frame payload in either family's repertoire is at most 7 bytes, so
  // payload byte 0 can never land in 0x11..0x1F and be mistaken for a first frame. There
  // is nothing here to assert — the guarantee is a property of the repertoire, and the
  // golden vectors in test_carminat_wire and test_updatelist_wire are what pin it.
}

// ---------------------------------------------------------------------------

void setUp(void) {}
void tearDown(void) {}


// ---------------------------------------------------------------------------
// enqueueExternal() — the zero-copy path past the 113-byte ceiling
// ---------------------------------------------------------------------------
// The OEM head unit's 0x1F1 nav screen is 304 wire bytes: 2 PCI + 302 declared. That needs
// 43 continuation frames, so the counter must wrap TWICE, and it is the first thing in this
// library to exercise a 12-bit first-frame length on the transmit side.

void test_the_OEM_nav_screen_is_44_frames_with_a_twice_wrapped_counter(void) {
  Rig r;
  r.up();

  // 2 PCI + 302 payload, exactly as the capture has it.
  static uint8_t msg[304];
  msg[0] = 0x11;                 // FF, high nibble of 302
  msg[1] = 0x2E;                 // low byte of 302
  for (uint16_t i = 2; i < sizeof(msg); ++i) msg[i] = static_cast<uint8_t>(i);

  const TxTicket t = r.d.enqueueExternal(0x1F1, msg, sizeof(msg));
  TEST_ASSERT_NOT_EQUAL_MESSAGE(kNoTicket, t, "304 bytes is inside AFFA_MAX_EXTERNAL_PAYLOAD");
  pumpUntilIdle(r.d);
  ASSERT_RESULT(Ok, r.d.lastResult());

  Frame f;
  int n = 0;
  uint8_t pcis[64] = {0};
  while (r.link.takeSent(f)) {
    if (n == 0) {
      TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x11, f.data[0], "the caller owns the first-frame PCI");
      TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x2E, f.data[1], "302, as the OEM declares it");
    } else if (n < 64) {
      pcis[n] = f.data[0];
    }
    ++n;
  }
  TEST_ASSERT_EQUAL_INT_MESSAGE(44, n, "1 first frame + 43 continuations");
  TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x21, pcis[1],  "continuations start at 0x21");
  TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x2F, pcis[15], "...run to 0x2F...");
  TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x20, pcis[16], "...and WRAP to 0x20, never 0x30");
  TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x20, pcis[32], "wrapping a second time");
  TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x2B, pcis[43], "the capture's last PCI");
}

void test_external_payload_is_borrowed_not_copied(void) {
  Rig r;
  r.up();

  // Mutating the caller's buffer AFTER enqueue must change what goes on the wire. That is
  // the contract, and asserting it is what stops a future 'defensive' memcpy from silently
  // reinstating the 1.1 kB this path exists to avoid.
  static uint8_t msg[16];
  for (uint8_t i = 0; i < sizeof(msg); ++i) msg[i] = 0xAA;
  const TxTicket t = r.d.enqueueExternal(0x151, msg, sizeof(msg));
  TEST_ASSERT_NOT_EQUAL(kNoTicket, t);
  msg[0] = 0x5A;
  pumpUntilIdle(r.d);

  Frame f;
  TEST_ASSERT_TRUE(r.link.takeSent(f));
  TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x5A, f.data[0], "the library reads the caller's storage");
}

void test_external_refuses_coalescing_and_reassert(void) {
  Rig r;
  r.up();
  static uint8_t msg[16] = {0};

  // The SLOT is what is refused. TxOptions::coalesce defaults to true, so a check on the
  // flag would reject every ordinary call — this asserts the distinction stays that way.
  TxOptions co;
  co.slot = RenderSlot::Text;
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(kNoTicket, r.d.enqueueExternal(0x151, msg, sizeof(msg), co),
                                   "coalescing would re-copy into a slot that owns no storage");
  ASSERT_RESULT(BadArgument, r.d.lastResult());

  TxOptions ra;
  ra.reassertAfterSession = true;
  ra.slot                 = RenderSlot::Control;
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(kNoTicket, r.d.enqueueExternal(0x151, msg, sizeof(msg), ra),
                                   "the control cache is a fixed AFFA_MAX_PAYLOAD copy");
  ASSERT_RESULT(BadArgument, r.d.lastResult());
}

void test_external_still_has_a_ceiling(void) {
  Rig r;
  r.up();
  static uint8_t msg[8] = {0};
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(
      kNoTicket, r.d.enqueueExternal(0x151, msg, AFFA_MAX_EXTERNAL_PAYLOAD + 1),
      "a borrowed pointer is still bounded");
  ASSERT_RESULT(TooLong, r.d.lastResult());
}


// ---------------------------------------------------------------------------
// enqueueSplit() — a copied header in front of a borrowed body
// ---------------------------------------------------------------------------

void test_split_frames_prefix_then_body_as_one_message(void) {
  Rig r;
  r.up();

  static uint8_t prefix[16];
  static uint8_t body[288];
  prefix[0] = 0x11; prefix[1] = 0x2E;
  for (uint8_t i = 2; i < sizeof(prefix); ++i) prefix[i] = static_cast<uint8_t>(0xA0 + i);
  for (uint16_t i = 0; i < sizeof(body); ++i)  body[i]  = static_cast<uint8_t>(i);

  const TxTicket t = r.d.enqueueSplit(0x1F1, prefix, sizeof(prefix), body, sizeof(body));
  TEST_ASSERT_NOT_EQUAL(kNoTicket, t);
  pumpUntilIdle(r.d);
  ASSERT_RESULT(Ok, r.d.lastResult());

  // Reassemble what actually went out and compare against prefix||body.
  static uint8_t got[320];
  uint16_t n = 0;
  Frame f;
  int frames = 0;
  while (r.link.takeSent(f)) {
    const int off = (frames == 0) ? 0 : 1;          // continuations carry a PCI byte
    for (int i = off; i < 8 && n < sizeof(prefix) + sizeof(body); ++i) got[n++] = f.data[i];
    ++frames;
  }
  TEST_ASSERT_EQUAL_INT_MESSAGE(44, frames, "16 + 288 = 304 bytes is 1 + 43 frames");
  TEST_ASSERT_EQUAL_UINT16(sizeof(prefix) + sizeof(body), n);
  TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(prefix, got, sizeof(prefix),
                                       "the copied header leads");
  TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(body, got + sizeof(prefix), sizeof(body),
                                       "the borrowed body follows, unbroken across the seam");
}

void test_split_borrows_the_body_but_copies_the_prefix(void) {
  Rig r;
  r.up();
  static uint8_t prefix[8] = {0x11, 0x00, 1, 2, 3, 4, 5, 6};
  static uint8_t body[16];
  for (uint8_t i = 0; i < sizeof(body); ++i) body[i] = 0xAA;

  const TxTicket t = r.d.enqueueSplit(0x151, prefix, sizeof(prefix), body, sizeof(body));
  TEST_ASSERT_NOT_EQUAL(kNoTicket, t);
  prefix[2] = 0x99;      // copied: must NOT be seen
  body[0]   = 0x5A;      // borrowed: MUST be seen
  pumpUntilIdle(r.d);

  Frame f;
  TEST_ASSERT_TRUE(r.link.takeSent(f));
  TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x01, f.data[2], "the prefix was copied at enqueue time");
  TEST_ASSERT_TRUE(r.link.takeSent(f));
  TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x5A, f.data[1], "the body is read from caller storage");
}

void test_split_rejects_a_render_slot(void) {
  Rig r;
  r.up();
  static uint8_t prefix[4] = {0}, body[8] = {0};
  TxOptions o;
  o.slot = RenderSlot::Text;
  TEST_ASSERT_EQUAL_UINT16(kNoTicket,
                           r.d.enqueueSplit(0x151, prefix, sizeof(prefix), body, sizeof(body), o));
  ASSERT_RESULT(BadArgument, r.d.lastResult());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_12_bit_first_frame_opens_a_message);
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
  RUN_TEST(test_the_OEM_nav_screen_is_44_frames_with_a_twice_wrapped_counter);
  RUN_TEST(test_external_payload_is_borrowed_not_copied);
  RUN_TEST(test_external_refuses_coalescing_and_reassert);
  RUN_TEST(test_external_still_has_a_ceiling);
  RUN_TEST(test_split_frames_prefix_then_body_as_one_message);
  RUN_TEST(test_split_borrows_the_body_but_copies_the_prefix);
  RUN_TEST(test_split_rejects_a_render_slot);
  return UNITY_END();
}
