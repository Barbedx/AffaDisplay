// Key decode, from the wire and back onto it.
//
// The three things that are load-bearing rather than tidy:
//   * the `03 89` guard — 0x1C1 is NOT a key-only channel, and a decoder without it
//     invents keys 0x640F and 0x3030 out of real panel traffic;
//   * the encoder exemption — masking 0x0141 rewrites it to 0x0101, i.e. every wheel-down
//     detent reported as a wheel-up;
//   * the 0x01C1 collision — a HELD detent has no distinguishable wire representation at
//     all, in either direction. Never build a feature on one.
//
// The key TABLE is [REF]-attested (affa3.h) and the encoder exemption is provable. What is
// NOT established is that the table is COMPLETE, so an unrecognised code must arrive with
// its raw value rather than be dropped or coerced onto a known key.

#include "../affa_test_support.h"

#include "carminat/CarminatDisplay.h"
#include "carminat/CarminatConstants.h"

using namespace affa;
using affatest::mk;
using affatest::drain;
using affatest::expectFrames;

namespace {

int     g_keys = 0;
Key     g_key  = Key::Load;
KeyEdge g_edge = KeyEdge::Click;

void countKey(Key k, KeyEdge e, void*) { ++g_keys; g_key = k; g_edge = e; }

struct Rig {
  LoopbackLink<256> link;
  affatest::FakeClock clk;
  CarminatDisplay d;
  Rig() : d(link, clk) {}

  void up() {
    d.begin();
    affatest::completeCarminatAuth(d, link, clk);
    d.onKey(&countKey, nullptr);
    drain(link);
    g_keys = 0;
  }

  // Injects `03 89 hi lo` on the key id and pumps one poll.
  void wireKey(uint8_t hi, uint8_t lo) {
    // Bytes 4..7 are the panel's 0xA3 filler and are DON'T CARE. Injecting them as 0xA3
    // rather than 0x00 is deliberate: a decoder that ever started reading them fails here.
    link.inject(mk(0x1C1, {0x03, 0x89, hi, lo, 0xA3, 0xA3, 0xA3, 0xA3}));
    d.poll();
  }
};

}  // namespace

// ---------------------------------------------------------------------------
// The guard
// ---------------------------------------------------------------------------

void test_the_03_89_guard_rejects_real_panel_traffic(void) {
  Rig r;
  r.up();

  // All three are [CAP-VERBATIM] panel -> radio payloads on the KEY id. None of them is a
  // key frame, and a decoder that read data[2..3] regardless would manufacture 0x640F and
  // 0x3030 out of the second and third.
  r.link.inject(mk(0x1C1, {0x70, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3}));
  r.link.inject(mk(0x1C1, {0x02, 0x64, 0x0F, 0xA3, 0xA3, 0xA3, 0xA3, 0xA3}));
  r.link.inject(mk(0x1C1, {0x05, 0x63, 0x30, 0x30, 0x33, 0x37, 0xA3, 0xA3}));
  r.link.inject(mk(0x1C1, {0x03, 0x88, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));   // wrong b1
  r.link.inject(mk(0x1C1, {0x04, 0x89, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));   // wrong b0
  r.d.poll();

  TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_keys, "no phantom keys from non-key traffic");

  // The panel STILL gets its generic ACK for each of them — the auto-ACK precedes the
  // validity check, which is the legacy order and what the panel expects.
  Frame f;
  int acks = 0;
  while (r.link.takeSent(f)) {
    if (f.id == 0x5C1 && f.data[0] == 0x74) ++acks;
  }
  TEST_ASSERT_EQUAL_INT_MESSAGE(5, acks, "Carminat acknowledges 0x1C1 before validating it");
}

void test_a_short_key_frame_is_not_decoded(void) {
  Rig r;
  r.up();
  Frame f = mk(0x1C1, {0x03, 0x89, 0x01, 0x41, 0, 0, 0, 0});
  f.len = 3;                        // data[3] is present in the struct but was not sent
  r.link.inject(f);
  r.d.poll();
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_keys, "len >= 4 before reading the low byte");
}

// ---------------------------------------------------------------------------
// The mask
// ---------------------------------------------------------------------------

void test_hold_is_either_mask_bit(void) {
  // KEY_HOLD_MASK is 0x80 | 0x40 and the test is `!= 0`, not `== 0xC0`. Our own emulator
  // sets both; nothing guarantees the panel does.
  struct V { uint8_t hi, lo; uint16_t key; bool hold; };
  static const V kV[] = {
      {0x00, 0x00, 0x0000, false},
      {0x00, 0x40, 0x0000, true},    // low bit of the mask alone
      {0x00, 0x80, 0x0000, true},    // high bit of the mask alone
      {0x00, 0xC0, 0x0000, true},    // both, which is what we transmit
      {0x00, 0x05, 0x0005, false},
      {0x00, 0xC5, 0x0005, true},
  };

  Rig r;
  r.up();
  for (const V& v : kV) {
    g_keys = 0;
    r.wireKey(v.hi, v.lo);
    TEST_ASSERT_EQUAL_INT(1, g_keys);
    TEST_ASSERT_EQUAL_HEX16(v.key, static_cast<uint16_t>(g_key));
    TEST_ASSERT_EQUAL_INT(v.hold ? 1 : 0, g_edge == KeyEdge::Hold ? 1 : 0);
  }
}

void test_the_mask_clears_the_low_byte_only(void) {
  // `raw & 0xFF3F`, written out. `raw & ~KEY_HOLD_MASK` is correct only by accident of
  // integer promotion (~(uint8_t)0xC0 is 0xFFFFFF3F, so the high byte survives).
  TEST_ASSERT_EQUAL_HEX16(0xFF3F, kKeyCodeMask);
  TEST_ASSERT_EQUAL_HEX8(0xC0, kKeyHoldMask);

  Rig r;
  r.up();
  g_keys = 0;
  r.wireKey(0x02, 0xC7);            // high byte 0x02 must survive the mask
  TEST_ASSERT_EQUAL_INT(1, g_keys);
  TEST_ASSERT_EQUAL_HEX16_MESSAGE(0x0207, static_cast<uint16_t>(g_key),
                                  "the high byte is not touched by the hold mask");
  TEST_ASSERT_TRUE(g_edge == KeyEdge::Hold);
}

// ---------------------------------------------------------------------------
// The encoder exemption and the 0x01C1 collision
// ---------------------------------------------------------------------------

void test_the_detent_codes_are_exempt_from_hold_masking(void) {
  Rig r;
  r.up();

  g_keys = 0;
  r.wireKey(0x01, 0x01);
  TEST_ASSERT_EQUAL_INT(1, g_keys);
  TEST_ASSERT_EQUAL_HEX16(0x0101, static_cast<uint16_t>(g_key));
  TEST_ASSERT_TRUE_MESSAGE(g_edge == KeyEdge::Click, "a bare detent is never a hold");

  g_keys = 0;
  r.wireKey(0x01, 0x41);
  TEST_ASSERT_EQUAL_INT(1, g_keys);
  TEST_ASSERT_EQUAL_HEX16_MESSAGE(0x0141, static_cast<uint16_t>(g_key),
                                  "masking 0x0141 would report every wheel-DOWN as a wheel-UP");
  TEST_ASSERT_TRUE(g_edge == KeyEdge::Click);
}

void test_a_held_detent_is_unrecoverable_by_design(void) {
  // 0x40 is SIMULTANEOUSLY RollDown's direction bit and half the hold mask, so both held
  // detents collapse onto one value that is not exempt and masks back to RollUp.
  TEST_ASSERT_EQUAL_HEX16(0x01C1, kKeyRollUp | kKeyHoldMask);
  TEST_ASSERT_EQUAL_HEX16(0x01C1, kKeyRollDown | kKeyHoldMask);
  TEST_ASSERT_EQUAL_HEX16_MESSAGE(kKeyRollUp | kKeyHoldMask, kKeyRollDown | kKeyHoldMask,
                                  "the two held detents are the SAME code on the wire");

  Rig r;
  r.up();
  g_keys = 0;
  r.wireKey(0x01, 0xC1);
  TEST_ASSERT_EQUAL_INT(1, g_keys);
  TEST_ASSERT_EQUAL_HEX16_MESSAGE(0x0101, static_cast<uint16_t>(g_key),
                                  "decoded as RollUp for legacy compatibility");
  TEST_ASSERT_TRUE(g_edge == KeyEdge::Hold);

  // And the ENCODER refuses rather than downgrading: transmitting the click form would
  // give a fine step where the caller asked for a coarse one, which is a wrong screen the
  // caller cannot detect.
  ASSERT_RESULT(NotSupported, r.d.pressKey(Key::RollUp, KeyEdge::Hold, KeySource::Wire));
  ASSERT_RESULT(NotSupported, r.d.pressKey(Key::RollDown, KeyEdge::Hold, KeySource::Wire));
  drain(r.link);
}

// ---------------------------------------------------------------------------
// Unrecognised codes
// ---------------------------------------------------------------------------

void test_an_unrecognised_code_arrives_with_its_raw_value(void) {
  // The table is attested, not proven complete. Dropping an unknown code would make a
  // panel variant look like a dead wheel; coercing it onto a known Key would make it look
  // like the wrong button. Both are worse than delivering the number.
  //
  // NOTE: affa::Key has no `Unknown` enumerator — it is a uint16_t-backed enum class whose
  // value IS the wire code, so an unrecognised code is delivered as an unnamed Key holding
  // exactly that code. See the openIssues note.
  struct V { uint8_t hi, lo; uint16_t raw; bool hold; };
  static const V kV[] = {
      {0x00, 0x07, 0x0007, false},   // not in the [REF] table at all
      {0x00, 0xC7, 0x0007, true},    // ... and it still masks and holds correctly
      {0x00, 0x3F, 0x003F, false},   // every bit the mask does NOT clear
      {0x12, 0x34, 0x1234, false},   // a whole foreign high byte
      {0x01, 0x02, 0x0102, false},   // adjacent to a detent, and NOT exempt
  };

  Rig r;
  r.up();
  for (const V& v : kV) {
    g_keys = 0;
    g_key  = Key::Pause;
    r.wireKey(v.hi, v.lo);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_keys, "an unrecognised code must NOT be dropped");
    TEST_ASSERT_EQUAL_HEX16_MESSAGE(v.raw, static_cast<uint16_t>(g_key),
                                    "it arrives carrying its raw value");
    TEST_ASSERT_EQUAL_INT(v.hold ? 1 : 0, g_edge == KeyEdge::Hold ? 1 : 0);
  }
}

// ---------------------------------------------------------------------------
// The encoder
// ---------------------------------------------------------------------------

void test_the_transmitted_key_frame_is_byte_exact(void) {
  Rig r;
  r.up();

  // The terminating gesture of the legacy sendPasswordSequence(): emulateKey(Load, true).
  // examples/08_radio_mitm expresses it as pressKey(Load, Hold, Wire), and this is the
  // assertion that the two are byte-identical.
  ASSERT_RESULT(Ok, r.d.pressKey(Key::Load, KeyEdge::Hold, KeySource::Wire));
  static const Frame kHoldLoad[] = {
      {0x1C1, 8, {0x03, 0x89, 0x00, 0xC0, 0x00, 0x00, 0x00, 0x00}, false},
  };
  expectFrames(r.link, kHoldLoad, 1, "pressKey(Load, Hold, Wire)");

  // Bytes 4..7 are LITERAL ZERO and not packetFiller() — that is what the capture of our
  // own emulated key shows. On UpdateList (filler 0x81) they would still be zero.
  ASSERT_RESULT(Ok, r.d.pressKey(Key::RollDown, KeyEdge::Click, KeySource::Wire));
  static const Frame kRollDown[] = {
      {0x1C1, 8, {0x03, 0x89, 0x01, 0x41, 0x00, 0x00, 0x00, 0x00}, false},
  };
  expectFrames(r.link, kRollDown, 1, "pressKey(RollDown, Click, Wire) — no hold bits added");
}

void test_a_decoded_key_round_trips_through_the_encoder(void) {
  // Everything the encoder can transmit must decode back to what was asked for. The two
  // held detents are excluded because they provably cannot — see the collision test.
  struct V { Key k; KeyEdge e; };
  static const V kV[] = {
      {Key::Load, KeyEdge::Click},   {Key::Load, KeyEdge::Hold},
      {Key::SrcNext, KeyEdge::Click},{Key::SrcNext, KeyEdge::Hold},
      {Key::SrcPrev, KeyEdge::Click},{Key::VolUp, KeyEdge::Click},
      {Key::VolDown, KeyEdge::Click},{Key::Pause, KeyEdge::Click},
      {Key::Pause, KeyEdge::Hold},   {Key::RollUp, KeyEdge::Click},
      {Key::RollDown, KeyEdge::Click},
  };

  Rig r;
  r.up();
  for (const V& v : kV) {
    ASSERT_RESULT(Ok, r.d.pressKey(v.k, v.e, KeySource::Wire));
    Frame f;
    TEST_ASSERT_TRUE(r.link.takeSent(f));

    g_keys = 0;
    r.link.inject(mk(0x1C1, {f.data[0], f.data[1], f.data[2], f.data[3], 0xA3, 0xA3, 0xA3, 0xA3}));
    r.d.poll();
    TEST_ASSERT_EQUAL_INT(1, g_keys);
    TEST_ASSERT_EQUAL_HEX16(static_cast<uint16_t>(v.k), static_cast<uint16_t>(g_key));
    TEST_ASSERT_EQUAL_INT(v.e == KeyEdge::Hold ? 1 : 0, g_edge == KeyEdge::Hold ? 1 : 0);
    drain(r.link);
  }
}

// ---------------------------------------------------------------------------

void setUp(void) {}
void tearDown(void) {}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_the_03_89_guard_rejects_real_panel_traffic);
  RUN_TEST(test_a_short_key_frame_is_not_decoded);
  RUN_TEST(test_hold_is_either_mask_bit);
  RUN_TEST(test_the_mask_clears_the_low_byte_only);
  RUN_TEST(test_the_detent_codes_are_exempt_from_hold_masking);
  RUN_TEST(test_a_held_detent_is_unrecoverable_by_design);
  RUN_TEST(test_an_unrecognised_code_arrives_with_its_raw_value);
  RUN_TEST(test_the_transmitted_key_frame_is_byte_exact);
  RUN_TEST(test_a_decoded_key_round_trips_through_the_encoder);
  return UNITY_END();
}
